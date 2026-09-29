#!/usr/bin/env bash
# 构建前同步版本号，再执行 flutter build；构建成功后安装到 adb 设备。
#
# 用法：
#   ./scripts/build.sh apk                  构建 release apk 并安装
#   ./scripts/build.sh apk --release
#   ./scripts/build.sh apk --debug
#   ./scripts/build.sh appbundle            构建 aab（不能 adb install，自动跳过安装）
#   ./scripts/build.sh apk --no-install     只构建，不安装
#   ./scripts/build.sh apk --uninstall      先卸载再装（build 号回退时用；会清数据）
#   WITH_TIME=1 ./scripts/build.sh apk      同日多次构建时带时分
#
# 第一个参数必须是构建目标（apk / appbundle），其余原样转给 `flutter build`。
# 接多台设备时会列出编号让你选；直接回车 = 取消安装（构建仍算成功）。
# 环境变量：WITH_TIME=1 同上；ADB=/path/to/adb 指定 adb。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

usage() {
  cat <<'EOF'
构建 + 安装 Helm One。

用法：
  ./scripts/build.sh apk                  构建 release apk 并安装到 adb 设备
  ./scripts/build.sh apk --release
  ./scripts/build.sh apk --debug          用 app-debug.apk
  ./scripts/build.sh appbundle            构建 aab（不能 adb install，自动跳过安装）
  ./scripts/build.sh apk --no-install     只构建，不安装
  ./scripts/build.sh apk --uninstall      先卸载再装（build 号回退时用；会清数据）

  ./scripts/build.sh -h | --help          显示本帮助

说明：
  第一个参数必须是构建目标（apk / appbundle），其余参数原样转给 `flutter build`。
  接多台设备时会列出编号让你选；直接回车 = 取消安装（构建仍算成功）。
  环境变量：WITH_TIME=1 同日多次构建带时分；ADB=/path/to/adb 指定 adb。
EOF
}

# 目标必须显式给：apk 和 appbundle 之间猜不出来（而且 aab 不能 adb install）。
# `-h/--help` 是主动要看帮助 → stdout + exit 0；什么都不给是用错 → stderr +
# exit 1，免得在脚本里被 `build.sh || ...` 悄悄当成成功吞掉。
# 这一段必须在 sync_version.sh 之前 —— 看帮助不该改 pubspec.yaml。
if [[ $# -eq 0 ]]; then
  usage >&2
  exit 1
fi
case "$1" in
  -h|--help|help) usage; exit 0 ;;
esac

# ── 参数 ─────────────────────────────────────────────────────────────────────
# 第一个非选项参数是构建目标（兼容原先的 `build.sh apk`），其余原样转给 flutter。
TARGET=""
INSTALL=1
UNINSTALL=0
PASSTHRU=()

for arg in "$@"; do
  case "$arg" in
    --no-install) INSTALL=0 ;;
    --uninstall)  UNINSTALL=1 ;;
    -*)           PASSTHRU+=("$arg") ;;
    *)
      if [[ -z "$TARGET" ]]; then
        TARGET="$arg"
      else
        PASSTHRU+=("$arg")
      fi
      ;;
  esac
done

if [[ -z "$TARGET" ]]; then
  echo "错误：没给构建目标（只给了选项）。" >&2
  echo >&2
  usage >&2
  exit 1
fi

# ── 构建 ─────────────────────────────────────────────────────────────────────
"$ROOT/scripts/sync_version.sh"
VER="$(grep -E '^version:' pubspec.yaml | head -1 | awk '{print $2}')"
echo "building $TARGET ($VER) ..."

FLUTTER="${FLUTTER_ROOT:-/home/jinsc/SDK/Flutter/flutter_3.44.6}/bin/flutter"
if [[ ! -x "$FLUTTER" ]]; then
  FLUTTER="$(command -v flutter)"
fi

