#!/bin/zsh
# Fetch Plex's patched FFMPEG source and unpack it into a local folder.
#
# Plex publishes the download location of their patched FFMPEG in the LICENSE
# file bundled with Plex Media Server.  This script:
#   1. asks the Plex update API for the latest FreeBSD build (a plain tarball),
#   2. downloads + extracts it just to read its LICENSE file,
#   3. scrapes the ffmpeg source-archive URLs out of that LICENSE,
#   4. downloads each archive and unpacks it into an output folder.
#
# It does NOT touch git and does NOT upload anything; the GitHub Actions
# workflow is what commits the result.

typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr REPO_PATH="${SCRIPT_PATH:h}"
# Output folder for the unpacked source.  Override by passing a path as $1.
typeset -gr OUT_PATH="${1:-${REPO_PATH}/plex-ffmpeg-source}"
typeset -gr WORK_PATH="${REPO_PATH}/run"
typeset -gr PMS_PATH="${WORK_PATH}/pms"
# The Plex LICENSE attributes its single ffmpeg GPL source to "Plex Transcoder"
# (verified across every released version). There is no separate "new"
# transcoder, so PlexTranscoder is the one canonical folder. In the unlikely
# event Plex ever lists a second, distinct source, it lands in PlexTranscoder-2.
typeset -gr TC_NAME="PlexTranscoder"
typeset -gr TC_PATH="${OUT_PATH}/${TC_NAME}"
typeset -gr VERSION_FILE="${REPO_PATH}/latest.version"

die() {
    echo "FATAL: $1"
    rm -rf "${WORK_PATH}" > /dev/null 2>&1
    exit 1
}

# Creates a directory, including its entire tree, if it doesn't exist.
make_dir() {
    if [[ ! -d "$1" ]]; then
        mkdir -p "$1" || die "Failed to create directory $1"
    fi
}

typeset -g api_version=''
typeset -g pms_url=''
query_pms_update_service() {
    () {
        echo "Querying PMS update service at $1"
        [[ -n "$1" ]] || die "Update request is invalid"
        api_version="$(jq --raw-output '.computer.FreeBSD.version' "$1")"
        [[ -n "$api_version" ]] || die "Failed to get API version from update service"
        pms_url="$(jq --raw-output '.computer.FreeBSD.releases[0].url' "$1")"
        [[ -n "$pms_url" ]] || die "Failed to get archive URL from update service"
    } =(curl --location --fail --silent "https://plex.tv/api/downloads/1.json")
}

# Download a URL to a temp file and extract it into a target directory.
# Note: we download to a file (not a pipe) so tar can auto-detect the
# compression -- GNU tar can't sniff a non-seekable stdin and would fail on the
# bzip2 PMS archive with "Archive is compressed. Use -j option".
download_and_extract() {
    local url="$1" target_path="$2"
    [[ -n "$url" ]] || die "Invalid parameters for download_and_extract: '$1'"
    [[ -z "$target_path" ]] || {
        make_dir "$target_path"
        cd "$target_path" || die "Couldn't change directory to $target_path for download_and_extract"
    }
    local tmp_archive
    tmp_archive="$(mktemp)" || die "Failed to create temp file for download"
    { curl --location --fail --output "$tmp_archive" "$url" && tar xf "$tmp_archive" } || {
        echo "Couldn't download or extract data from '$url'"
        rm -f "$tmp_archive"
        return 1
    }
    rm -f "$tmp_archive"
    return 0
}

# Maps each unpacked transcoder (by repo folder name) to the original ffmpeg
# source directory name, which carries the full upstream commit hash, e.g.
# plexinc-plex-media-server-ffmpeg-gpl-c75335c5e1ba5fa483dc6100c6a11c54e48f759f
typeset -gA FFMPEG_SOURCE_DIRS

