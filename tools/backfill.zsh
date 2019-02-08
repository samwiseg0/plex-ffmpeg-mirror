#!/bin/zsh
# Backfill the mirror from a local archive of old Plex Media Server FreeBSD
# tarballs. Follows the same process as tools/fetch-plex-ffmpeg.zsh, but reads
# each release's bundled Resources/LICENSE from a local tarball instead of the
# Plex update API, and replays the history one version at a time.
#
# For each version (oldest -> newest) it:
#   1. extracts the LICENSE from the local PMS tarball,
#   2. parses the ffmpeg GPL source URL(s),
#   3. downloads + unpacks the source into plex-ffmpeg-source/,
#   4. de-duplicates identical transcoders by real CONTENT hash (so two
#      genuinely different trees can never collapse, and identical ones do),
#   5. writes latest.version,
#   6. makes ONE commit + annotated tag per version, backdated to the tarball's
#      modification time (best-effort proxy for the release date).
#
# It works on a dedicated branch (default: "backfill"), branched from the
# current branch, so `main` is left untouched. Nothing is pushed and no GitHub
# Releases are created unless you pass --publish.
#
# Usage:
#   tools/backfill.zsh <archive_dir> [--branch NAME] [--limit N] [--publish]
#
# <archive_dir> holds one subfolder per version, each containing
#   PlexMediaServer-<version>-FreeBSD-amd64.tar.bz2

emulate -L zsh
setopt extended_glob null_glob pipe_fail

typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr REPO_PATH="${SCRIPT_PATH:h}"
typeset -gr OUT_PATH="${REPO_PATH}/plex-ffmpeg-source"
# Plex's LICENSE attributes its single ffmpeg GPL source to "Plex Transcoder"
# (verified across every released version), so PlexTranscoder is the one
# canonical folder. A second distinct source (never seen) would go to
# PlexTranscoder-2.
typeset -gr TC_NAME="PlexTranscoder"
typeset -gr TC_PATH="${OUT_PATH}/${TC_NAME}"
typeset -gr VERSION_FILE="${REPO_PATH}/latest.version"
typeset -gr WORK_PATH="${REPO_PATH}/run"

typeset -g ARCHIVE_DIR=""
typeset -g BRANCH="backfill"
typeset -gi LIMIT=0
typeset -gi PUBLISH=0

die() { print -r -- "FATAL: $1" >&2; exit 1; }
make_dir() { [[ -d "$1" ]] || mkdir -p "$1" || die "mkdir $1"; }

# Pick whichever SHA-256 tool exists (Linux: sha256sum, macOS: shasum -a 256).
sha256_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi
}
sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi
}

# Deterministic content hash of a directory tree (relative paths + file bytes).
# Sensitive to both content and structure, so different trees never collide.
tree_content_hash() {
    local dir="$1"
    [[ -d "$dir" ]] || { print -r -- "MISSING"; return; }
    ( cd "$dir" && find . -type f -print | LC_ALL=C sort | while IFS= read -r f; do
        sha256_file "$f"
      done ) | sha256_stdin | awk '{print $1}'
}

# Epoch mtime of a file (macOS then GNU stat).
file_mtime() {
    stat -f %m "$1" 2>/dev/null || stat -c %Y "$1" 2>/dev/null
}

# Extract the top-level LICENSE from a PMS tarball to stdout. Stops after the
# first matching member (bzip2 isn't seekable, so without this tar would keep
# decompressing the whole ~88MB stream). bsdtar: --fast-read; GNU: --occurrence.
typeset -g _IS_BSDTAR=""
license_of() {
    [[ -n "$_IS_BSDTAR" ]] || { tar --version 2>/dev/null | grep -qi bsdtar && _IS_BSDTAR=1 || _IS_BSDTAR=0; }
    if [[ "$_IS_BSDTAR" == 1 ]]; then
        tar -xOf "$1" --fast-read '*/Resources/LICENSE' 2>/dev/null
    else
        tar -xO --occurrence=1 --wildcards -f "$1" '*/Resources/LICENSE' 2>/dev/null
    fi
}