# 注意：这里**不能**再用 exec —— exec 会把本 shell 换成 flutter，后面就没法安装了。
"$FLUTTER" build "$TARGET" ${PASSTHRU[@]+"${PASSTHRU[@]}"}

# ── 安装 ─────────────────────────────────────────────────────────────────────
if [[ "$INSTALL" != "1" ]]; then
  echo "（--no-install：已跳过安装）"
  exit 0
fi

case "$TARGET" in
  apk) ;;
  appbundle|aab)
    echo "（aab 是上架包，不能 adb install，已跳过安装）"
    exit 0
    ;;
  *)
    echo "（$TARGET 不是 Android apk，已跳过安装）"
    exit 0
    ;;
esac

# 菜单必须走终端：stdin 可能被重定向（例如 `| tee`），那时问不出来。
if [[ ! -r /dev/tty ]]; then
  echo "（没有可交互终端，已跳过安装）"
  exit 0
fi

# 打印编号菜单让用户选一个，stdout 回显 0 起的下标。
pick_index() {
  local title="$1"; shift
  local -a items=("$@")
  local n=${#items[@]}
  local i sel

  if (( n == 0 )); then
    return 1
  fi
  if (( n == 1 )); then
    echo 0
    return 0
  fi

  printf '%s\n' "$title" > /dev/tty
  for (( i = 0; i < n; i++ )); do
    printf '  %d) %s\n' "$(( i + 1 ))" "${items[i]}" > /dev/tty
  done

  while :; do
    printf '请选择 [1-%d]（回车取消）: ' "$n" > /dev/tty
    if ! read -r sel < /dev/tty; then
      return 1
    fi
    if [[ -z "$sel" ]]; then
      return 1
    fi
    if [[ "$sel" =~ ^[0-9]+$ ]] && (( sel >= 1 && sel <= n )); then
      echo $(( sel - 1 ))
      return 0
    fi
    printf '  输入无效：%s\n' "$sel" > /dev/tty
  done
}

# ADB 允许用环境变量覆盖，但**必须验一下真能用**：一个继承来的、已失效的
# ADB 路径会让后面 `"$ADB" devices` 以 127（command not found）炸掉，而 set -e
# 会把它当成脚本失败 —— 报错里却完全看不出是 adb 路径的问题（踩过）。
# 注意 `${ADB:-}`：set -u 下裸用 $ADB 会在变量不存在时先报错。
if [[ -n "${ADB:-}" && ! -x "${ADB:-}" ]]; then
  echo "⚠ ADB 环境变量指向的不是可执行文件：$ADB（改回自动查找）" >&2
  ADB=""
fi

# adb 优先用 PATH 里的；找不到再按 ANDROID_HOME / ANDROID_SDK_ROOT / 常见默认位置找。
ADB="${ADB:-}"
if [[ -z "$ADB" ]]; then
  ADB="$(command -v adb || true)"
fi
if [[ -z "$ADB" ]]; then
  for cand in "${ANDROID_HOME:-}/platform-tools/adb" \
              "${ANDROID_SDK_ROOT:-}/platform-tools/adb" \
              "$HOME/Android/Sdk/platform-tools/adb" \
              "/lib/android-sdk/platform-tools/adb"; do
    if [[ -x "$cand" ]]; then
      ADB="$cand"
      break
    fi
  done
fi

if [[ -z "$ADB" || ! -x "$ADB" ]]; then
  echo "⚠ 找不到可用的 adb，已跳过安装（构建已完成）。" >&2
  echo "  装上 Android platform-tools 后重试，或用 ADB=/path/to/adb 指定。" >&2
  exit 0
fi

# 解析 `adb devices -l` 的输出（结果写进下面几个全局数组）。
#
# **别假设分隔符是制表符**：实测 adb 1.0.41 / 36.0.0 的输出是**空格对齐** ——
# serial 用空格补齐到 24 列再跟状态，整行一个 \t 都没有（`adb devices -l | cat -A`
# 直接看得出来）。按 \t 切会把每一行都丢掉，症状就是"设备明明在、脚本说没设备"。
#
# 所以这里按"空白串"切（awk 默认 FS 就是干这个的），再用**状态白名单**认设备行 ——
# 顺带自然过滤掉 "List of devices attached" 和 "* daemon ..." 这些非设备行。
parse_devices() {
  READY_SERIALS=()
  READY_LABELS=()
  OTHER_LINES=()

  local serial state detail
  while IFS=$'\t' read -r serial state detail; do
    [[ -z "$serial" ]] && continue
    if [[ "$state" == "device" ]]; then
      READY_SERIALS+=("$serial")
      if [[ -n "$detail" ]]; then
        READY_LABELS+=("$serial  ($detail)")
      else
        READY_LABELS+=("$serial")
      fi
    else
      OTHER_LINES+=("$serial  [$state]")
    fi
  done < <(printf '%s\n' "$1" | awk '
    {
      if ($1 ~ /^\*/) next
      state = $2
      if (state != "device" && state != "offline" && state != "unauthorized" && \
          state != "bootloader" && state != "recovery" && state != "sideload" && \
          state != "no" && state != "authorizing") next
      detail = ""
      for (i = 3; i <= NF; i++) {
        if ($i ~ /^model:/) { detail = $i; sub(/^model:/, "", detail) }
      }
      gsub(/_/, " ", detail)
      printf "%s\t%s\t%s\n", $1, state, detail
    }')
}

# 读一次设备快照。**不丢 stderr**：daemon 启动消息和连接失败原因都在那儿，
# 藏起来的话 "没设备" 就成了无法解释的黑盒（最初就是 2>/dev/null 踩的）。
DEV_RAW="$("$ADB" devices -l 2>&1)"
parse_devices "$DEV_RAW"

# server 没起来时，第一次调用只负责把 daemon 拉起来，USB 枚举还没完成，
# 这一趟经常返回空列表 —— 所以要再问几次。但也不能在真没插设备时干等：
# 只有"刚把 daemon 拉起来"这种情况才多等。
if (( ${#READY_SERIALS[@]} == 0 )) &&
   [[ "$DEV_RAW" == *"daemon not running"* || "$DEV_RAW" == *"starting now"* ||
      "$DEV_RAW" == *"daemon started"* ]]; then
  echo "（adb server 刚启动，等它枚举完 USB 设备…）"
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    sleep 0.3
    DEV_RAW="$("$ADB" devices -l 2>&1)"
    parse_devices "$DEV_RAW"
    if (( ${#READY_SERIALS[@]} > 0 )); then
      break
    fi
  done
fi

if (( ${#READY_SERIALS[@]} == 0 )); then
  echo
  echo "⚠ 没有可直接安装的设备，已跳过安装（构建已完成）。"
  # 把 adb 自己说的话放出来 —— "daemon not running" / "cannot connect" 这类
  # 原因都在这里，比一句"没设备"有用得多。
  while IFS= read -r l; do
    [[ -z "$l" || "$l" == "List of devices attached" ]] && continue
    echo "  adb: $l"
  done <<<"$DEV_RAW"
  if (( ${#OTHER_LINES[@]} > 0 )); then
    echo "  这些设备目前不可用："
    for l in "${OTHER_LINES[@]}"; do
      echo "    - $l"
    done
    echo "  unauthorized → 在手机上确认「允许 USB 调试」；offline → 拔插一次或 adb kill-server"
  else
    echo "  插上设备、打开 USB 调试，再跑一次（或先 adb devices 看状态）。"
  fi
  exit 0
fi

if (( ${#READY_SERIALS[@]} == 1 )); then
  IDX=0
  echo "设备：${READY_LABELS[0]}"
else
  # 有可用设备时也提一下不可用的，否则"明明插了 4 台却只列 3 台"会让人困惑。
  if (( ${#OTHER_LINES[@]} > 0 )); then
    echo "（另有 ${#OTHER_LINES[@]} 台不可用，未计入候选：）"
    for l in "${OTHER_LINES[@]}"; do
      echo "  - $l"
    done
  fi
  if ! IDX="$(pick_index "检测到 ${#READY_SERIALS[@]} 台设备，选一台安装：" "${READY_LABELS[@]}")"; then
    echo "已取消安装（构建已完成）。"
    exit 0
  fi
fi
SERIAL="${READY_SERIALS[IDX]}"

# 挑要装的 apk：先按构建模式精确匹配，`--split-per-abi` 之类对不上名字时列出来问。
APK_DIR="$ROOT/build/app/outputs/flutter-apk"
MODE="release"
for a in ${PASSTHRU[@]+"${PASSTHRU[@]}"}; do
  case "$a" in
    --debug)   MODE="debug" ;;
    --profile) MODE="profile" ;;
  esac
done

APK=""
if [[ -f "$APK_DIR/app-$MODE.apk" ]]; then
  APK="$APK_DIR/app-$MODE.apk"
else
  CANDIDATES=()
  while IFS= read -r apk; do
    [[ -n "$apk" ]] && CANDIDATES+=("$apk")
  done < <(find "$APK_DIR" -maxdepth 1 -name '*.apk' -printf '%T@ %p\n' 2>/dev/null |
           sort -rn | cut -d' ' -f2-)

  if (( ${#CANDIDATES[@]} == 0 )); then
    echo "⚠ 在 $APK_DIR 下没找到 apk，已跳过安装。" >&2
    exit 0
  elif (( ${#CANDIDATES[@]} == 1 )); then
    APK="${CANDIDATES[0]}"
  else
    if ! IDX="$(pick_index "找到多个 apk（--split-per-abi？），选一个安装：" "${CANDIDATES[@]}")"; then
      echo "已取消安装（构建已完成）。"
      exit 0
    fi
    APK="${CANDIDATES[IDX]}"
  fi
fi

echo
echo "→ 安装到 $SERIAL"
echo "  $APK"

# applicationId 从 gradle 里取，别写死 —— 改名时这里不会失效。
APP_ID="$(sed -n -E 's/^[[:space:]]*applicationId[[:space:]]*=[[:space:]]*"([^"]+)".*/\1/p' \
          android/app/build.gradle.kts 2>/dev/null | head -1)"

if [[ "$UNINSTALL" == "1" ]]; then
  echo "  （--uninstall：先卸载，应用数据会被清空）"
  if [[ -n "$APP_ID" ]]; then
    "$ADB" -s "$SERIAL" uninstall "$APP_ID" >/dev/null 2>&1 || true
  fi
fi

if out="$("$ADB" -s "$SERIAL" install -r "$APK" 2>&1)"; then
  # adb 的成功字面量是 "Success"；原样打出来，别吞掉细节。
  printf '%s\n' "$out" | sed 's/^/  /'
  echo "✓ 安装完成：$SERIAL"
  exit 0
fi

printf '%s\n' "$out" | sed 's/^/  /' >&2

if grep -q 'INSTALL_FAILED_VERSION_DOWNGRADE' <<<"$out"; then
  cat >&2 <<'EOF'

  ✗ 版本号回退了。Android 只允许升级或同版本覆盖，而设备上那个 build 号更大。
    常见于「带时分的构建(202609170027) → 普通构建(20260917)」。两条路：
      ./scripts/build.sh apk --uninstall    # 先卸载再装（会清空应用数据）
      WITH_TIME=1 ./scripts/build.sh apk    # 让 build 号继续变大
EOF
elif grep -q 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' <<<"$out"; then
  cat >&2 <<'EOF'

  ✗ 签名不一致（设备上装的这个包是另一套签名，例如 debug 版 vs release 版）。
    只能卸载重装：
      ./scripts/build.sh apk --uninstall    # 会清空应用数据
EOF
fi

exit 1
