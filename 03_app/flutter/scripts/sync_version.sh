#!/usr/bin/env bash
# 可选：把 version.name + 当日日期写入 pubspec.yaml（便于非 Android / 文档一致）。
# Android Studio 点 Run 已由 android/app/build.gradle.kts 无感滚动，不必依赖本脚本。
#   手动版本：version.name（例如 1.0.0）
#   构建号：YYYYMMDD（同日多次构建可 WITH_TIME=1 → YYYYMMDDHHMM）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME_FILE="$ROOT/version.name"
PUBSPEC="$ROOT/pubspec.yaml"

if [[ ! -f "$NAME_FILE" ]]; then
  echo "1.0.0" > "$NAME_FILE"
fi

NAME="$(tr -d '[:space:]' < "$NAME_FILE")"
if [[ ! "$NAME" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.+][0-9A-Za-z.-]+)?$ ]]; then
  echo "invalid version.name: '$NAME' (expect like 1.0.0)" >&2
  exit 1
fi

if [[ "${WITH_TIME:-0}" == "1" ]]; then
  BUILD="$(date +%Y%m%d%H%M)"
else
  BUILD="$(date +%Y%m%d)"
fi

# Android versionCode 必须是 int；YYYYMMDD / YYYYMMDDHHMM 均可。
sed -i -E "s/^version:[[:space:]].*/version: ${NAME}+${BUILD}/" "$PUBSPEC"
echo "synced pubspec version: ${NAME}+${BUILD}"
