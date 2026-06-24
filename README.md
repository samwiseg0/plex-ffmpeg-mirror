# plex-ffmpeg-mirror

An automated mirror of Plex's patched FFMPEG source.

A scheduled [GitHub Actions](.github/workflows/refresh.yml) workflow runs daily,
pulls the latest FFMPEG source that Plex publishes in its bundled `LICENSE`
file, and commits any changes back to this repository. No server or cron host
of your own is required -- GitHub runs it for you.

## Layout

```
.github/workflows/refresh.yml   # the scheduled job
tools/fetch-plex-ffmpeg.zsh     # downloads + unpacks the source
plex-ffmpeg-source/             # the mirrored source (committed by the workflow)
  NewPlexTranscoder/
  PlexTranscoder/
latest.version                  # last Plex version that was mirrored
```

## How it works

1. The workflow triggers on a cron schedule (and can be run manually from the
   Actions tab via `workflow_dispatch`).
2. It installs `zsh`/`jq`, then runs `tools/fetch-plex-ffmpeg.zsh`, which:
   - asks the Plex update API for the latest FreeBSD build,
   - extracts it just to read its `Resources/LICENSE`,
   - scrapes the ffmpeg source-archive URLs from that license,
   - downloads and unpacks each into `plex-ffmpeg-source/`.
3. If the working tree changed, the workflow commits and pushes using the
   built-in `GITHUB_TOKEN`.

## First-time setup

1. Create a new empty repo on GitHub and push these files.
2. In the repo: **Settings -> Actions -> General -> Workflow permissions ->
   Read and write permissions** (lets the job push commits).
3. The schedule starts automatically. To run it immediately, open the
   **Actions** tab, select **Refresh Plex FFMPEG source**, and click **Run
   workflow**.

## Run locally

```zsh
zsh tools/fetch-plex-ffmpeg.zsh                 # -> ./plex-ffmpeg-source
zsh tools/fetch-plex-ffmpeg.zsh /some/folder    # -> /some/folder
```
