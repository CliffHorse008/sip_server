#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT_FILE="$ROOT_DIR/video.mp4"
OUTPUT_DIR="$ROOT_DIR/test_media"
VIDEO_FPS="30"
POSITIONAL_ARGS=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [input_file] [output_dir]

Options:
  -i, --input <file>    指定输入视频文件，默认: $ROOT_DIR/dlc_video.mp4
  -o, --output <dir>    指定输出目录，默认: $ROOT_DIR/test_media
  -r, --video-fps <fps> 指定输出视频帧率，默认: 30
  -h, --help            显示帮助

Examples:
  ./scripts/prepare_media.sh
  ./scripts/prepare_media.sh ./demo.mp4
  ./scripts/prepare_media.sh -i ./demo.mp4 -o ./custom_media
  ./scripts/prepare_media.sh -i ./demo.mp4 -r 25
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -i|--input)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        usage
        exit 1
      fi
      INPUT_FILE="$2"
      shift 2
      ;;
    -o|--output)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        usage
        exit 1
      fi
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -r|--video-fps)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        usage
        exit 1
      fi
      VIDEO_FPS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "unknown option: $1" >&2
      usage
      exit 1
      ;;
    *)
      POSITIONAL_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ ${#POSITIONAL_ARGS[@]} -ge 1 ]]; then
  INPUT_FILE="${POSITIONAL_ARGS[0]}"
fi

if [[ ${#POSITIONAL_ARGS[@]} -ge 2 ]]; then
  OUTPUT_DIR="${POSITIONAL_ARGS[1]}"
fi

if [[ ${#POSITIONAL_ARGS[@]} -gt 2 ]]; then
  echo "too many positional arguments" >&2
  usage
  exit 1
fi

if [[ ! -f "$INPUT_FILE" ]]; then
  echo "input file not found: $INPUT_FILE" >&2
  exit 1
fi

if ! [[ "$VIDEO_FPS" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "invalid video fps: $VIDEO_FPS" >&2
  exit 1
fi

VIDEO_GOP="$(awk "BEGIN { v = int($VIDEO_FPS + 0.5); if (v < 1) v = 1; print v }")"

mkdir -p "$OUTPUT_DIR"

echo "input  : $INPUT_FILE"
echo "output : $OUTPUT_DIR"
echo "fps    : $VIDEO_FPS"

INPUT_DURATION="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$INPUT_FILE")"
if [[ -z "$INPUT_DURATION" ]]; then
  INPUT_DURATION="10"
fi

# 这里不再直接 copy 视频流，而是转成固定帧率、无 B 帧、带 AUD 的 H264。
# 当前 demo 发送器按固定 video_fps 节奏送帧；如果源视频是 VFR 或带 B 帧，直接抽裸流会更容易出现 Linphone 端跳帧。
ffmpeg -y -i "$INPUT_FILE" \
  -an \
  -vf "fps=${VIDEO_FPS},format=yuv420p" \
  -c:v libx264 \
  -preset veryfast \
  -tune zerolatency \
  -profile:v baseline \
  -level:v 3.1 \
  -pix_fmt yuv420p \
  -g "$VIDEO_GOP" \
  -keyint_min "$VIDEO_GOP" \
  -bf 0 \
  -sc_threshold 0 \
  -x264-params "aud=1:repeat-headers=1:force-cfr=1" \
  -f h264 \
  "$OUTPUT_DIR/video.h264"

ffmpeg -y -i "$INPUT_FILE" \
  -vn \
  -c:a aac \
  -b:a 128k \
  -ac 2 \
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