# Download one ffmpeg source archive and unpack it into repo_path.
unpack_ffmpeg_archive() {
    local url="$1" temp_path="$2" repo_path="$3"
    echo "Downloading $url"
    () {
        local archive_file="$1"
        [[ -n "$1" && -s "$1" ]] || {
            echo "Failed to download ffmpeg archive from $url"
            return 1
        }
        make_dir "$temp_path"
        cd "$temp_path"
        echo "Extracting ${archive_file}"
        tar xf "${archive_file}" || {
            echo "Failed to extract ${archive_file}"
            return 1
        }
        typeset -a extracted_files; extracted_files=( "${temp_path}"/*(DN) )
        [[ ! -d "$repo_path" ]] || rm -rf "$repo_path" > /dev/null 2>&1
        local source_name
        if (( ${#extracted_files[@]} == 1 )) && [[ -d "${extracted_files[1]}" ]]; then
            # Preserve the original hash-bearing folder name before renaming.
            source_name="${extracted_files[1]:t}"
            echo "Moving code from ${extracted_files[1]} to ${repo_path}"
            mv "${extracted_files[1]}" "${repo_path}"
        else
            source_name="${url:t}"
            echo "Moving code from ${temp_path} to ${repo_path}"
            make_dir "${repo_path}"
            mv "${temp_path}"/*(DN) "${repo_path}"/
        fi
        FFMPEG_SOURCE_DIRS[${repo_path:t}]="${source_name}"
        echo "Unpacked to ${repo_path} (source: ${source_name})"
        return 0
    } =(curl --location --fail "$url")
    return $?
}

get_ffmpeg_archives_from_server() {
    local license_file="$(find "${PMS_PATH}" | grep 'Resources/LICENSE')"
    [[ -n "$license_file" ]] || die "Failed to locate LICENSE file for parsing"

    local ffmpeg_url=''
    local -i ffct=0 unpacked=0
    # Process substitution (not a pipe) keeps the loop in the current shell so
    # FFMPEG_SOURCE_DIRS set inside unpack_ffmpeg_archive survives the loop.
    # The first (normally only) source goes to PlexTranscoder; any additional
    # distinct source would go to PlexTranscoder-2, -3, ...
    while read ffmpeg_url; do
        local dest
        if (( ffct == 0 )); then
            dest="${TC_PATH}"
        else
            dest="${OUT_PATH}/${TC_NAME}-$((ffct + 1))"
        fi
        unpack_ffmpeg_archive "${ffmpeg_url}" "${WORK_PATH}/src-${ffct}" "${dest}" && unpacked=1
        (( ffct++ ))
    done < <(grep -oiE 'https?://[^[:space:]]*ffmpeg[^[:space:]]*' "$license_file" | awk '!seen[$0]++')
    (( unpacked == 1 )) && return 0 || return 1
}

# When every unpacked transcoder shares the same upstream ffmpeg source (same
# hash-bearing folder name), keep a single canonical copy and drop the rest.
# Plex lists one GPL ffmpeg source, so normally there is only one folder and
# this is a no-op. It only matters if Plex ever lists multiple URLs that happen
# to resolve to the same source.
dedupe_identical_sources() {
    local -a labels=( ${(k)FFMPEG_SOURCE_DIRS} )
    (( ${#labels[@]} > 1 )) || return 0

    local first="${FFMPEG_SOURCE_DIRS[${labels[1]}]}"
    local l
    for l in ${labels}; do
        [[ "${FFMPEG_SOURCE_DIRS[$l]}" == "$first" ]] || return 0
    done

    # All identical: keep PlexTranscoder if present, else the first label.
    local keep="${TC_NAME}"
    [[ -n "${FFMPEG_SOURCE_DIRS[$keep]}" ]] || keep="${labels[1]}"
    for l in ${labels}; do
        [[ "$l" == "$keep" ]] && continue
        rm -rf "${OUT_PATH}/$l"
        unset "FFMPEG_SOURCE_DIRS[$l]"
        echo "Removed duplicate ${l} (identical source to ${keep})"
    done
}

make_dir "$OUT_PATH"
make_dir "$WORK_PATH"

query_pms_update_service
echo "Latest Plex FreeBSD version: $api_version"

download_and_extract "$pms_url" "${PMS_PATH}" || die "Failed to download/extract PMS archive"

if get_ffmpeg_archives_from_server; then
    dedupe_identical_sources
    # Record the Plex version plus the original hash-bearing ffmpeg source
    # folder name (the rename to PlexTranscoder otherwise discards the upstream
    # commit hash).
    {
        echo "plex=${api_version}"
        for label in ${(ok)FFMPEG_SOURCE_DIRS}; do
            echo "${label}=${FFMPEG_SOURCE_DIRS[$label]}"
        done
    } > "${VERSION_FILE}"
    echo "Done. FFMPEG source unpacked under: ${OUT_PATH}"
    echo "Recorded versions:"
    cat "${VERSION_FILE}"
else
    echo "Done, but no ffmpeg source archives were unpacked (check the warnings above)."
fi

rm -rf "${WORK_PATH}" > /dev/null 2>&1
