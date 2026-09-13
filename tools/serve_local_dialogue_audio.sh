#!/usr/bin/env bash
# Serve the already-generated dialogue WAV fixtures from Spark to the ESP32.
# This intentionally binds to Spark's LAN address only; no cloud service is used.
set -euo pipefail

audio_dir="${HOME}/workspace/data/aec/dialogue_wenyanwen"
esp32_dir="${audio_dir}/esp32"
bind_address="${1:-192.168.1.16}"
port="${2:-8080}"

test -d "$audio_dir"
command -v ffmpeg >/dev/null
mkdir -p "$esp32_dir"

# AudioCodec output is 16 kHz. The source fixtures are 24 kHz, so make small
# local PCM copies once; serving them avoids a full-file buffer or resampler on
# the ESP32 and prevents 1.5x-speed playback.
for source in "$audio_dir"/*_assistant.wav; do
    target="$esp32_dir/$(basename "$source")"
    if [[ ! -f "$target" || "$source" -nt "$target" ]]; then
        ffmpeg -y -v error -i "$source" -ar 16000 -ac 1 -c:a pcm_s16le "$target"
    fi
done

exec python3 -m http.server "$port" --bind "$bind_address" --directory "$audio_dir"