# download_unpack <url> <dest_repo_path> -> echoes the original source dir name
download_unpack() {
    local url="$1" dest="$2"
    local tmp_archive tmp_dir
    tmp_archive="$(mktemp)" || return 1
    if ! curl --location --fail --silent --show-error --output "$tmp_archive" "$url"; then
        print -r -- "  download failed: $url" >&2
        rm -f "$tmp_archive"; return 1
    fi
    tmp_dir="$(mktemp -d)" || { rm -f "$tmp_archive"; return 1; }
    if ! tar xf "$tmp_archive" -C "$tmp_dir" 2>/dev/null; then
        print -r -- "  extract failed: $url" >&2
        rm -rf "$tmp_archive" "$tmp_dir"; return 1
    fi
    rm -f "$tmp_archive"
    local -a extracted=( "$tmp_dir"/*(DN) )
    local source_name
    [[ -d "$dest" ]] && rm -rf "$dest"
    make_dir "${dest:h}"
    if (( ${#extracted[@]} == 1 )) && [[ -d "${extracted[1]}" ]]; then
        source_name="${extracted[1]:t}"
        mv "${extracted[1]}" "$dest"
    else
        source_name="${url:t}"
        make_dir "$dest"
        mv "$tmp_dir"/*(DN) "$dest"/
    fi
    rm -rf "$tmp_dir"
    print -r -- "$source_name"
    return 0
}

# Parse each "... contains code from ffmpeg: <URL>" line from a tarball's
# LICENSE into parallel arrays SRC_FOLDERS / SRC_URLS, mapping the attribution
# to a folder: "Plex Transcoder" -> PlexTranscoder, "Plex Media Scanner and Plex
# Media Server" -> PlexMediaServer. Keying on the "from ffmpeg:" attribution
# naturally ignores sibling non-ffmpeg URLs (e.g. realtek-openmax).
typeset -ga SRC_FOLDERS SRC_URLS
parse_sources() {
    SRC_FOLDERS=(); SRC_URLS=()
    local line url prefix folder
    while IFS= read -r line; do
        url="${line##*ffmpeg:}"
        url="${url#"${url%%[![:space:]]*}"}"   # strip leading whitespace
        url="${url%%[[:space:]]*}"              # cut at first trailing whitespace
        [[ "$url" == http://* || "$url" == https://* ]] || continue
        prefix="${(L)line}"
        if [[ "$prefix" == *transcoder* ]]; then
            folder="PlexTranscoder"
        elif [[ "$prefix" == *scanner* || "$prefix" == *"media server"* ]]; then
            folder="PlexMediaServer"
        else
            folder="PlexTranscoder"
        fi
        SRC_FOLDERS+=( "$folder" )
        SRC_URLS+=( "$url" )
    done < <(license_of "$1" | grep -iE 'from ffmpeg:[[:space:]]*https?://')
}

# Materialize the source(s) for one version into the repo. Returns non-zero if
# nothing could be downloaded (e.g. dead link). Reuses in-place folders when the
# URL set is unchanged from the previous version (avoids redundant downloads).
typeset -g LAST_URL_KEY=""
process_version() {
    local version="$1" tb="$2"
    parse_sources "$tb"
    (( ${#SRC_URLS[@]} )) || { print -r -- "  no ffmpeg URL; skipping"; return 1; }

    local url_key="${(j:|:)SRC_URLS}"
    typeset -A src_names

    if [[ "$url_key" == "$LAST_URL_KEY" && -d "$TC_PATH" ]]; then
        # Same source set as the previous version: reuse the existing tree(s).
        local f
        for f in "${OUT_PATH}"/*(/N); do
            src_names[${f:t}]="$(<"${WORK_PATH}/.src-${f:t}" 2>/dev/null)"
        done
        print -r -- "  source unchanged from previous version (reused)"
    else
        rm -rf "${OUT_PATH}"/*(/N)
        rm -f "${WORK_PATH}/".src-*(N)
        local i ok=0 folder url dest name l
        for i in {1..${#SRC_URLS[@]}}; do
            folder="${SRC_FOLDERS[$i]}"
            url="${SRC_URLS[$i]}"
            dest="${OUT_PATH}/${folder}"
            if name="$(download_unpack "$url" "$dest")"; then
                src_names[$folder]="$name"
                ok=1
            fi
        done
        (( ok )) || { print -r -- "  no sources downloaded; skipping version"; return 1; }

        # Content-hash dedup: drop PlexMediaServer if byte-identical to
        # PlexTranscoder (modern releases list the same source for both).
        if [[ -n "${src_names[PlexTranscoder]:-}" && -n "${src_names[PlexMediaServer]:-}" ]]; then
            if [[ "$(tree_content_hash "$TC_PATH")" == "$(tree_content_hash "${OUT_PATH}/PlexMediaServer")" ]]; then
                print -r -- "  PlexMediaServer identical to PlexTranscoder; dropping"
                rm -rf "${OUT_PATH}/PlexMediaServer"
                unset 'src_names[PlexMediaServer]'
            fi
        fi
        # Cache source names for the in-place reuse fast path.
        for l in ${(k)src_names}; do
            print -r -- "${src_names[$l]}" > "${WORK_PATH}/.src-${l}"
        done
    fi

    LAST_URL_KEY="$url_key"
    {
        print -r -- "plex=${version}"
        local label
        for label in ${(ok)src_names}; do
            print -r -- "${label}=${src_names[$label]}"
        done
    } > "$VERSION_FILE"
    return 0
}

main() {
    while (( $# )); do
        case "$1" in
            --branch)  BRANCH="$2"; shift 2;;
            --limit)   LIMIT="$2";  shift 2;;
            --publish) PUBLISH=1;   shift;;
            -h|--help) sed -n '2,40p' "$0"; exit 0;;
            -*)        die "Unknown option: $1";;
            *)         ARCHIVE_DIR="$1"; shift;;
        esac
    done
    [[ -n "$ARCHIVE_DIR" && -d "$ARCHIVE_DIR" ]] || die "usage: backfill.zsh <archive_dir> [--branch NAME] [--limit N] [--publish]"

    cd "$REPO_PATH" || die "cannot cd to repo $REPO_PATH"
    [[ -d .git ]] || die "not a git repo: $REPO_PATH"
    [[ -z "$(git status --porcelain --untracked-files=no)" ]] || die "working tree has uncommitted tracked changes; commit or stash first"

    # Work on a dedicated branch so main is untouched.
    if git show-ref --verify --quiet "refs/heads/${BRANCH}"; then
        git checkout -q "$BRANCH" || die "checkout $BRANCH failed"
    else
        git checkout -q -b "$BRANCH" || die "create branch $BRANCH failed"
    fi
    print -r -- "On branch ${BRANCH} (main left untouched)."

    make_dir "$WORK_PATH"

    typeset -a versions
    versions=( ${(f)"$(ls -1 "$ARCHIVE_DIR" | sort -V)"} )

    local version dir tb mtime datestr
    integer count=0 made=0
    for version in $versions; do
        (( LIMIT > 0 && made >= LIMIT )) && break
        dir="$ARCHIVE_DIR/$version"
        [[ -d "$dir" ]] || continue
        local -a tbs=( "$dir"/PlexMediaServer-*-FreeBSD-amd64.tar.bz2 )
        (( ${#tbs[@]} )) || { print -r -- "=== $version === (no FreeBSD tarball; skipping)"; continue; }
        tb="${tbs[1]}"

        if git rev-parse -q --verify "refs/tags/v${version}" >/dev/null 2>&1; then
            print -r -- "=== $version === already tagged; skipping"
            continue
        fi

        print -r -- "=== $version ==="
        process_version "$version" "$tb" || continue

        git add -A
        if git diff --cached --quiet; then
            print -r -- "  no changes; skipping commit"
            continue
        fi
        mtime="$(file_mtime "$tb")"; [[ -n "$mtime" ]] || mtime="$(date +%s)"
        datestr="@${mtime}"
        GIT_AUTHOR_DATE="$datestr" GIT_COMMITTER_DATE="$datestr" \
            git commit -q -m "Backfill - Plex version ${version}"
        GIT_COMMITTER_DATE="$datestr" \
            git tag -a "v${version}" -m "Plex FFMPEG source for Plex version ${version}"
        print -r -- "  committed + tagged v${version}"
        (( made++ ))
        (( count++ ))
    done

    rm -rf "$WORK_PATH"

    print -r -- ""
    print -r -- "Staged ${made} new version(s) on branch ${BRANCH}."
    if (( PUBLISH )); then
        print -r -- "Pushing branch and tags..."
        git push -u origin "$BRANCH"
        git push origin --tags
        print -r -- "Creating GitHub Releases..."
        for version in $versions; do
            git rev-parse -q --verify "refs/tags/v${version}" >/dev/null 2>&1 || continue
            gh release view "v${version}" >/dev/null 2>&1 && continue
            gh release create "v${version}" --title "v${version}" \
                --notes "Mirrored Plex FFMPEG GPL source for Plex Media Server version \`${version}\`." \
                && print -r -- "  released v${version}"
        done
    else
        print -r -- "Review with:  git log --oneline --no-walk --tags --decorate"
        print -r -- "Publish with: tools/backfill.zsh '${ARCHIVE_DIR}' --branch ${BRANCH} --publish"
    fi
}

main "$@"
