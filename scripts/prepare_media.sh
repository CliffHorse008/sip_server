#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT_FILE="${1:-$ROOT_DIR/dlc_video.mp4}"
OUTPUT_DIR="${2:-$ROOT_DIR/test_media}"

mkdir -p "$OUTPUT_DIR"

echo "input  : $INPUT_FILE"
echo "output : $OUTPUT_DIR"

INPUT_DURATION="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$INPUT_FILE")"
if [[ -z "$INPUT_DURATION" ]]; then
  INPUT_DURATION="10"
fi

ffmpeg -y -i "$INPUT_FILE" \
  -an \
  -c:v copy \
  -bsf:v h264_mp4toannexb,h264_metadata=aud=insert \
  "$OUTPUT_DIR/video.h264"

ffmpeg -y -i "$INPUT_FILE" \
  -vn \
  -c:a copy \
  -f adts \
  "$OUTPUT_DIR/audio.aac"

SOURCE_MAX_VOLUME="$(ffmpeg -hide_banner -i "$INPUT_FILE" -vn -af volumedetect -f null - 2>&1 | awk -F': ' '/max_volume/ {print $2}' | tail -n 1 || true)"
if [[ -n "$SOURCE_MAX_VOLUME" && "$SOURCE_MAX_VOLUME" != "-91.0 dB" ]]; then
  ffmpeg -y -i "$INPUT_FILE" \
    -vn \
    -ac 1 \
    -ar 8000 \
    -c:a pcm_alaw \
    -f alaw \
    "$OUTPUT_DIR/audio.g711a"
else
  echo "source audio is silent, generating 440Hz test tone for G711A"
  ffmpeg -y \
    -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=${INPUT_DURATION}" \
    -ac 1 \
    -c:a pcm_alaw \
    -f alaw \
    "$OUTPUT_DIR/audio.g711a"
fi

echo "generated:"
ls -lh "$OUTPUT_DIR"
