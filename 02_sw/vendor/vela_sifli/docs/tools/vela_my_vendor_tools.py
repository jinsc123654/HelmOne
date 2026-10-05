#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
openvela My Vendor (SF32LB52) 板级构建辅助脚本（用法类似 ESP-IDF / sf_sdk_tools.py）。

文档:
  vendor/my_vendor/docs/tools/vela_my_vendor_tools.md

用法（在 openvela 根目录或任意子目录执行）:
  python3 vela_my_vendor_tools.py menuconfig
  python3 vela_my_vendor_tools.py build          # nuttx + boot，不打任何文件系统
  python3 vela_my_vendor_tools.py build-fs       # kv + lfs + fat（全国图）
  python3 vela_my_vendor_tools.py pack-sd-img    # SD 整盘 .img（Etcher/dd）
  python3 vela_my_vendor_tools.py burn-sd --sd /dev/sdX  # 刻录 my_vendor_sd.img 到 SD 卡
  python3 vela_my_vendor_tools.py build-all      # main → factory → boot → fs
  python3 vela_my_vendor_tools.py wrap           # nuttx.bin → nuttx.flash.bin（烧录用）
  python3 vela_my_vendor_tools.py pack-fw        # OVNX+CRC，改名为 1.0.0-Helm-One.bin 供 OTA
  python3 vela_my_vendor_tools.py build-factory  # 工厂变体（MTP 自启 /mnt/lfs + /mnt/kv + /mnt/fat）
  python3 vela_my_vendor_tools.py flash-factory  # 仅烧 factory 槽 @ 0x62500000
  python3 vela_my_vendor_tools.py flash-fs       # 仅烧录 fs_root.bin
  python3 vela_my_vendor_tools.py flash-all      # boot + factory + main + fs
  python3 vela_my_vendor_tools.py monitor                 # 交互监视（自动挂 hub）
  python3 vela_my_vendor_tools.py monitor --status        # 查 hub
  python3 vela_my_vendor_tools.py monitor --follow        # 第二终端 / 脚本跟日志
  python3 vela_my_vendor_tools.py monitor sys             # 往 NSH 发一条命令
  python3 vela_my_vendor_tools.py decode 0x10000251   # 单地址解析（无需 ALLSYMS）
  python3 vela_my_vendor_tools.py build flash monitor    # 可组合，顺序执行

Tab 补全（bash）:
  eval "$(python3 vela_my_vendor_tools.py complete bash)"
  python3 vela_my_vendor_tools.py complete install
"""

from __future__ import annotations

import argparse
import base64
import datetime
import faulthandler
import gzip
import hashlib
import importlib.util
import json
import os
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import zlib
import threading
import time
import types
from collections import deque
from pathlib import Path

# =============================================================================
# 工程配置 — 切换板子时主要改这里
# =============================================================================
# ⚠ 本工具链**不假定这棵树叫 my_vendor**：下面所有相对路径都以**本树根**
# （`VENDOR_ROOT`，由本文件位置推导：<vendor>/docs/tools/ 往上三级）为基准，
# 运行期才解析 ⇒ 本树放在 <openvela>/vendor/<任意名字>/ 下都能编；多份并存也
# 互不干扰（cmake_out 目录名会带上本树目录名）。

# 相对**本树根**的板级 config 路径（末尾可有可无 /）
# 与 sifli 一致：configs/nsh/ 仅含 defconfig
BOARD_CONFIG = "boards/sf32lb52/my_vendor/configs/nsh"

# Factory 固件变体（MTP 自启 /mnt/lfs + EXTRA_PATHS /mnt/kv,/mnt/fat）。
# build-all 始终编它；也可单独 build-factory / flash-factory。
# 独立输出到 cmake_out/<本树目录名>_nsh-factory/，打进 pack-sd-img / flash-all 的 factory 槽；
# 2SFBL 的 "factory" 命令跳转到它。产品固件仍走上面的 BOARD_CONFIG。日常 flash 不含此槽。
FACTORY_BOARD_CONFIG = "boards/sf32lb52/my_vendor/configs/nsh-factory"

# NAND boot 配置（分区表、烧录参数）— 不在 board configs 里
BOOT_CONFIG = "boot_loader/config/nsh"

# UART 下载 / 串口监视（Linux: /dev/ttyUSB0；Windows: COM19 或 19）
PORT: str | None = None

# 串口监视波特率（SF32 日志口常用 1000000，与 CONFIG_UART_BAUD 一致）
MONITOR_BAUD = 1000000
# MCU 未烧新固件时：CH340 一次 USB 包打满 UART 会赶上 DMA 重装窗口。
# 超过该长度的 TX 按块写并留 1ms 间隙；单键/短命令不节流。
MONITOR_TX_CHUNK = 64
MONITOR_TX_GAP_S = 0.001

# sftool 烧录波特率（默认 1M；用 -fb 3000000 等调节，见 SiFli ram_patch 文档）
FLASH_BAUD = 1_000_000

# NuttX readline 仅以 LF(\\n) 作为行结束（见 readline_common.c），Enter 须发 \\n 不能发 \\r
# NSH 由设备 readline 回显，保持 False；bttool 用 getline 无回显，由 MONITOR_SMART_BTTOOL 自动开本地 echo
MONITOR_ECHO = False
MONITOR_EOL = "lf"  # NuttX NSH 用 lf；RT-Thread MSH 可试 cr
# bttool 下自动开本地回显（getline 无设备回显）；空回车原样转发给设备
MONITOR_SMART_BTTOOL = True
# True：串口收到的 VT100/ANSI（清行 [K、颜色等）原样交给终端，否则会显示成 ␛[K
MONITOR_RAW = True
# miniterm 过滤器：direct=透传；default 会把 ESC 转成可见乱码
MONITOR_FILTER = "direct"
# NSH Tab 补全（须 defconfig 含 CONFIG_READLINE_TABCOMPLETION=y 并重新 build）
# menuconfig: System Libraries → readline() → Tab completion
# ttyACM/USB-CDC 须拉高 DTR，否则主机发不出数据
MONITOR_DTR = True
MONITOR_RTS = True
# 无 CONFIG_ALLSYMS 时，用 ELF+addr2line 解析 backtrace/PC（类似 ESP-IDF monitor）
MONITOR_DECODE_ELF = True
ELF_RESOLVE_SCRIPT = "scripts/vela_elf_resolve.py"
# monitor 旁路控制：本机 Unix 套接字。用户继续在 miniterm 里操作串口；
# agent 用 `monitor-ctl` 把按键/命令转发进同一串口（不抢 tty）。
MONITOR_CTL_WAIT_MS = 3000
MONITOR_CTL_IDLE_MS = 250
MONITOR_CTL_RX_RING = 64 * 1024
# 让出串口（给 flash）后最多等多久自己收回：flash 崩了/被 Ctrl-C 掉，也不会把
# 串口永久留在"已让出"状态。见 MonitorBridge.release / acquire。
MONITOR_HANDOVER_MAX_S = 180.0
# 最后一个流式客户端退出后，broker 还等多久才收工。烧录/USB 复位会让客户端
# 短暂掉线；5 s 太短会"看起来 hub 一直在重启"。给 5 分钟宽限：人还在别的
# 终端里重连时 hub 还活着，真的人都走了才收工。见 _maybe_idle_shutdown /
# serial_hub --idle-timeout。
MONITOR_BROKER_IDLE_S = 300.0
# 交互式 monitor 接回 broker 时回放多少字节（退出 monitor / 烧录期间错过的那一段）。
MONITOR_CLIENT_REPLAY = 256 * 1024
# broker 的**落盘日志**（追加写）：环形缓冲只在内存里，进程重启就没了；同时写一份
# 文件，`tail -f` 随时能看。空串 = 默认 ~/.cache/vela/serial/<dev>.log（lease 化后
# /tmp 只留信物，不再堆 .sock/.log）。
MONITOR_SPOOL = ""

# serial_hub.py 懒加载（同目录）：lease / abstract 端点约定。
_SERIAL_HUB_MOD: types.ModuleType | None = None


def _serial_hub_mod() -> types.ModuleType:
    global _SERIAL_HUB_MOD
    if _SERIAL_HUB_MOD is not None:
        return _SERIAL_HUB_MOD
    path = Path(__file__).resolve().with_name("serial_hub.py")
    spec = importlib.util.spec_from_file_location("vela_serial_hub_mod", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载 serial_hub: {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    _SERIAL_HUB_MOD = mod
    return mod


def _mux_connect_target(
    port: str | None = None, hint: Path | str | None = None
) -> str | None:
    """解析多路复用连接目标：lease → abstract；兼容旧 /tmp/*.sock。"""
    hub = _serial_hub_mod()
    if port:
        got = hub.resolve_endpoint_from_lease(port)
        if got is not None:
            return got[0]
        legacy = hub.legacy_sock_path_for(port)
        if legacy.exists():
            return str(legacy)
        # hub 刚起来、lease 尚未刷盘：直接试 abstract
        return hub.abstract_sock_addr(port)
    if hint is None:
        return None
    if isinstance(hint, Path) or str(hint).endswith(".lease"):
        path = Path(hint)
        meta = hub.read_lease_json(path)
        if meta and hub.pid_alive(int(meta.get("pid") or 0)):
            return hub.unix_connect_target(
                str(meta.get("sock") or hub.abstract_sock_display(str(meta.get("port") or "")))
            )
        if path.exists() and path.suffix == ".sock":
            return str(path)
        return None
    return hub.unix_connect_target(hint)
# Ctrl-S 暂停显示：期间收到的内容先留在内存里（不落盘、不阻塞读线程），
# Ctrl-Q 时按原顺序补上屏。MONITOR_PAUSE_MAX 是缓冲上限，超出丢**最旧**的
# （保住与实时流相接的那一段）；MONITOR_PAUSE_CHUNK 是补屏时每轮最多写的
# 字节数 —— 分块写是为了边补屏边继续收串口，一次倒完会让内核串口缓冲
# （通常 4 KB）溢出丢字节。见 SifliMiniterm.pause_rx / _pause_service。
MONITOR_PAUSE_MAX = 1024 * 1024
MONITOR_PAUSE_CHUNK = 4096
# 串口读等待上限（秒）。既是 Ctrl-Q 补屏的响应上限，也是退出时线程收尾的上限。
MONITOR_READ_TIMEOUT_S = 0.2

# 并行编译任务数；0 表示自动使用 CPU 核心数
JOBS = 0

# SiFli SDK 根目录（build-boot 用；None 则按下方候选路径自动探测）
SIFLI_SDK: str | None = "/home/jinsc/SDK/SiFli/SDK/2.4"

# NAND boot 镜像（flash 固定从此目录读取，勿放 cmake_out）
BOOT_BIN_NAMES = ("ftab.bin", "bootloader.bin")
FS_ROOT_BIN_NAME = "fs_root.bin"
FLASHER_ARGS_NAME = "flasher_args.json"
FS_ROOT_BUILD_SCRIPT = "scripts/build_fs_root.sh"
MKFS_DIR = "boards/sf32lb52/my_vendor/mkfs"
FLASH_ARGS_LIB = "scripts/flash_args_lib.py"

# NAND boot 镜像目录（ftab.bin / bootloader.bin）；flash 从此处读取
# 相对**本树根**，或绝对路径
BOOT_BIN_DIR = "boot_loader/bin"

# vendored bootloader 工程（scons 编译入口）
BOOT_LOADER_DIR = "boot_loader"

# sftool 烧录：地址由 ptab.json 实时计算，清单见 sftool_param.json；
# build 后写入 cmake_out/.../flasher_args.json（类似 ESP-IDF flasher_args）。

# nuttx.bin 后处理：1KiB OVNX 头(版本/构建时间/整段长度) + payload + 4B CRC
NUTTX_BIN_NAME = "nuttx.bin"
NUTTX_FLASH_BIN_NAME = "nuttx.flash.bin"
WRAP_NUTTX_SCRIPT = "scripts/wrap_nuttx_image.sh"
# build / flash 自动 wrap；二级 boot 校验 CRC 并 UART 打印构建日期/长度/CRC
AUTO_WRAP_NUTTX = True

# nuttx 编译成功后自动打印固件体积摘要（analyze_firmware_size.py --brief）
AUTO_SIZE_SUMMARY = True
SIZE_ANALYZE_SCRIPT = "scripts/analyze_firmware_size.py"

# =============================================================================

BOARD_COMMANDS = frozenset(
    {
        "build",
        "build-all",
        "build-fs",
        "wrap",
        "pack-fw",
        "pack-ota",
        "menuconfig",
        "savedefconfig",
        "flash",
        "flash-all",
        "flash-fs",
        "build-factory",
        "flash-factory",
        "pack-sd-img",
        "burn-sd",
        "clean",
        "fullclean",
        "distclean",
    },
)

COMMANDS = (
    "build",
    "build-all",
    "build-boot",
    "build-factory",
    "flash-factory",
    "build-fs",
    "pack-sd-img",
    "burn-sd",
    "wrap",
    "pack-fw",
    "pack-ota",
    "menuconfig",
    "savedefconfig",
    "flash",
    "flash-all",
    "flash-fs",
    "monitor",
    # 兼容旧名（docs/补全仍认；日常请只用 monitor）
    "monitor-broker",
    "monitor-ctl",
    "decode",
    "clean",
    "fullclean",
    "distclean",
    "complete",
    "help",
)

# monitor 客户端模式（status/follow/发 NSH）识别用；交互监视的选项不在此列。
_MONITOR_CLIENT_TOKS = frozenset(
    {
        "--status",
        "status",
        "ping",
        "--rx",
        "--follow",
        "--no-stdin",
        "--line-stdin",
        "--follow-replay",
        "--hex",
        "--wait",
        "--idle",
        "--no-nl",
        "release",
        "acquire",
        "stop",
    }
)
_MONITOR_INTERACTIVE_OPT_TAKES_VAL = frozenset(
    {
        "-p",
        "--port",
        "-b",
        "--baud",
        "--sock",
        "--dtr",
        "--rts",
        "--elf",
    }
)
_MONITOR_INTERACTIVE_FLAGS = frozenset(
    {
        "--no-reset",
        "--reset",
        "--no-broker",
        "--broker",
        "--no-ctl",
        "--no-decode",
        "--decode",
        "--no-smart-bttool",
        "--smart-bttool",
        "-h",
        "--help",
    }
) | _MONITOR_INTERACTIVE_OPT_TAKES_VAL


def _is_monitor_client_argv(argv: list[str]) -> bool:
    """`monitor` 后面是客户端用法（status/follow/NSH），而不是交互 miniterm。"""
    if not argv:
        return False
    i = 0
    while i < len(argv):
        a = argv[i]
        key = a.split("=", 1)[0]
        if a in _MONITOR_CLIENT_TOKS or key in _MONITOR_CLIENT_TOKS:
            return True
        if a in _MONITOR_INTERACTIVE_OPT_TAKES_VAL and i + 1 < len(argv) and "=" not in a:
            i += 2
            continue
        if a in _MONITOR_INTERACTIVE_FLAGS or key in _MONITOR_INTERACTIVE_FLAGS:
            i += 1
            continue
        if a.startswith("-"):
            # 未知短/长选项：交给 argparse 交互路径报错，不抢成客户端
            i += 1
            continue
        # 裸词 → 当作 NSH / 板级命令转发
        return True
    return False


def _monitor_early_entry(argv: list[str]) -> tuple[int, str] | None:
    """若应以客户端入口处理，返回 (monitor 词下标, 词本身)。"""
    found: list[tuple[int, str]] = []
    for i, arg in enumerate(argv):
        if arg in COMMANDS or arg == "ctl":
            found.append((i, arg))
    for i, arg in found:
        if arg in ("monitor-ctl", "ctl"):
            return i, arg
    if len(found) == 1 and found[0][1] == "monitor":
        i, arg = found[0]
        if _is_monitor_client_argv(argv[i + 1 :]):
            return i, arg
    return None

SCRIPT_PATH = Path(__file__).resolve()
SCRIPT_BASENAME = SCRIPT_PATH.name
# 本树根（<vendor>）：本文件在 <vendor>/docs/tools/ 下 ⇒ 往上三级。
# 所有相对路径（BOARD_CONFIG 等）都以它为基准 —— 这棵树改名 / 多份并存都不受影响。
VENDOR_ROOT = SCRIPT_PATH.parent.parent.parent


def find_openvela_root(start: Path | None = None) -> Path:
    """向上查找包含 build.sh 与 nuttx/ 的 openvela 根目录。"""
    cur = (start or Path.cwd()).resolve()
    for directory in (cur, *cur.parents):
        if (directory / "build.sh").is_file() and (directory / "nuttx").is_dir():
            return directory
    raise RuntimeError(
        "无法定位 openvela 根目录（需存在 build.sh 与 nuttx/）。"
        "请在 openvela 工程内执行本脚本。"
    )


def boot_config_path(root: Path) -> Path:
    """NAND boot 配置目录（ptab.json / sftool_param.json / boot.json）。"""
    return _vpath(BOOT_CONFIG)


def board_config_path(root: Path) -> Path:
    rel = BOARD_CONFIG.strip().rstrip("/")
    path = _vpath(rel)
    if not path.is_dir():
        raise RuntimeError(f"板级配置目录不存在: {path}")
    defconfig = path / "defconfig"
    if not defconfig.is_file():
        raise RuntimeError(f"缺少 defconfig: {defconfig}")
    return path


def wrap_nuttx_script(root: Path) -> Path:
    return _vpath(WRAP_NUTTX_SCRIPT)


def nuttx_raw_bin(out: Path) -> Path:
    return out / NUTTX_BIN_NAME


def nuttx_flash_bin(out: Path) -> Path:
    return out / NUTTX_FLASH_BIN_NAME


def nuttx_firmware_for_flash(out: Path) -> Path:
    """Prefer wrapped nuttx.flash.bin when present."""
    wrapped = nuttx_flash_bin(out)
    if wrapped.is_file():
        return wrapped
    return nuttx_raw_bin(out)


def nuttx_elf_path(root: Path) -> Path:
    """cmake 输出目录中的 nuttx ELF（addr2line 用）。"""
    return cmake_out_dir(root) / "nuttx"


def _import_elf_resolve():
    """加载 vendor/my_vendor/scripts/vela_elf_resolve.py。"""
    root = find_openvela_root(SCRIPT_PATH.parent)
    path = _vpath(ELF_RESOLVE_SCRIPT)
    if not path.is_file():
        raise RuntimeError(f"未找到 ELF 解析脚本: {path}")
    spec = importlib.util.spec_from_file_location("vela_elf_resolve", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载 {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def make_panic_decoder(root: Path, elf: Path | None = None):
    """构建 monitor 用 PanicSerialDecoder；ELF 缺失时仍返回透传解码器。"""
    mod = _import_elf_resolve()
    if not MONITOR_DECODE_ELF:
        return mod.PanicSerialDecoder(None)
    elf_path = (elf or nuttx_elf_path(root)).resolve()
    if not elf_path.is_file():
        print(
            f"WARN: monitor 崩溃解码: ELF 不存在 ({elf_path})，"
            "请先 build；串口仍正常显示",
            file=sys.stderr,
        )
        return mod.PanicSerialDecoder(None)
    try:
        resolver = mod.ElfSymbolResolver(elf_path)
    except Exception as exc:
        print(f"WARN: monitor 崩溃解码初始化失败: {exc}", file=sys.stderr)
        return mod.PanicSerialDecoder(None)
    print(f"崩溃地址解码: {elf_path} (addr2line)", file=sys.stderr)
    return mod.PanicSerialDecoder(resolver)


def cmake_out_dir(root: Path) -> Path:
    """
    cmake 输出目录: cmake_out/<本树目录名>_<config_name>，与 build.sh 同命名习惯。
    本树在 vendor/my_vendor 时 → cmake_out/my_vendor_nsh/（与旧行为逐字一致）；
    放在 vendor/vela_sifli 时 → cmake_out/vela_sifli_nsh/ ⇒ 多份树并存互不覆盖。
    """
    cfg = board_config_path(root)
    return root / "cmake_out" / f"{VENDOR_ROOT.name}_{cfg.name}"


def factory_out_dir(root: Path) -> Path:
    """Factory 变体的 cmake 输出目录（cmake_out/<本树目录名>_nsh-factory/）。"""
    cfg = _vpath(FACTORY_BOARD_CONFIG.strip().rstrip("/"))
    return root / "cmake_out" / f"{VENDOR_ROOT.name}_{cfg.name}"


def _vpath(spec: str) -> Path:
    """把 spec 解析为路径：绝对路径原样；相对路径相对**本树根**（VENDOR_ROOT）。"""
    p = Path(spec.strip())
    if p.is_absolute():
        return p.resolve()
    return (VENDOR_ROOT / p).resolve()


def boot_bin_dir(root: Path) -> Path:
    """ftab.bin / bootloader.bin 安装目录。"""
    return _vpath(BOOT_BIN_DIR)


def boot_bin_missing(root: Path) -> list[str]:
    """返回 boot_loader/bin 中缺失的 NAND 启动镜像文件名。"""
    boot_dir = boot_bin_dir(root)
    missing: list[str] = []
    for name in BOOT_BIN_NAMES:
        if not (boot_dir / name).is_file():
            missing.append(name)
    return missing


def warn_boot_bin_missing(root: Path, *, after_nuttx: bool = False) -> None:
    """提示 boot_loader/bin 不完整（不自动构建时用）。"""
    boot_dir = boot_bin_dir(root)
    missing = boot_bin_missing(root)
    if not missing:
        return
    boot_sh = boot_loader_path(root) / "build.sh"
    when = "nuttx 编译完成后将自动尝试 build-boot …" if after_nuttx else ""
    lines = [
        "",
        "提示: boot_loader/bin/ 尚无完整 NAND 启动镜像:",
        f"  {boot_dir}/",
        f"  缺少: {', '.join(missing)}",
        when,
        "单独构建: python3 {0} build-boot  或  {1} --no-prompt".format(
            SCRIPT_BASENAME, boot_sh),
        "",
    ]
    print("\n".join(lines), file=sys.stderr)


def ensure_boot_bin_after_build(root: Path, out: Path, jobs: int) -> None:
    """nuttx 编译成功后，若 bin/ 缺镜像则自动 build-boot。"""
    boot_dir = boot_bin_dir(root)
    if not boot_bin_missing(root):
        nuttx = out / "nuttx.bin"
        ftab = boot_dir / "ftab.bin"
        if nuttx.is_file() and ftab.is_file():
            if nuttx.stat().st_mtime > ftab.stat().st_mtime:
                print(
                    "\n提示: nuttx.bin 已更新，建议重新 build-boot 以更新 ftab.bin"
                    "（镜像体积变化时必需）:\n"
                    f"  python3 {SCRIPT_BASENAME} build-boot\n",
                    file=sys.stderr,
                )
        return
    nuttx = out / "nuttx.bin"
    if not nuttx.is_file():
        print(
            f"\nWARN: {nuttx} 未生成，跳过 boot 镜像构建。\n",
            file=sys.stderr,
        )
        return
    print(
        "\n=== 正在构建 boot_loader/bin/ (ftab.bin + bootloader.bin) ===\n",
        file=sys.stderr,
    )
    cmd_build_boot(root, out, jobs)
    still = boot_bin_missing(root)
    if still:
        raise RuntimeError(
            f"boot 镜像仍未就绪: {boot_bin_dir(root)}/ 缺少 {', '.join(still)}"
        )
    print(
        f"\nboot 镜像已写入: {boot_bin_dir(root)}/\n",
        file=sys.stderr,
    )


def boot_bin_image_paths(root: Path) -> dict[str, Path]:
    """flash 使用的 boot 镜像绝对路径（仅 boot_loader/bin）。"""
    boot_dir = boot_bin_dir(root)
    return {name: (boot_dir / name).resolve() for name in BOOT_BIN_NAMES}


def fs_root_bin_path(root: Path) -> Path:
    return boot_bin_dir(root) / FS_ROOT_BIN_NAME


def fs_root_missing(root: Path) -> bool:
    return not fs_root_bin_path(root).is_file()


def config_has_bicycle(root: Path) -> bool:
    """defconfig 是否启用 MYVENDOR_BICYCLE（决定 build-all 是否打 fs）。"""
    text = (board_config_path(root) / "defconfig").read_text(
        encoding="utf-8", errors="replace"
    )
    return "CONFIG_MYVENDOR_BICYCLE=y" in text


def cmd_build_fs(root: Path) -> None:
    """打包 mkfs/{kv,lfs,fat}。仅 build-fs / build-all 调用。"""
    build = _vpath(FS_ROOT_BUILD_SCRIPT)
    mkfs = _vpath(MKFS_DIR)
    if not build.is_file():
        raise RuntimeError(f"未找到: {build}")
    if not mkfs.is_dir():
        raise RuntimeError(f"mkfs 目录不存在: {mkfs}")
    print("\n=== 构建 kv/lfs/fat 镜像 ===\n", file=sys.stderr)
    run(["bash", str(build)], cwd=root, env={"BUILD_FAT": "1"})
    if fs_root_missing(root):
        raise RuntimeError(f"fs_root.bin 未生成: {fs_root_bin_path(root)}")
    print(f"\nfs 镜像: {fs_root_bin_path(root)}\n", file=sys.stderr)
    out = cmake_out_dir(root)
    ensure_flasher_args(root, out)


def boot_loader_path(root: Path) -> Path:
    """vendored boot_loader 根目录。"""
    return _vpath(BOOT_LOADER_DIR)


def resolve_sifli_sdk(root: Path) -> Path:
    """解析 SiFli SDK 根目录（含 export.sh）。"""
    if SIFLI_SDK:
        sdk = Path(SIFLI_SDK).expanduser().resolve()
        if sdk.is_dir() and (sdk / "export.sh").is_file():
            return sdk
        raise RuntimeError(
            f"SIFLI_SDK 无效（需含 export.sh）: {sdk}\n"
            f"请修改 {SCRIPT_BASENAME} 顶部 SIFLI_SDK。"
        )

    candidates = [
        root.parent / "SiFli" / "SDK" / "2.4",
        Path.home() / "SDK" / "SiFli" / "SDK" / "2.4",
    ]
    env = os.environ.get("SIFLI_SDK") or os.environ.get("SIFLI_SDK_PATH")
    if env:
        candidates.insert(0, Path(env).expanduser())

    for candidate in candidates:
        sdk = candidate.resolve()
        if sdk.is_dir() and (sdk / "export.sh").is_file():
            return sdk

    raise RuntimeError(
        "未找到 SiFli SDK。请在 vela_my_vendor_tools.py 顶部设置 SIFLI_SDK，"
        "或 export SIFLI_SDK=/path/to/SDK/2.4"
    )


def board_config_arg(root: Path) -> str:
    """传给 build.sh 的路径参数（带尾部 /），**相对 openvela 根**。

    ⚠ build.sh 的 setup_cmake_binary_dir 用 ``[ -d ${ROOTDIR}/${arg} ]`` 判存在、
    再 basename 两跳取 board/config 名 ⇒ 参数必须是**相对根**的路径、形状为
    ``./vendor/<本树目录名>/boards/<chip>/<product>/configs/<cfg>/``：
    本树改名无所谓（仍相对根），但**不能改成绝对路径**（会掉进另一条 N 段解析分支）。
    """
    cfg = board_config_path(root)
    try:
        rel = cfg.relative_to(root.resolve())
    except ValueError:
        # 本树不在该根下（少见）：退回绝对路径，调用方自行保证可用
        return f"{cfg.as_posix().rstrip('/')}/"
    return f"./{rel.as_posix().strip('/')}/"


def board_config_name() -> str:
    """configs 目录名，如 nsh / nsh-driver。"""
    return Path(BOARD_CONFIG.strip().rstrip("/")).name


def print_firmware_size_summary(root: Path) -> None:
    """编译成功后打印简洁体积表（失败时静默跳过）。"""
    if not AUTO_SIZE_SUMMARY:
        return
    script = _vpath(SIZE_ANALYZE_SCRIPT)
    if not script.is_file():
        return
    out = cmake_out_dir(root)
    if not (out / "nuttx").is_file():
        return
    try:
        subprocess.run(
            [
                sys.executable,
                str(script),
                "--root",
                str(root),
                "--config",
                board_config_name(),
                "--brief",
            ],
            cwd=root,
            check=False,
        )
    except OSError as exc:
        print(f"WARN: firmware size summary skipped: {exc}", file=sys.stderr)


def tab_completion_status(root: Path) -> bool | None:
    """检查 Tab 补全是否已在 defconfig 或已构建的 .config 中启用。"""
    cfg = board_config_path(root)
    defconfig = (cfg / "defconfig").read_text(encoding="utf-8", errors="replace")
    if "CONFIG_READLINE_TABCOMPLETION=y" in defconfig:
        return True

    dotconfig = cmake_out_dir(root) / ".config"
    if dotconfig.is_file():
        text = dotconfig.read_text(encoding="utf-8", errors="replace")
        if "CONFIG_READLINE_TABCOMPLETION=y" in text:
            return True
        if "# CONFIG_READLINE_TABCOMPLETION is not set" in text:
            return False

    if (
        "CONFIG_READLINE_TABCOMPLETION is not set" in defconfig
        or "# CONFIG_READLINE_TABCOMPLETION is not set" in defconfig
    ):
        return False
    return None


def warn_tab_completion(root: Path) -> None:
    """monitor/build 前提示 Tab 补全配置状态。"""
    status = tab_completion_status(root)
    if status is True:
        return
    if status is False:
        print(
            "警告: 未启用 CONFIG_READLINE_TABCOMPLETION，NSH Tab 补全无效。\n"
            "  1) 在 defconfig 加入: CONFIG_READLINE_TABCOMPLETION=y\n"
            "  2) 或: ./vela_my_vendor_tools.py menuconfig\n"
            "     → System Libraries and NSH → readline() → Tab completion\n"
            "  3) build 后执行 savedefconfig 写回 defconfig\n",
            file=sys.stderr,
        )
        return
    print(
        "提示: 若 Tab 补全无效，请确认 CONFIG_READLINE_TABCOMPLETION=y 并已重新 build。",
        file=sys.stderr,
    )


def job_count(cli_jobs: int | None) -> int:
    if cli_jobs is not None and cli_jobs > 0:
        return cli_jobs
    if JOBS > 0:
        return JOBS
    return os.cpu_count() or 1


def run(
    cmd: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    printable = " ".join(cmd)
    print(f"\n>>> {printable}\n")
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        cmd,
        cwd=cwd,
        env=merged,
        check=check,
        text=True,
    )


def list_serial_ports() -> list[str]:
    system = platform.system()
    ports: list[str] = []
    if system == "Darwin":
        dev = Path("/dev")
        for entry in dev.iterdir():
            name = entry.name
            if name.startswith("cu.") and (
                "usb" in name.lower() or "serial" in name.lower() or "SLAB" in name
            ):
                ports.append(str(entry))
        ports.sort()
    elif system == "Linux":
        for pattern in ("ttyUSB*", "ttyACM*"):
            ports.extend(str(p) for p in Path("/dev").glob(pattern))
        ports.sort()
    return ports


def resolve_port(port: str | None) -> str:
    if port:
        # 剥掉命令行/粘贴带进来的脏尾巴（全角逗号、顿号、引号等曾导致
        # serial_hub 去开 `/dev/ttyACM0、` 然后立刻收工）。
        port = port.strip().strip("\"'").rstrip("、，,;；")
        if port.isdigit() and platform.system() == "Windows":
            return f"COM{port}"
        return port

    ports = list_serial_ports()
    if not ports:
        raise RuntimeError(
            "未指定串口且未自动检测到设备。请使用 -p/--port，"
            "例如: -p /dev/ttyUSB0 或 -p COM19"
        )

    print("可用串口:")
    for idx, name in enumerate(ports):
        print(f"  [{idx}] {name}")

    sel = input("输入序号或直接输入端口名: ").strip()
    if sel.isdigit():
        index = int(sel)
        if 0 <= index < len(ports):
            return ports[index]
        raise RuntimeError(f"无效序号: {sel}")
    if sel:
        return sel
    raise RuntimeError("未选择串口")


def _resolve_flash_path(out: Path, spec: str) -> str:
    """将 path@addr 中的 path 解析为绝对路径（若在 out 下存在则相对 out）。"""
    if "@" not in spec:
        raise RuntimeError(f"FLASH 参数格式应为 path@address: {spec}")
    path_part, addr = spec.rsplit("@", 1)
    path = Path(path_part)
    if not path.is_absolute():
        candidate = out / path
        if candidate.is_file():
            path = candidate
    return f"{path}@{addr}"


def _flash_args_lib(root: Path) -> types.ModuleType:
    """Load vendor/my_vendor/scripts/flash_args_lib.py."""
    path = _vpath(FLASH_ARGS_LIB)
    if not path.is_file():
        raise RuntimeError(f"未找到: {path}")
    spec = importlib.util.spec_from_file_location("flash_args_lib", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载: {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ensure_flasher_args(root: Path, out: Path) -> Path | None:
    """构建后写入 cmake_out/.../flasher_args.json（地址来自 ptab.json）。"""
    if not out.is_dir():
        out.mkdir(parents=True, exist_ok=True)
    try:
        fal = _flash_args_lib(root)
        dest = fal.write_flasher_args(
            root=root,
            out=out,
            boot_config=BOOT_CONFIG,
            boot_bin_dir_spec=BOOT_BIN_DIR,
        )
        print(f"flasher_args: {dest}", file=sys.stderr)
        return dest
    except Exception as exc:
        print(f"WARN: 未生成 {FLASHER_ARGS_NAME}: {exc}", file=sys.stderr)
        return None


def _load_flash_args_uart_fallback(
    out: Path,
    *,
    include_fs: bool,
    fs_only: bool,
    include_factory: bool = False,
) -> tuple[str, str, list[str]] | None:
    sh = out / "uart_download.sh"
    if not sh.is_file():
        return None
    text = sh.read_text(encoding="utf-8")
    match = re.search(
        r'sftool\s+-p\s+"\$input"\s+-c\s+(\S+)\s+-m\s+(\S+)\s+write_flash\s+(.+)$',
        text,
        re.MULTILINE,
    )
    if not match:
        return None
    chip, memory, tail = match.group(1), match.group(2), match.group(3).strip()
    resolved: list[str] = []
    for token in tail.split():
        token = token.strip('"')
        path_part = token.split("@", 1)[0]
        is_fs = (
            Path(path_part).name == FS_ROOT_BIN_NAME
            or "fs_root_sparse" in path_part.replace("\\", "/")
        )
        is_kv = (
            Path(path_part).name == "kv_root.bin"
            or "kv_root_sparse" in path_part.replace("\\", "/")
        )
        is_fat = (
            Path(path_part).name == "fat_root.bin"
            or "fat_root_sparse" in path_part.replace("\\", "/")
        )
        is_factory = (
            Path(path_part).name == "factory.bin"
            or "nsh-factory" in path_part.replace("\\", "/")
        )
        if is_fat:
            continue
        if fs_only:
            if not is_fs:
                continue
        elif (is_fs or is_kv) and not include_fs:
            continue
        elif is_factory and not include_factory:
            continue
        if "@" in token and not Path(token.split("@", 1)[0]).is_absolute():
            resolved.append(_resolve_flash_path(out, token))
        else:
            resolved.append(token)
    if not resolved:
        return None
    return chip, memory.lower(), resolved


def resolve_flash_baud(override: int | None = None) -> int:
    """sftool -b；默认 FLASH_BAUD，可用 -fb 覆盖。"""
    return override if override is not None else FLASH_BAUD


def load_sftool_flash_args(
    out: Path,
    root: Path | None = None,
    *,
    include_fs: bool = True,
    fs_only: bool = False,
    include_factory: bool = False,
    flash_medium: str | None = None,
) -> tuple[str, str, list[str]]:
    """烧录参数：优先 flasher_args.json，否则 ptab + sftool_param 实时计算。

    include_fs=False 时跳过 fs_root.bin / kv_root.bin（普通 flash）。
    include_factory=False 时跳过 factory 槽（普通 flash；flash-all / pack 为 True）。
    fs_only=True 时仅保留 fs_root.bin（flash-fs）。
    FAT 余量卷从不走串口（pack-sd-img / burn-sd）。
    flash_medium 指定时强制按对应 ptab 重算地址（忽略 flasher_args 快照）。
    """
    if root is None:
        raise RuntimeError("load_sftool_flash_args 需要 openvela 根目录")

    fal = _flash_args_lib(root)
    storage = flash_medium
    try:
        return fal.load_flash_args(
            root=root,
            out=out,
            boot_config=BOOT_CONFIG,
            boot_bin_dir_spec=BOOT_BIN_DIR,
            include_fs=include_fs,
            fs_only=fs_only,
            include_factory=include_factory,
            storage=storage,
        )
    except (FileNotFoundError, RuntimeError, KeyError):
        pass

    uart = _load_flash_args_uart_fallback(
        out,
        include_fs=include_fs,
        fs_only=fs_only,
        include_factory=include_factory,
    )
    if uart is not None:
        return uart

    param_path = boot_config_path(root) / "sftool_param.json"
    # 按 BOOT_STORAGE 选 ptab.<medium>.json（见 flash_args_lib.PTAB_BY_STORAGE）。
    ptab_path = fal.resolve_ptab_path(boot_config_path(root))
    firmware = nuttx_firmware_for_flash(out)
    if not firmware.is_file():
        raise RuntimeError(
            f"未找到固件 {nuttx_raw_bin(out)}（或 {nuttx_flash_bin(out)}）。"
            f"请先 build / wrap；烧录配置见 {param_path}。"
        )
    if not ptab_path.is_file():
        raise RuntimeError(f"未找到分区表: {ptab_path}")

    param = fal.load_sftool_param(param_path)
    ptab_addrs = fal.load_ptab_img_addresses(ptab_path)
    if "main" not in ptab_addrs:
        raise RuntimeError(f"ptab 中无 main 分区: {ptab_path}")
    chip = param["chip"]
    memory = str(param["memory"]).lower()
    # storage.conf 优先（sd/emmc -> sftool -m sd），与主路径一致。
    storage_mem = fal.boot_storage_memory(boot_config_path(root))
    if storage_mem:
        memory = storage_mem
    addr = fal.fmt_addr(ptab_addrs["main"])
    return chip, memory, [f"{firmware.resolve()}@{addr}"]


def find_sftool() -> str:
    """查找 sftool：PATH → SIFLI_SDK_TOOLS_PATH → ~/.sifli/tools/sftool。"""
    exe = shutil.which("sftool")
    if exe:
        return exe

    tools_roots: list[Path] = []
    env_tools = os.environ.get("SIFLI_SDK_TOOLS_PATH")
    if env_tools:
        tools_roots.append(Path(env_tools))
    tools_roots.append(Path.home() / ".sifli")

    candidates: list[Path] = []
    for root in tools_roots:
        sftool_dir = root / "tools" / "sftool"
        if not sftool_dir.is_dir():
            continue
        for ver_dir in sftool_dir.iterdir():
            candidate = ver_dir / "sftool"
            if candidate.is_file():
                candidates.append(candidate)

    if candidates:
        candidates.sort(key=lambda p: p.parent.name, reverse=True)
        return str(candidates[0].resolve())

    raise RuntimeError(
        "未找到 sftool。请安装 SiFli SDK 工具链: "
        "cd $SIFLI_SDK && ./install.sh && source export.sh；"
        "或将 ~/.sifli/tools/sftool/*/sftool 加入 PATH。"
    )


def cmd_wrap_nuttx(root: Path, out: Path) -> Path:
    """Wrap nuttx.bin → nuttx.flash.bin (1KiB header + 4B CRC); nuttx.bin unchanged."""
    script = wrap_nuttx_script(root)
    if not script.is_file():
        raise RuntimeError(f"未找到 wrap 脚本: {script}")

    raw = nuttx_raw_bin(out)
    if not raw.is_file():
        raise RuntimeError(
            f"未找到 {raw}。请先: python3 {SCRIPT_BASENAME} build"
        )

    run([str(script), str(out)], cwd=root)
    wrapped = nuttx_flash_bin(out)
    if not wrapped.is_file():
        raise RuntimeError(f"wrap 未生成 {wrapped}")
    return wrapped


def _latest_ota_fw(out: Path) -> Path | None:
    ota_dir = out / "fw"
    if not ota_dir.is_dir():
        return None
    bins = [p for p in ota_dir.glob("*.bin") if p.is_file()]
    if not bins:
        return None
    bins.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return bins[0]


def cmd_pack_fw(root: Path, out: Path) -> Path:
    """Wrap nuttx.bin (OVNX 头 + CRC) and rename for /mnt/kv/fw OTA upload."""
    print(
        "\n=== pack-fw: 生成带 CRC 的 OTA 固件（拷到 /mnt/kv/fw/）===\n",
        file=sys.stderr,
    )
    wrapped = cmd_wrap_nuttx(root, out)
    ota = _latest_ota_fw(out)
    if ota is None:
        raise RuntimeError(
            f"未生成 OTA 文件 {out / 'fw'}/*.bin。请确认 wrap 已成功。"
        )
    sidecar = ota.with_suffix(".txt")
    print(
        f"\n=== OTA 固件已就绪 ===\n"
        f"  烧录镜像 : {wrapped}\n"
        f"  OTA 文件 : {ota}  ({ota.stat().st_size} B)\n"
        f"  上传路径 : /mnt/kv/fw/{ota.name}\n"
        f"  说明     : 工厂 MTP 拷到 KV 卷的 fw/，或 BLE OTA 相对路径 fw/{ota.name}\n"
        + (f"  元数据   : {sidecar}\n" if sidecar.is_file() else "")
    )
    return ota


def ensure_nuttx_flash_image(root: Path, out: Path) -> Path:
    """Build nuttx.flash.bin when missing or older than nuttx.bin."""
    if not AUTO_WRAP_NUTTX:
        return nuttx_firmware_for_flash(out)

    raw = nuttx_raw_bin(out)
    wrapped = nuttx_flash_bin(out)
    if not raw.is_file():
        return wrapped

    if not wrapped.is_file():
        return cmd_wrap_nuttx(root, out)

    if raw.stat().st_mtime > wrapped.stat().st_mtime:
        return cmd_wrap_nuttx(root, out)

    return wrapped


def cmd_build_boot(root: Path, out: Path, jobs: int) -> None:
    """Build ftab.bin + bootloader.bin via boot_loader/build.sh."""
    script = _vpath("boot_loader/build.sh")
    if not script.is_file():
        raise RuntimeError(f"未找到 boot 构建脚本: {script}")

    sdk = resolve_sifli_sdk(root)
    boot_out = boot_bin_dir(root)
    fal = _flash_args_lib(root)
    storage = fal.boot_storage_value(boot_config_path(root)) or "nand"
    env = os.environ.copy()
    env["SIFLI_SDK"] = str(sdk)
    env["BOOT_BIN_DIR"] = str(boot_out)
    # build.sh 以 storage.conf 为准；此处显式传入，避免 shell 残留 BOOT_STORAGE=sd。
    env["BOOT_STORAGE"] = storage
    print(f"INFO: build-boot BOOT_STORAGE={storage} (from {boot_config_path(root).parent.parent / 'storage.conf'})")
    run(
        [
            str(script),
            "--no-prompt",
            "--nuttx-dir", str(out),
            "-j", str(jobs),
        ],
        cwd=root,
        env=env,
    )
    ensure_flasher_args(root, out)


def _sanitized_build_env() -> dict[str, str] | None:
    """Strip SiFli-SDK entries from PYTHONPATH.

    Sourcing the SDK's export.sh (e.g. for build-boot) leaves PYTHONPATH
    pointing at .../SiFli/SDK/.../tools/build, whose resource.py shadows the
    stdlib resource module that openvela's tools/mkallsyms.py (pyelftools)
    needs, breaking the build with
    'module resource has no attribute getpagesize'. Drop those entries.
    """
    pp = os.environ.get("PYTHONPATH")
    if not pp:
        return None
    kept = [p for p in pp.split(os.pathsep)
            if p and "SiFli" not in p and "/SDK/" not in p]
    new_pp = os.pathsep.join(kept)
    if new_pp == pp:
        return None
    return {"PYTHONPATH": new_pp}


def sync_boot_storage_header(root: Path) -> None:
    """从 boot_loader/storage.conf 生成 app 侧的 my_vendor_boot_storage.h。

    单一来源：storage.conf 的 BOOT_STORAGE 既决定 2 级 bootloader / 烧录介质，
    也决定 app 侧的分区头（ptab.h 经此宏分派到 ptab_nand.h / ptab_sdmmc.h）与
    运行期 rootfs 挂载（NAND vs SDIO）。best-effort：失败仅告警，不中断构建。
    """
    macro_by_value = {
        "nand": "MY_VENDOR_BOOT_FROM_NAND",
        "sd": "MY_VENDOR_BOOT_FROM_SD",
        "emmc": "MY_VENDOR_BOOT_FROM_EMMC",
    }
    try:
        fal = _flash_args_lib(root)
        value = fal.boot_storage_value(boot_config_path(root)) or "nand"
        macro = macro_by_value.get(value)
        if macro is None:
            print(f"WARN: 未知 BOOT_STORAGE='{value}'，跳过 boot_storage 头同步",
                  file=sys.stderr)
            return
        include_dir = board_config_path(root).parent.parent / "include"
        header = include_dir / "my_vendor_boot_storage.h"
        content = (
            "#ifndef __MY_VENDOR_BOOT_STORAGE__H__\n"
            "#define __MY_VENDOR_BOOT_STORAGE__H__\n"
            "\n"
            "/*\n"
            " * Boot storage selection for the app (NuttX) side.\n"
            " *\n"
            " * AUTO-GENERATED from boot_loader/storage.conf by\n"
            " * <本固件树>/build_board.py before each `build`. Do NOT edit by\n"
            " * hand — change boot_loader/storage.conf (BOOT_STORAGE=nand|sd|emmc) and\n"
            " * rebuild.\n"
            " *\n"
            " * Exactly one of MY_VENDOR_BOOT_FROM_{NAND,SD,EMMC} is defined; it drives\n"
            " * both the partition header choice (ptab.h) and the rootfs mount path\n"
            " * (NAND vs SDIO) in board bringup.\n"
            " */\n"
            "\n"
            f"#define {macro} 1\n"
            "\n"
            "#endif /* __MY_VENDOR_BOOT_STORAGE__H__ */\n"
        )
        if not header.is_file() or header.read_text(encoding="utf-8") != content:
            header.write_text(content, encoding="utf-8")
            print(f"INFO: boot storage header -> {macro} (BOOT_STORAGE={value})")
    except (OSError, RuntimeError) as exc:
        print(f"WARN: boot_storage 头同步跳过: {exc}", file=sys.stderr)


def sync_ptab_table(root: Path) -> None:
    """从 boot_loader/config/nsh/ptab*.json 生成 app 侧 ptab_table.c/.h。

    与二级 bootloader 共用 gen_ptab_table.py；NSH 命令 ``ptab`` 打印这张表。
    best-effort：失败仅告警，不中断构建。
    """
    try:
        fal = _flash_args_lib(root)
        value = fal.boot_storage_value(boot_config_path(root)) or "nand"
        boot_cfg = boot_config_path(root)
        ptab = fal.resolve_ptab_path(boot_cfg, value)
        if not ptab.is_file():
            print(f"WARN: ptab json 不存在: {ptab}", file=sys.stderr)
            return

        fal.print_ptab_build_summary(ptab, value, stage="app-build")

        board_dir = board_config_path(root).parent.parent
        out_c = board_dir / "bsp" / "ptab_table.c"
        out_h = board_dir / "include" / "ptab_table.h"
        script_dir = root / "vendor" / "my_vendor" / "boot_loader" / "scripts"
        if str(script_dir) not in sys.path:
            sys.path.insert(0, str(script_dir))
        import gen_ptab_table as gpt  # noqa: E402

        entries = gpt.parse_entries(ptab)
        new_h = gpt.render_header(value, ptab.name, len(entries))
        new_c = gpt.render_source(value, ptab.name, entries)
        changed = False
        if not out_h.is_file() or out_h.read_text(encoding="utf-8") != new_h:
            out_h.write_text(new_h, encoding="utf-8")
            changed = True
        if not out_c.is_file() or out_c.read_text(encoding="utf-8") != new_c:
            out_c.write_text(new_c, encoding="utf-8")
            changed = True
        if changed:
            print(f"INFO: app ptab table -> {len(entries)} entries ({ptab.name})")
    except (OSError, RuntimeError, ImportError) as exc:
        print(f"WARN: ptab_table 同步跳过: {exc}", file=sys.stderr)


def ensure_nuttx_cmake_ready(root: Path) -> None:
    """CMake 构建要求 nuttx/ 内无 make 残留的 .config。"""
    nuttx = root / "nuttx"
    if not (nuttx / ".config").exists():
        return
    print(
        "WARN: nuttx/.config 来自 make 构建，会阻止 --cmake；正在清理 …",
        file=sys.stderr,
    )
    for name in (".config", ".config.old", ".version", "Makefile"):
        path = nuttx / name
        if path.exists() or path.is_symlink():
            path.unlink()


# 构建产物存档：每次 NuttX 构建成功后，把 ELF 压缩留一份到 ELF_ARCHIVE_DIR。
#
# 为什么：现场崩溃的地址（coredump / `sched_dumpstack`）只有配上**当时那一版**
# ELF 才能解析成人能看的调用栈；固件一重编，旧地址就对不上了。所以每构建一次
# 存一份，名字自带 CRC 便于认版本。
#
# 命名：`<项目名>+<config>+<日期>-<时间>+<crc32>.elf.gz`
#   my_vendor+nsh+20260917-153012+a1b2c3d4.elf.gz
# 去重：目录里已有同 CRC 的档就跳过（重编同一份源码不会堆垃圾）。
# 保留：最多 ELF_ARCHIVE_KEEP 份，按 mtime 从旧到新清理 —— **只删本目录里
#       我们自己命名的 `*.elf.gz`**，别人的东西一概不碰。
# 失败：只告警，绝不让构建失败（存档是附加功能）。
ELF_ARCHIVE_DIR = Path("/home/jinsc/SDK/vela/elf")
ELF_ARCHIVE_KEEP = 100


def _elf_crc32(path: Path) -> str:
    """流式计算 ELF 的 CRC32（几十 MB，别整个读进内存）。"""
    crc = 0
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            crc = zlib.crc32(chunk, crc)
    return f"{crc & 0xffffffff:08x}"


def archive_elf(root: Path) -> None:
    """把本次构建的 ELF 压缩存档（命名 / 去重 / 保留策略见上方注释）。"""
    elf = nuttx_elf_path(root)
    if not elf.is_file():
        return

    try:
        cfg = board_config_path(root)
        board = cfg.parent.parent.name          # 板目录名，如 my_vendor
        config = cfg.name                       # configs 目录名，如 nsh
        crc = _elf_crc32(elf)

        ELF_ARCHIVE_DIR.mkdir(parents=True, exist_ok=True)
        dup = [p for p in ELF_ARCHIVE_DIR.glob("*.elf.gz")
               if p.name.endswith(f"+{crc}.elf.gz")]
        if dup:
            print(f"ELF 存档：CRC {crc} 已有（{dup[0].name}），跳过")
            return

        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        dst = ELF_ARCHIVE_DIR / f"{board}+{config}+{stamp}+{crc}.elf.gz"
        with gzip.open(dst, "wb", compresslevel=9) as out, elf.open("rb") as src:
            shutil.copyfileobj(src, out, 1 << 20)

        ours = sorted(ELF_ARCHIVE_DIR.glob("*.elf.gz"),
                      key=lambda p: p.stat().st_mtime)
        for old in ours[: max(0, len(ours) - ELF_ARCHIVE_KEEP)]:
            old.unlink()
            print(f"ELF 存档：清理旧档 {old.name}")

        kib = dst.stat().st_size / 1024.0
        print(f"ELF 存档：{dst.name}（{kib:.0f} KiB，共 {len(ours)} 份）")
    except Exception as exc:  # noqa: BLE001 — 存档失败不能影响构建
        print(f"WARN: ELF 存档失败：{exc}")


def cmd_build_nuttx(root: Path, jobs: int) -> Path:
    """仅编译 NuttX（build.sh --cmake），返回 cmake 输出目录。"""
    out = cmake_out_dir(root)
    ensure_nuttx_cmake_ready(root)
    sync_boot_storage_header(root)
    sync_ptab_table(root)
    warn_boot_bin_missing(root, after_nuttx=True)
    warn_tab_completion(root)
    # 显式 `-b <输出目录>`：build.sh 自己推的目录名是"路径第 5 段"（板内目录名 my_vendor），
    # 本树改名后会与上面 cmake_out_dir() 算的（带本树目录名）**不一致** ⇒ 必须由这里钉死。
    # 本树在 vendor/my_vendor 时两者本来就相同（cmake_out/my_vendor_nsh），行为不变。
    run(
        ["./build.sh", board_config_arg(root), "--cmake",
         "-b", str(out), f"-j{jobs}"],
        cwd=root,
        env=_sanitized_build_env(),
    )
    archive_elf(root)
    return out


def cmd_build(root: Path, jobs: int) -> None:
    out = cmd_build_nuttx(root, jobs)
    ensure_boot_bin_after_build(root, out, jobs)
    if AUTO_WRAP_NUTTX:
        ensure_nuttx_flash_image(root, out)
    ensure_flasher_args(root, out)
    print_firmware_size_summary(root)


def cmd_build_all(root: Path, jobs: int) -> None:
    """全量：main → factory → boot → wrap → kv/lfs/fat。"""
    print(
        "\n=== build-all: main (nsh) ===\n",
        file=sys.stderr,
    )
    out = cmd_build_nuttx(root, jobs)
    nuttx = nuttx_raw_bin(out)
    if not nuttx.is_file():
        raise RuntimeError(
            f"nuttx 未生成: {nuttx}。请先解决 app 编译错误。"
        )
    print(
        "\n=== build-all: factory (nsh-factory) ===\n",
        file=sys.stderr,
    )
    cmd_build_factory(root, jobs)
    print(
        "\n=== build-all: build-boot (ftab.bin + bootloader.bin) ===\n",
        file=sys.stderr,
    )
    cmd_build_boot(root, out, jobs)
    still = boot_bin_missing(root)
    if still:
        raise RuntimeError(
            f"boot 镜像仍未就绪: {boot_bin_dir(root)}/ 缺少 {', '.join(still)}"
        )
    if AUTO_WRAP_NUTTX:
        ensure_nuttx_flash_image(root, out)
    if config_has_bicycle(root):
        print(
            "\n=== build-all: build-fs (kv + lfs + fat) ===\n",
            file=sys.stderr,
        )
        cmd_build_fs(root)
    else:
        ensure_flasher_args(root, out)
    print_firmware_size_summary(root)


def cmd_build_factory(root: Path, jobs: int) -> Path:
    """编译 Factory 固件变体（configs/nsh-factory）并 wrap 成 OVNX 镜像。

    输出 cmake_out/my_vendor_nsh-factory/nuttx.flash.bin。build-all 会编它并打进
    pack-sd-img / flash-all；也可 flash-factory 单烧 ptab "factory"。产品固件
    （BOARD_CONFIG）的构建不受影响。
    """
    global BOARD_CONFIG
    saved = BOARD_CONFIG
    BOARD_CONFIG = FACTORY_BOARD_CONFIG
    try:
        board_config_path(root)  # 校验 factory defconfig 存在
        print(
            f"\n=== build-factory: config={FACTORY_BOARD_CONFIG} "
            f"-> {factory_out_dir(root)} ===\n",
            file=sys.stderr,
        )
        out = cmd_build_nuttx(root, jobs)
        raw = nuttx_raw_bin(out)
        if not raw.is_file():
            raise RuntimeError(f"factory nuttx 未生成: {raw}（先解决 app 编译错误）")
        if AUTO_WRAP_NUTTX:
            ensure_nuttx_flash_image(root, out)
        print_firmware_size_summary(root)
    finally:
        BOARD_CONFIG = saved

    wrapped = nuttx_flash_bin(out)
    print(
        f"factory 固件: {wrapped}\n"
        f"烧录: python3 {SCRIPT_BASENAME} flash-factory"
        f"（或 pack-sd-img / flash-all）",
        file=sys.stderr,
    )
    return out


def cmd_flash_factory(
    root: Path,
    port: str | None,
    *,
    erase_all: bool = False,
    flash_medium: str | None = None,
    flash_baud: int | None = None,
) -> None:
    """只烧 Factory 固件到 ptab "factory" 分区（不动 ftab/bootloader/main/fs）。"""
    out = factory_out_dir(root)
    if not out.is_dir():
        raise RuntimeError(
            f"factory 构建目录不存在: {out}\n"
            f"请先: python3 {SCRIPT_BASENAME} build-factory"
        )
    ensure_nuttx_flash_image(root, out)
    firmware = nuttx_firmware_for_flash(out)
    if not firmware.is_file():
        raise RuntimeError(
            f"未找到 factory 固件: {nuttx_flash_bin(out)}\n"
            f"请先: python3 {SCRIPT_BASENAME} build-factory"
        )

    fal = _flash_args_lib(root)
    cfg_dir = boot_config_path(root)
    medium_key = fal.normalize_storage_medium(flash_medium)
    ptab_path = fal.resolve_ptab_path(cfg_dir, storage=medium_key)
    if not ptab_path.is_file():
        raise RuntimeError(f"未找到分区表: {ptab_path}")
    addrs = fal.load_ptab_img_addresses(ptab_path)
    if "factory" not in addrs:
        raise RuntimeError(
            f"ptab 中无 factory 分区: {ptab_path}\n"
            f"请确认 ptab.sdmmc.json 已加入 factory 区，并 build-boot 重生成分区表。"
        )
    addr = fal.fmt_addr(addrs["factory"])

    param = fal.load_sftool_param(cfg_dir / "sftool_param.json")
    chip = param["chip"]
    memory = str(param["memory"]).lower()
    if medium_key:
        memory = medium_key
    else:
        storage_mem = fal.boot_storage_memory(cfg_dir)
        if storage_mem:
            memory = storage_mem

    baud = resolve_flash_baud(flash_baud)
    flash_args = [f"{firmware.resolve()}@{addr}"]
    _flash_sftool(
        out,
        port,
        chip,
        memory,
        flash_args,
        flash_baud=baud,
        erase_all=erase_all,
        summary_lines=[
            f"  sftool -b {baud}",
            f"  {firmware} @ {addr}  (ptab factory)",
            "  (只烧 factory；产品固件请用 flash / flash-all)",
        ],
        root=root,
        flash_medium=flash_medium,
    )


def cmd_menuconfig(root: Path) -> None:
    run(
        ["./build.sh", board_config_arg(root), "--cmake",
         "-b", str(cmake_out_dir(root)), "menuconfig"],
        cwd=root,
    )


def cmd_savedefconfig(root: Path) -> None:
    run(
        ["./build.sh", board_config_arg(root), "--cmake",
         "-b", str(cmake_out_dir(root)), "savedefconfig"],
        cwd=root,
    )


def _flash_args_include_fs(flash_args: list[str], root: Path) -> bool:
    return _flash_args_lib(root).flash_args_include_fs(flash_args)


def _flash_args_include_factory(flash_args: list[str], root: Path) -> bool:
    fal = _flash_args_lib(root)
    for spec in flash_args:
        if fal.is_factory_flash_path(spec.split("@", 1)[0]):
            return True
    return False


def _erase_fs_region(
    port: str | None,
    chip: str,
    memory: str,
    root: Path,
    *,
    flash_baud: int,
    flash_medium: str | None = None,
) -> None:
    """Erase FS_REGION before sparse fs_root flash (NAND must erase before rewrite)."""
    fal = _flash_args_lib(root)
    try:
        region = fal.fs_region_erase_region(
            root, BOOT_CONFIG, storage=flash_medium,
        )
    except RuntimeError as exc:
        print(f"\n=== 跳过 FS 整区擦除（{exc}）===\n", file=sys.stderr)
        return
    # "0xADDR:0xSIZE"
    try:
        _addr_s, size_s = region.split(":", 1)
        region_size = int(size_s, 0)
    except ValueError:
        region_size = 0
    # Multi-GiB SD FS_REGION: serial erase_region is impractical; sparse write
    # + pack-sd-img / blank card is the intended path.
    if memory == "sd" and region_size >= 512 * 1024 * 1024:
        print(
            f"\n=== 跳过 FS 整区擦除 {region} "
            f"（{region_size / (1024**3):.2f} GiB，串口擦除不现实）===\n"
            f"  SD 请用 pack-sd-img + dd/Etcher；或确认卡上 FS 区为空白后 flash-fs。\n",
            file=sys.stderr,
        )
        return

    sftool = find_sftool()
    resolved = resolve_port(port)
    print(
        f"\n=== 擦除 FS 分区 {region}（sparse 烧录前清空，与 mkfs 完全一致）===\n",
        file=sys.stderr,
    )
    _prepare_rom_download(resolved, flash_baud)
    run(
        _sftool_cmd(
            sftool,
            resolved,
            flash_baud,
            chip,
            memory,
            ["erase_region", region],
        ),
        cwd=root,
    )


def _prepare_rom_download(port: str, baud: int) -> None:
    """Pin-reset into Mask ROM's ~1 s UART Debug-IP window, then release the port.

    SF32LB52 has no BOOT pin. ROM only waits for sftool's ATSF32 after a *pin*
    reset (monitor's RTS pulse).  msh ``reboot`` uses HAL_PMU_Reboot() which
    sets PMUC_CR_REBOOT; A4+ ROM then skips that window and 2SFBL msh eats
    ``~ATSF32!``.  USB-CDC often ignores sftool's RTS-only toggle; this uses
    the same DTR/RTS pulse as ``monitor``.
    """
    import serial
    from serial.serialutil import SerialException

    print(
        "硬件复位进入 ROM 下载窗口（monitor 若在跑，flash 会先请它让出串口；"
        "不要在 msh 里 reboot）...",
        file=sys.stderr,
    )
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dsrdtr = False
    ser.rtscts = False
    try:
        ser.dtr = False
        ser.rts = False
        ser.open()
    except SerialException as exc:
        raise RuntimeError(
            f"无法打开 {port} 做复位: {exc}\n"
            "请先退出 monitor / 其它占用该串口的进程后再 flash。"
        ) from exc
    try:
        _pulse_reset(ser, boot_dtr=False, hold_s=0.1)
        ser.dtr = False
        ser.rts = False
    finally:
        ser.close()


def _sftool_supports_no_reset_no_sync(sftool: str) -> bool:
    """True when this sftool can skip stub re-download (needed for fast ACM flash)."""
    try:
        proc = subprocess.run(
            [sftool, "--help"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return "no_reset_no_sync" in ((proc.stdout or "") + (proc.stderr or ""))


def _sftool_cmd(
    sftool: str,
    port: str,
    baud: int,
    chip: str,
    memory: str,
    extra: list[str],
    *,
    compat: bool = True,
    before: str = "no_reset",
    after: str | None = None,
) -> list[str]:
    # no_reset: we already pin-reset; sftool RTS-only often does nothing on ACM.
    cmd = [
        sftool,
        "-p",
        port,
        "-b",
        str(baud),
        "-c",
        chip,
        "-m",
        memory,
        "--before",
        before,
    ]
    if after is not None:
        cmd.extend(["--after", after])
    cmd.extend(
        [
            "--connect-attempts",
            "10",
            "--compat",
            "true" if compat else "false",
            *extra,
        ]
    )
    return cmd


def _monitor_ctl_op(
    op: str, *, port: str | None = None, timeout: float = 3.0,
    pulse: bool = False,
) -> bool:
    """经控制通道请正在跑的 hub 让出/收回串口。

    没有 hub（信物/abstract 不存在）时返回 False —— 调用方按"串口没人占"处理。
    """
    resolved = resolve_port(port) if port else None
    path = default_monitor_ctl_sock(resolved)
    try:
        req = {"op": op, "port": resolved or ""}
        if pulse:
            req["pulse"] = True
        resp = _monitor_ctl_request(
            path, req, timeout=timeout, port=resolved
        )
    except Exception:
        return False
    return bool(resp.get("ok"))


def _flash_sftool(
    out: Path,
    port: str | None,
    chip: str,
    memory: str,
    flash_args: list[str],
    *,
    flash_baud: int,
    erase_all: bool = False,
    summary_lines: list[str] | None = None,
    root: Path | None = None,
    flash_medium: str | None = None,
) -> None:
    """烧录入口：整段流程独占串口。

    有 monitor 在跑时**先经控制通道请它让出**（`{"op":"release"}`），结束后
    自动还回去（`{"op":"acquire"}`）：monitor 关 fd、挂起读循环，RX 环和套接字
    全程保留，所以烧完还能接着看日志、`monitor-ctl` 也不用重连。没有 monitor
    时是一条 no-op —— 老的"先关掉 monitor"仍然能用，只是不再需要。
    """
    released = _monitor_ctl_op("release", port=resolve_port(port))
    if released:
        print(
            "monitor 已让出串口（日志缓冲保留，烧完自动收回；"
            "它也会在让出超时后自己收回）"
        )
    try:
        _flash_sftool_locked(
            out,
            port,
            chip,
            memory,
            flash_args,
            flash_baud=flash_baud,
            erase_all=erase_all,
            summary_lines=summary_lines,
            root=root,
            flash_medium=flash_medium,
        )
    finally:
        if released and _monitor_ctl_op(
            "acquire", port=resolve_port(port), pulse=True
        ):
            print("monitor 已收回串口并补了一次复位，继续监视（开机日志接着抓）")


def _flash_sftool_locked(
    out: Path,
    port: str | None,
    chip: str,
    memory: str,
    flash_args: list[str],
    *,
    flash_baud: int,
    erase_all: bool = False,
    summary_lines: list[str] | None = None,
    root: Path | None = None,
    flash_medium: str | None = None,
) -> None:
    resolved = resolve_port(port)

    missing: list[str] = []
    for spec in flash_args:
        path_part = spec.split("@", 1)[0]
        if not Path(path_part).is_file():
            missing.append(path_part)
    if missing:
        raise RuntimeError(
            "烧录缺少镜像文件:\n" + "\n".join(f"  - {p}" for p in missing)
        )

    if root is not None:
        _flash_args_lib(root).reject_uart_fat_args(flash_args)

    sftool = find_sftool()
    write_extra = ["write_flash"]
    if erase_all:
        write_extra.append("--erase-all")
    write_extra.extend(flash_args)

    # --compat true is required to download the RAM stub over USB-CDC (64 KiB
    # Debug-IP frames time out on ACM).  The same flag also chunks write_flash
    # payload at 256 B + 10 ms (~20 KB/s).  Two-phase: compat stub + tiny
    # erase in BOOT_RESERVE, then no_reset_no_sync write without compat.
    warmup: str | None = None
    two_phase = _sftool_supports_no_reset_no_sync(sftool)
    if two_phase and root is not None:
        fal = _flash_args_lib(root)
        try:
            warmup = fal.stub_warmup_erase_region(
                root, BOOT_CONFIG, storage=flash_medium,
            )
        except (OSError, RuntimeError, ValueError) as exc:
            print(
                f"提示: 无法选 stub 预热区（{exc}）；"
                "整段 --compat 烧录大约只有 20KB/s。",
                file=sys.stderr,
            )
            two_phase = False
    elif two_phase and root is None:
        two_phase = False

    if two_phase and warmup:
        print(
            "提示: 将先做与 monitor 相同的 RTS 复位，再立刻 sftool"
            "（SF32LB52 ROM 约 1 秒下载窗；msh reboot 无效）。"
            + f"\nsftool: -m {memory} -b {flash_baud}"
            + f"\n  1) stub  --compat true --after no_reset  erase_region {warmup}"
            + "\n  2) write --compat false --before no_reset_no_sync  write_flash"
            + "\n     （compat 只用于灌 stub；payload 不再 256B+10ms，避免 ~20KB/s）"
            + (f"\n烧录镜像:\n" + "\n".join(summary_lines) if summary_lines else "")
            + f"\n（等效于 {out.name}/uart_download.sh -p {resolved}）"
        )
        _prepare_rom_download(resolved, flash_baud)
        run(
            _sftool_cmd(
                sftool,
                resolved,
                flash_baud,
                chip,
                memory,
                ["erase_region", warmup],
                compat=True,
                before="no_reset",
                after="no_reset",
            ),
            cwd=out,
        )
        run(
            _sftool_cmd(
                sftool,
                resolved,
                flash_baud,
                chip,
                memory,
                write_extra,
                compat=False,
                before="no_reset_no_sync",
            ),
            cwd=out,
        )
        return

    if not _sftool_supports_no_reset_no_sync(sftool):
        print(
            "提示: 当前 sftool 无 --before no_reset_no_sync"
            "（需要 0.1.16+），整段使用 --compat true，写入大约 20KB/s。"
            "可安装 ~/.sifli/tools/sftool/0.1.16 。",
            file=sys.stderr,
        )
    print(
        "提示: 将先做与 monitor 相同的 RTS 复位，再立刻 sftool"
        "（SF32LB52 ROM 约 1 秒下载窗；msh reboot 无效）。"
        + f"\nsftool: -m {memory} -b {flash_baud} --before no_reset --compat true"
        + (f"\n烧录镜像:\n" + "\n".join(summary_lines) if summary_lines else "")
        + f"\n（等效于 {out.name}/uart_download.sh -p {resolved}）"
    )
    _prepare_rom_download(resolved, flash_baud)
    run(
        _sftool_cmd(sftool, resolved, flash_baud, chip, memory, write_extra),
        cwd=out,
    )


def _missing_firmware_help(
    root: Path,
    out: Path,
    *,
    missing_boot: list[str],
    missing_images: list[str],
    need_fs: bool = False,
) -> str:
    boot_dir = boot_bin_dir(root)
    boot_script = boot_loader_path(root) / "build.sh"
    lines = ["NAND 烧录缺少镜像文件:"]
    if missing_boot:
        lines.extend([
            "",
            f"boot 目录 (须先 build-boot): {boot_dir}/",
            *[f"  - 缺少 {name}" for name in missing_boot],
        ])
    if missing_images:
        lines.extend([
            "",
            "以下路径不存在:",
            *[f"  - {p}" for p in missing_images],
        ])
    lines.extend([
        "",
        "请先:",
        f"  1) python3 {SCRIPT_BASENAME} build        # nuttx.bin → cmake_out",
        f"  2) python3 {SCRIPT_BASENAME} build-boot   # ftab + bootloader",
        f"  3) python3 {SCRIPT_BASENAME} build-factory # factory 槽（pack / flash-all）",
    ])
    if need_fs:
        lines.append(
            f"  4) python3 {SCRIPT_BASENAME} build-fs     # fs_root.bin"
        )
    lines.extend([
        "或:",
        f"  {boot_script} --no-prompt --nuttx-dir {out}",
        "",
        "全量烧录（含文件系统）:",
        f"  python3 {SCRIPT_BASENAME} build-all flash-all",
    ])
    return "\n".join(lines)


def cmd_flash(
    out: Path,
    port: str | None,
    root: Path | None = None,
    *,
    erase_all: bool = False,
    flash_medium: str | None = None,
    flash_baud: int | None = None,
) -> None:
    """烧录 ftab + bootloader + nuttx，不含 fs_root.bin / factory。"""
    if not out.is_dir():
        raise RuntimeError(f"构建目录不存在: {out}，请先 build")

    root = root or find_openvela_root()
    baud = resolve_flash_baud(flash_baud)
    ensure_nuttx_flash_image(root, out)
    missing_boot = boot_bin_missing(root)

    chip, memory, flash_args = load_sftool_flash_args(
        out, root, include_fs=False, flash_medium=flash_medium,
    )

    missing_images: list[str] = []
    for spec in flash_args:
        path_part = spec.split("@", 1)[0]
        if not Path(path_part).is_file():
            missing_images.append(path_part)

    if missing_boot or missing_images:
        raise RuntimeError(
            _missing_firmware_help(
                root, out,
                missing_boot=missing_boot,
                missing_images=missing_images,
            )
        )

    boot_images = boot_bin_image_paths(root)
    app_image = nuttx_firmware_for_flash(out)
    _flash_sftool(
        out,
        port,
        chip,
        memory,
        flash_args,
        flash_baud=baud,
        erase_all=erase_all,
        summary_lines=[
            f"  sftool -b {baud}",
            f"  {boot_images['ftab.bin']}",
            f"  {boot_images['bootloader.bin']}",
            f"  {app_image}",
            f"  (不含 kv/lfs/fat / factory；LFS 用 flash-fs / flash-all，FAT 用 pack-sd-img / burn-sd，工厂槽用 flash-factory / flash-all)",
            f"  (raw {nuttx_raw_bin(out)} 未修改，供 build-boot / 调试)",
            f"  (参数: {out / FLASHER_ARGS_NAME})",
        ],
        root=root,
        flash_medium=flash_medium,
    )


def cmd_flash_all(
    out: Path,
    port: str | None,
    root: Path | None = None,
    *,
    erase_all: bool = False,
    flash_medium: str | None = None,
    flash_baud: int | None = None,
) -> None:
    """烧录 sftool_param.json 中的全部镜像（含 factory 与 fs_root.bin）。"""
    if not out.is_dir():
        raise RuntimeError(f"构建目录不存在: {out}，请先 build")

    root = root or find_openvela_root()
    baud = resolve_flash_baud(flash_baud)
    ensure_nuttx_flash_image(root, out)
    factory_out = factory_out_dir(root)
    if factory_out.is_dir() and (
        nuttx_raw_bin(factory_out).is_file()
        or nuttx_flash_bin(factory_out).is_file()
    ):
        ensure_nuttx_flash_image(root, factory_out)
    missing_boot = boot_bin_missing(root)

    chip, memory, flash_args = load_sftool_flash_args(
        out, root, include_fs=True, include_factory=True,
        flash_medium=flash_medium,
    )

    missing_images: list[str] = []
    for spec in flash_args:
        path_part = spec.split("@", 1)[0]
        if not Path(path_part).is_file():
            missing_images.append(path_part)

    if missing_boot or missing_images:
        raise RuntimeError(
            _missing_firmware_help(
                root, out,
                missing_boot=missing_boot,
                missing_images=missing_images,
                need_fs=any(
                    FS_ROOT_BIN_NAME in spec for spec in flash_args
                ),
            )
        )

    boot_images = boot_bin_image_paths(root)
    app_image = nuttx_firmware_for_flash(out)
    factory_image = nuttx_firmware_for_flash(factory_out_dir(root))
    fs_image = fs_root_bin_path(root)
    lines = [
        f"  sftool -b {baud}",
        f"  {boot_images['ftab.bin']}",
        f"  {boot_images['bootloader.bin']}",
        f"  {app_image}",
        f"  (参数: {out / FLASHER_ARGS_NAME})",
    ]
    if _flash_args_include_factory(flash_args, root):
        lines.append(f"  {factory_image}  (factory)")
    if _flash_args_include_fs(flash_args, root):
        lines.append(f"  {fs_image}")
    lines.append("  (不含 fat_root；/mnt/fat 请 pack-sd-img / burn-sd)")
    if _flash_args_include_fs(flash_args, root):
        _erase_fs_region(
            port, chip, memory, root,
            flash_baud=baud, flash_medium=flash_medium,
        )
    _flash_sftool(
        out, port, chip, memory, flash_args,
        flash_baud=baud,
        erase_all=erase_all,
        summary_lines=lines,
        root=root,
        flash_medium=flash_medium,
    )


def cmd_pack_sd_img(root: Path, out: Path) -> Path:
    """打包 SD 启动整盘 raw .img（供 Etcher / dd / Rufus DD，非 sftool）。"""
    scripts = root / "vendor" / "my_vendor" / "scripts"
    sys.path.insert(0, str(scripts))
    import pack_sd_img as psi  # noqa: E402

    ensure_nuttx_flash_image(root, out)
    factory_out = factory_out_dir(root)
    factory_fw = nuttx_firmware_for_flash(factory_out)
    if not factory_fw.is_file():
        if nuttx_raw_bin(factory_out).is_file():
            ensure_nuttx_flash_image(root, factory_out)
            factory_fw = nuttx_firmware_for_flash(factory_out)
    if not factory_fw.is_file():
        raise RuntimeError(
            f"未找到 factory 固件: {nuttx_flash_bin(factory_out)}\n"
            f"请先: python3 {SCRIPT_BASENAME} build-all"
            f"（或 python3 {SCRIPT_BASENAME} build-factory）"
        )
    if fs_root_missing(root):
        raise RuntimeError(
            f"未找到 {fs_root_bin_path(root)}，请先: "
            f"python3 {SCRIPT_BASENAME} build-fs"
        )
    img = psi.pack_sd_img(root, out)
    print(
        f"\n=== pack-sd-img 完成 ===\n"
        f"  镜像: {img}\n"
        f"\n可以用下面任一方式刻录到 SD 卡（确认 /dev/sdX，会覆盖卡头）:\n"
        f"  python3 {SCRIPT_BASENAME} burn-sd --sd /dev/sdX\n"
        f"  python3 {SCRIPT_BASENAME} burn-sd --sd /dev/sdX --yes\n"
        f"  python3 {SCRIPT_BASENAME} burn-sd --sd /dev/sdX --repack --yes\n"
        f"\n不带 --sd 运行 burn-sd 会列出可写的移动磁盘。"
        f"前 9 MiB 为 boot+main+factory 实心；LFS 仍是稀疏段。\n",
        file=sys.stderr,
    )
    return img


def _sd_img_path(root: Path) -> Path:
    return root / "vendor" / "my_vendor" / "boot_loader" / "bin" / "my_vendor_sd.img"


def _sd_img_sources_newer(root: Path, out: Path, img: Path) -> list[Path]:
    """Firmware/boot/fs files newer than the packed .img (need --repack)."""
    if not img.is_file():
        return []
    stamp = img.stat().st_mtime
    boot = root / "vendor" / "my_vendor" / "boot_loader" / "bin"
    candidates = [
        out / NUTTX_FLASH_BIN_NAME,
        out / NUTTX_BIN_NAME,
        factory_out_dir(root) / NUTTX_FLASH_BIN_NAME,
        factory_out_dir(root) / NUTTX_BIN_NAME,
        boot / "ftab.bin",
        boot / "bootloader.bin",
        boot / "fs_root.bin",
    ]
    newer: list[Path] = []
    for path in candidates:
        try:
            if path.is_file() and path.stat().st_mtime > stamp + 1:
                newer.append(path)
        except OSError:
            continue
    return newer


def _run_dd(args: list[str], root: Path) -> None:
    cmd = ["dd", *args]
    if hasattr(os, "geteuid") and os.geteuid() != 0:
        cmd = ["sudo", "-E", *cmd]
    run(cmd, cwd=root)


def _lsblk_json() -> dict:
    try:
        proc = subprocess.run(
            ["lsblk", "-J", "-b", "-o", "NAME,PATH,SIZE,TYPE,RM,MODEL,TRAN,MOUNTPOINT"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(f"无法运行 lsblk: {exc}") from exc
    return json.loads(proc.stdout or "{}")


def _iter_lsblk_devices(node: dict, parent: dict | None = None):
    yield node, parent
    for child in node.get("children") or []:
        yield from _iter_lsblk_devices(child, node)


def list_sd_burn_candidates() -> list[dict]:
    """Removable / USB whole disks suitable for burn-sd."""
    data = _lsblk_json()
    out: list[dict] = []
    for blk in data.get("blockdevices") or []:
        for node, _parent in _iter_lsblk_devices(blk):
            if node.get("type") != "disk":
                continue
            rm = bool(node.get("rm"))
            tran = (node.get("tran") or "").lower()
            if not (rm or tran in ("usb", "mmc", "sd")):
                continue
            path = node.get("path") or f"/dev/{node.get('name')}"
            size = int(node.get("size") or 0)
            out.append(
                {
                    "path": path,
                    "name": node.get("name"),
                    "size": size,
                    "model": (node.get("model") or "").strip(),
                    "tran": tran or "?",
                    "rm": rm,
                }
            )
    return out


def _device_mountpoints(dev: Path) -> list[str]:
    """Mounted paths on whole disk or any of its partitions."""
    data = _lsblk_json()
    mounts: list[str] = []
    target = str(dev.resolve())
    base = target

    for blk in data.get("blockdevices") or []:
        for node, parent in _iter_lsblk_devices(blk):
            path = node.get("path") or f"/dev/{node.get('name')}"
            if path == target or (
                parent
                and (parent.get("path") or f"/dev/{parent.get('name')}") == base
            ):
                mp = node.get("mountpoint")
                if mp:
                    mounts.append(str(mp))
    return mounts


def _is_partition_node_name(name: str) -> bool:
    """True for partition nodes like sdb1 / mmcblk0p1 / nvme0n1p2."""
    if re.match(r"^(sd|vd|hd)[a-z]+\d+$", name):
        return True
    if re.match(r"^mmcblk\d+p\d+$", name):
        return True
    if re.match(r"^nvme\d+n\d+p\d+$", name):
        return True
    return False


def _validate_burn_sd_device(device: str) -> Path:
    dev = Path(device).expanduser()
    if not dev.is_absolute():
        dev = Path("/dev") / device
    try:
        dev = dev.resolve(strict=True)
    except FileNotFoundError as exc:
        raise RuntimeError(f"设备不存在: {device}") from exc

    if not dev.is_block_device():
        raise RuntimeError(f"不是块设备: {dev}")

    if _is_partition_node_name(dev.name):
        raise RuntimeError(
            f"请指定整盘（如 /dev/sdb 或 /dev/mmcblk0），不要用分区 {dev}"
        )

    data = _lsblk_json()
    matched: dict | None = None
    for blk in data.get("blockdevices") or []:
        for node, _parent in _iter_lsblk_devices(blk):
            path = node.get("path") or f"/dev/{node.get('name')}"
            if path == str(dev):
                matched = node
                break
        if matched:
            break

    if matched is None:
        raise RuntimeError(f"lsblk 未找到设备: {dev}")
    if matched.get("type") != "disk":
        raise RuntimeError(
            f"{dev} 类型为 {matched.get('type')!r}，请选择 TYPE=disk 的整盘"
        )

    mounts = _device_mountpoints(dev)
    dangerous = {"/", "/boot", "/boot/efi", "/home", "/usr"}
    hit = sorted(dangerous.intersection(mounts))
    if hit:
        raise RuntimeError(
            f"拒绝刻录 {dev}：其上挂载了系统分区 {', '.join(hit)}"
        )

    size = int(matched.get("size") or 0)
    if size < 64 * 1024 * 1024:
        raise RuntimeError(f"设备过小 ({size} bytes): {dev}")

    return dev


def _print_burn_sd_candidates() -> None:
    cands = list_sd_burn_candidates()
    print("可刻录候选（可移动 / USB / MMC）：")
    if not cands:
        print("  （未发现，请用 lsblk 确认后手动指定 --sd /dev/...）")
        return
    for c in cands:
        gib = c["size"] / (1024**3)
        model = c["model"] or "-"
        print(
            f"  {c['path']:<14} {gib:6.2f} GiB  "
            f"tran={c['tran']:<4} rm={int(c['rm'])}  {model}"
        )
    print(
        f"\n示例: python3 {SCRIPT_BASENAME} burn-sd --sd /dev/sdX --yes"
    )


def _unmount_sd_device(dev: Path) -> None:
    """Unmount any filesystems on the disk or its partitions."""
    data = _lsblk_json()
    target = str(dev)
    to_unmount: list[tuple[str, str]] = []  # (block, mountpoint)

    for blk in data.get("blockdevices") or []:
        for node, parent in _iter_lsblk_devices(blk):
            path = node.get("path") or f"/dev/{node.get('name')}"
            parent_path = None
            if parent:
                parent_path = parent.get("path") or f"/dev/{parent.get('name')}"
            if path == target or parent_path == target:
                mp = node.get("mountpoint")
                if mp:
                    to_unmount.append((path, str(mp)))

    for blk, mp in to_unmount:
        print(f"INFO: umount {mp} ({blk})", file=sys.stderr)
        subprocess.run(["udisksctl", "unmount", "-b", blk], check=False)
        subprocess.run(["umount", mp], check=False)


def cmd_burn_sd(
    root: Path,
    out: Path,
    *,
    device: str | None,
    image: str | Path | None = None,
    yes: bool = False,
    repack: bool = False,
) -> None:
    """将 SD 启动 .img 用 dd 写入整盘（LBA0）。默认 my_vendor_sd.img。"""
    if not device:
        _print_burn_sd_candidates()
        return

    dev = _validate_burn_sd_device(device)
    default_img = _sd_img_path(root)

    if image is not None:
        img = Path(image).expanduser()
        if not img.is_absolute():
            # Prefer CWD, then openvela root.
            cand = Path.cwd() / img
            img = cand if cand.is_file() else (root / img)
        img = img.resolve()
        if repack:
            print(
                "WARN: 已指定 --image，忽略 --repack（不会覆盖自定义镜像）",
                file=sys.stderr,
            )
    else:
        img = default_img
        stale = _sd_img_sources_newer(root, out, img) if img.is_file() else []
        if repack or not img.is_file() or stale:
            why = "重新" if (repack or stale) else ""
            if stale and not repack:
                names = ", ".join(p.name for p in stale)
                print(
                    f"INFO: {img.name} 比 {names} 旧，自动重新 pack-sd-img\n",
                    file=sys.stderr,
                )
            print(
                f"\n=== {why}打包 SD 镜像 ===\n",
                file=sys.stderr,
            )
            img = cmd_pack_sd_img(root, out)

    if not img.is_file():
        raise RuntimeError(
            f"未找到镜像: {img}\n"
            f"  默认路径: {default_img}\n"
            f"  或先: python3 {SCRIPT_BASENAME} pack-sd-img\n"
            f"  或用: --image /path/to/xxx.img"
        )

    img_size = img.stat().st_size
    try:
        blocks = os.stat(img).st_blocks * 512
    except (AttributeError, OSError):
        blocks = img_size

    print(
        f"\n=== burn-sd ===\n"
        f"  image   : {img}\n"
        f"  logical : {img_size} bytes ({img_size / (1024**2):.1f} MiB)\n"
        f"  on-disk : ~{blocks} bytes ({blocks / (1024**2):.1f} MiB)\n"
        f"  device  : {dev}\n",
        file=sys.stderr,
    )
    subprocess.run(["lsblk", str(dev)], check=False)

    sysf = Path("/sys/class/block") / Path(dev).name / "size"
    if sysf.is_file():
        card_bytes = int(sysf.read_text().strip()) * 512
        print(
            f"  card    : {card_bytes} bytes ({card_bytes / (1024**2):.0f} MiB)\n"
            f"  image   : {img_size} bytes ({img_size / (1024**2):.0f} MiB)",
            file=sys.stderr,
        )
        if img_size > card_bytes:
            raise RuntimeError(
                f"这张卡只有 {card_bytes / (1024**2):.0f} MiB，"
                f"镜像逻辑大小 {img_size / (1024**2):.0f} MiB。"
                f"请换更大的卡，或按该卡 CSD 重打 "
                f"（8GB 空图用 my_vendor_sd_8g.img）。"
            )

    if not yes:
        print(
            f"\n警告: 将清空 {dev} 开头数据并写入固件镜像。\n"
            f"确认请输入大写 YES 后回车（或加 --yes 跳过）：",
            file=sys.stderr,
        )
        try:
            answer = input().strip()
        except EOFError as exc:
            raise RuntimeError("无交互输入，请加 --yes") from exc
        if answer != "YES":
            raise RuntimeError("已取消（未输入 YES）")

    _unmount_sd_device(dev)

    # Phase 1: dense boot+main+factory (1 MiB boot + 4 MiB main + 4 MiB factory).
    # Do not use conv=sparse here: GNU dd seeks over NUL *inside* a 1 MiB block.
    scripts = root / "vendor" / "my_vendor" / "scripts"
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))
    import pack_sd_img as psi  # noqa: E402

    boot_mib = psi.BOOT_APP_DENSE_BYTES // (1024 * 1024)
    kv_mib = 256
    print(
        f"\n=== 开始刻录 ===\n"
        f"  1) 前 {boot_mib} MiB 密写（ftab/bootloader/main/factory）\n"
        f"  2) {kv_mib} MiB 起 sparse 写 kv / lfs / map\n",
        file=sys.stderr,
    )
    _run_dd(
        [
            f"if={img}",
            f"of={dev}",
            "bs=1M",
            f"count={boot_mib}",
            "conv=fsync",
            "status=progress",
        ],
        root,
    )
    if img_size > kv_mib * 1024 * 1024:
        _run_dd(
            [
                f"if={img}",
                f"of={dev}",
                "bs=1M",
                f"skip={kv_mib}",
                f"seek={kv_mib}",
                "conv=fsync,sparse",
                "status=progress",
            ],
            root,
        )
    subprocess.run(["sudo", "blockdev", "--rereadpt", str(dev)], check=False)
    print(
        f"\n完成: {img} -> {dev}\n"
        f"请将卡插入板子（SD 启动）。"
        f"不要用 conv=sparse 一次性 dd 整张镜像。\n",
        file=sys.stderr,
    )


def cmd_flash_fs(
    out: Path,
    port: str | None,
    root: Path | None = None,
    *,
    erase_all: bool = False,
    flash_medium: str | None = None,
    flash_baud: int | None = None,
) -> None:
    """仅烧录文件系统分区 fs_root.bin。"""
    root = root or find_openvela_root()
    fal = _flash_args_lib(root)
    cfg = boot_config_path(root)
    storage = flash_medium or fal.boot_storage_value(cfg) or "nand"
    ptab = fal.resolve_ptab_path(cfg, storage=storage)
    if fal.fs_region_is_remainder(ptab):
        print(
            "FS_REGION max_size=0（运行时按 CSD 剩余空间）。"
            "FAT 余量卷请用 pack-sd-img，不要 serial flash-fs 整段。",
            file=sys.stderr,
        )
        return

    baud = resolve_flash_baud(flash_baud)
    fs_bin = fs_root_bin_path(root)
    if not fs_bin.is_file():
        raise RuntimeError(
            f"未找到 {fs_bin}，请先: python3 {SCRIPT_BASENAME} build-fs"
        )

    flash_out = out if out.is_dir() else boot_config_path(root)
    chip, memory, flash_args = load_sftool_flash_args(
        flash_out, root, fs_only=True, flash_medium=flash_medium,
    )
    fs_summary = flash_args[0] if flash_args else str(fs_bin)

    if _flash_args_include_fs(flash_args, root):
        _erase_fs_region(
            port, chip, memory, root,
            flash_baud=baud, flash_medium=flash_medium,
        )

    _flash_sftool(
        flash_out,
        port,
        chip,
        memory,
        flash_args,
        flash_baud=baud,
        erase_all=erase_all,
        summary_lines=[f"  sftool -b {baud}", f"  {fs_summary}"],
        root=root,
        flash_medium=flash_medium,
    )


_MONITOR_EOL_MAP = {"cr": "CR", "lf": "LF", "crlf": "CRLF"}
_MONITOR_EOL_MINITERM = {"cr": "cr", "lf": "lf", "crlf": "crlf"}


def _run_miniterm_subprocess(
    resolved: str,
    baud: int,
    *,
    echo: bool,
    eol: str,
    dtr: bool = MONITOR_DTR,
    rts: bool = MONITOR_RTS,
) -> None:
    """退回 pyserial 自带 miniterm CLI（无 bttool 智能处理）。"""
    eol_arg = _MONITOR_EOL_MAP.get(eol.lower())
    if eol_arg is None:
        raise RuntimeError(f"无效 MONITOR_EOL={eol!r}，可选: cr | lf | crlf")

    args = [
        sys.executable,
        "-m",
        "serial.tools.miniterm",
        resolved,
        str(baud),
        "--eol",
        eol_arg,
        "--dtr",
        "1" if dtr else "0",
        "--rts",
        "1" if rts else "0",
    ]
    if MONITOR_RAW:
        args.append("--raw")
    if MONITOR_FILTER:
        args.extend(["-f", MONITOR_FILTER])
    if echo:
        args.append("-e")
    subprocess.run(args, check=True)


def _pulse_reset(ser, *, boot_dtr: bool = False, hold_s: float = 0.1) -> None:
    """给芯片一个硬件复位脉冲，使其正常启动（不进下载模式）。

    SF32LB52 的 CH340 自动下载电路：DTR 接 BOOT 选择、RTS 接 RESET(EN)。
    要正常启动：BOOT 保持运行态(dtr=boot_dtr，默认 False)，RTS 拉一下复位再
    松开，产生复位边沿。sftool 用同样的线复位芯片，故此处可靠。
    复位后 bootrom 会重新打印 SFBL，随后跳转到 app（应能看到 A/B/...）。
    """
    import time as _time
    try:
        ser.dtr = boot_dtr      # BOOT = run
        ser.rts = True          # assert RESET
        _time.sleep(hold_s)
        ser.rts = False         # release RESET -> 正常启动
        _time.sleep(0.02)
    except Exception as exc:
        print(f"复位脉冲失败(忽略): {exc}")


def default_monitor_ctl_sock(port: str | None = None) -> Path:
    """串口**信物**路径（/tmp 唯一落盘标识）。可用 VELA_SERIAL_HUB_LEASE 覆盖。

    2026-09-22：信物名 = `vela-serial-<dev>-<hash8>.lease`，hash 来自
    serial_hub.py 绝对路径 + 串口绝对路径，避免多份 SDK 撞车。多路复用是
    abstract `@vela-serial-<dev>-<hash8>`。本函数仍返回 Path；实际 connect
    走 `_mux_connect_target()`。
    """
    env = os.environ.get("VELA_SERIAL_HUB_LEASE", "").strip() or os.environ.get(
        "VELA_MONITOR_SOCK", ""
    ).strip()
    if env:
        p = Path(env).expanduser()
        # 旧环境变量若仍指向 .sock，改写成对应 .lease
        if p.suffix == ".sock":
            return p.with_suffix(".lease")
        return p

    if port:
        return _serial_hub_mod().lease_path_for(port)

    tool = Path(__file__).resolve()
    tag = hashlib.sha1(str(tool).encode("utf-8")).hexdigest()[:8]
    return Path(f"/tmp/vela_my_vendor_tools-{tag}-default.lease")


def _patch_serial_tx(ser, lock: threading.RLock | None = None) -> threading.RLock:
    """Wrap ser.write: mutex + chunked TX so 1 Mbps USB-UART does not overrun MCU RX."""
    if lock is None:
        lock = threading.RLock()
    orig = ser.write

    def paced_write(data, _orig=orig, _lock=lock, _ser=ser):
        raw = data if isinstance(data, (bytes, bytearray)) else bytes(data)
        with _lock:
            if len(raw) <= MONITOR_TX_CHUNK:
                n = _orig(raw)
            else:
                n = 0
                off = 0
                while off < len(raw):
                    part = raw[off : off + MONITOR_TX_CHUNK]
                    n += _orig(part)
                    off += len(part)
                    if off < len(raw):
                        time.sleep(MONITOR_TX_GAP_S)
            try:
                _ser.flush()
            except Exception:
                pass
            return n

    ser.write = paced_write
    return lock


class MonitorBridge:
    """串口 TX 互斥 + RX 环形缓冲，供 miniterm 与 monitor-ctl 共用。"""

    def __init__(self, rx_ring: int = MONITOR_CTL_RX_RING):
        self._lock = threading.RLock()
        self._rx_cv = threading.Condition(self._lock)
        self._ser = None
        self._rx = bytearray()
        self._rx_ring = rx_ring
        self.port = ""
        self.baud = 0
        # 让出/收回（见 release/acquire）。released=True 时 fd 已关、读循环挂起，
        # 但 RX 环与套接字照旧 —— 这正是"烧录时 monitor 不死"的实现。
        self.released = False
        self.released_at = 0.0
        self.release_reason = ""
        self._dtr = MONITOR_DTR
        self._rts = MONITOR_RTS
        self._last_note_text = ""
        self._last_note_ms = 0.0
        self.last_acquire_error = ""
        # 流式推送钩子（由 MonitorCtlServer 挂上）：串口每收到一段就推给
        # `monitor-ctl --follow` 之类的客户端。挂在 bridge 上而不是散在读线程里，
        # 是为了让**直连模式和 broker 模式走同一条路**：谁读串口谁就喂 feed_rx，
        # 推送自然发生。
        self.push_hook = None

    @property
    def is_open(self) -> bool:
        ser = self._ser
        return ser is not None and bool(getattr(ser, "is_open", False))

    def attach(self, ser, *, port: str, baud: int, dtr=None, rts=None) -> None:
        with self._lock:
            _patch_serial_tx(ser, self._lock)
            # miniterm 的 writer 线程直接调 ser.write：让出串口期间要"丢掉而
            # 不是抛异常"，否则一次按键就会把 writer 线程打挂（退出时看起来
            # 像 monitor 死了）。monitor-ctl 的发送走 write_tx，那条路会明确
            # 报错 —— 命令行里看得见的错误比静默丢弃有用。
            inner = ser.write
            bridge = self

            def guarded_write(data, _inner=inner, _bridge=bridge):
                if _bridge.released:
                    _bridge.note_throttled(
                        "串口已让出，丢弃一次发送（等 acquire 后重试）"
                    )
                    raw = data if isinstance(data, (bytes, bytearray)) else bytes(data)
                    return len(raw)
                return _inner(data)

            ser.write = guarded_write
            self._ser = ser
            self.port = port
            self.baud = baud
            if dtr is not None:
                self._dtr = bool(dtr)
            if rts is not None:
                self._rts = bool(rts)
            self.released = False
            self.release_reason = ""

    def detach(self) -> None:
        with self._lock:
            self._ser = None
            self.released = False

    def release(self, reason: str = "flash") -> bool:
        """把串口让给别的进程（烧录），自己挂起读循环，其余照旧。

        为什么需要：flash 要独占串口做 RTS 复位 + ROM 下载握手，两个进程同时
        开着同一个 tty 会互相抢字节。以前只能"先把 monitor 关掉"，代价是这段
        串口输出全丢、烧完还要手动重开（monitor-ctl 也就断了）。
        现在由 monitor 自己让出：关 fd、读循环空转等 acquire，**RX 环（64 KiB）
        与控制套接字原样保留** —— 烧录期间的板子输出确实收不到（fd 不在我们
        手里），但让出/收回这两条分界线会写进环里，历史也不丢。
        """
        with self._lock:
            if self.released:
                return True

            ser = self._ser
            if ser is None:
                return False

            try:
                # 不调 `cancel_read()`：它往 pyserial 的"中止读"管道里塞一个字节，
                # 而 `read()` 每次看到那个管道可读就**直接返回、不去读串口** ——
                # 万一这个字节在重开之后还留着，读通道就被毒掉了（每次读都空手
                # 而归）。关 fd 就够了：阻塞中的读会报错，读循环的守卫认得出来。
                if getattr(ser, "is_open", False):
                    ser.close()
            except Exception:
                return False

            self.released = True
            self.released_at = time.monotonic()
            self.release_reason = reason
        return True

    def acquire(self, reason: str = "", pulse: bool = False) -> bool:
        """收回串口（release 之后原地重开同一个节点）。

        两点讲究：
        - **不重放复位脉冲，也不让 open() 顺手来一次**：先置 RTS/DTR 为释放态
          再 open（pyserial 的 setter 在关闭时只记状态，open 时才打上去，默认
          RTS=True —— 本板 RTS 就是 RESET，不先置的话每次收回都变成一次复位，
          实测会把板子的 USB-CDC 打断、要等它重新枚举）。
        - **失败不停在 released**：见下面 except 里的注释。
        """
        with self._lock:
            if not self.released:
                return True

            ser = self._ser
            if ser is None:
                self.released = False
                return False

            try:
                ser.rts = False
                ser.dtr = False
                ser.open()
                ser.timeout = MONITOR_READ_TIMEOUT_S
            except Exception as exc:
                # 节点没了（板子 USB 重新枚举）或被别的东西占了：**不能停在
                # released**，否则读循环会永远等一个不会来的 acquire，monitor
                # 从此僵死。清掉标志、把串口交给正常的重连路径 —— 读线程下一轮
                # 就会看到断开，`cmd_monitor` 用 `_wait_for_serial()` 等节点回来
                # 后重新 attach。实测：复位/让出频繁时板子的 CDC 会消失 ~20 s。
                self.last_acquire_error = str(exc)
                self.released = False
                self.released_at = 0.0
                self.note_throttled(f"收回串口失败（{exc}），交给重连逻辑")
                return False

            self.released = False
            self.released_at = 0.0
            self.release_reason = ""
            self.last_acquire_error = ""

        if pulse:
            # 烧完的 sftool 是 `--after no_reset`，板子停在 stub 里不会自己起来，
            # 不补这一下就只能看到一片空白（这也是老的"烧完手动重开 monitor"
            # 顺带做到的：重开时的复位脉冲）。在这里补上，开机日志就能接着抓。
            import serial as serial_mod

            try:
                _pulse_reset(ser, boot_dtr=False)
                ser.dtr = False
                ser.rts = False
            except serial_mod.SerialException as exc:
                self.note_throttled(f"收回后复位失败（{exc}）")
            else:
                self.note("已补发复位脉冲，接着抓开机日志（从 SFBL 起）")
        return True

    def handover_expired(self) -> bool:
        """让出后超时（flash 崩了）——由读循环据此自己收回串口。"""
        with self._lock:
            if not self.released:
                return False
            return (time.monotonic() - self.released_at) >= MONITOR_HANDOVER_MAX_S

    def write_tx(self, data: bytes) -> None:
        with self._lock:
            ser = self._ser
            if self.released:
                raise RuntimeError(
                    "串口已让给 flash（monitor 处于 release 状态），"
                    "等它 acquire 之后再说 —— 或 `monitor-ctl acquire` 立刻收回"
                )
            if ser is None or not getattr(ser, "is_open", False):
                raise RuntimeError("串口未打开")
            ser.write(data)
            try:
                ser.flush()
            except Exception:
                pass

    def note(self, text: str) -> None:
        """把一条 monitor 自身的事件同时写进 RX 环（`--rx` 能看到）并打到屏幕。

        让出/收回必须留痕：否则日志里出现一段空白，看的人也分不清是"板子没
        输出"还是"我们在烧录"。
        """
        line = f"\r\n*** monitor: {text} ***\r\n".encode("utf-8")
        # **提示进屏幕与流式客户端，不进 RX 环形缓冲**：环形缓冲会被 `--rx` 和
        # attach 的 `replay` 原样交出去，提示写进去就会"回放一次、再写一次"，
        # 自我放大到最后把真正的板子数据挤出环形缓冲（2026-09-19 实测到这种
        # 反馈环：日志里全是重复的接入提示，板子数据被顶掉）。
        hook = self.push_hook
        if hook is not None:
            try:
                hook(line)
            except Exception:
                pass
        try:
            sys.stdout.write(line.decode("utf-8"))
            sys.stdout.flush()
        except Exception:
            pass

    def note_throttled(self, text: str, *, gap_s: float = 2.0) -> None:
        """同 note，但同一条消息至少间隔 gap_s 才重复（发 TX 被丢时用）。"""
        now = time.monotonic()
        with self._lock:
            if text == self._last_note_text and (now - self._last_note_ms) < gap_s:
                return
            self._last_note_text = text
            self._last_note_ms = now
        self.note(text)

    def feed_rx(self, data: bytes) -> None:
        if not data:
            return
        hook = self.push_hook
        if hook is not None:
            try:
                hook(data)
            except Exception:
                pass
        with self._rx_cv:
            self._rx.extend(data)
            extra = len(self._rx) - self._rx_ring
            if extra > 0:
                del self._rx[:extra]
            self._rx_cv.notify_all()

    def rx_snapshot(self) -> bytes:
        with self._lock:
            return bytes(self._rx)

    def send_and_collect(
        self,
        payload: bytes,
        *,
        wait_ms: int = MONITOR_CTL_WAIT_MS,
        idle_ms: int = MONITOR_CTL_IDLE_MS,
    ) -> bytes:
        wait_s = max(wait_ms, 0) / 1000.0
        idle_s = max(idle_ms, 1) / 1000.0
        with self._rx_cv:
            mark = len(self._rx)
        self.write_tx(payload)
        deadline = time.monotonic() + wait_s
        last_len = mark
        last_change = time.monotonic()
        with self._rx_cv:
            while True:
                now = time.monotonic()
                if now >= deadline:
                    break
                self._rx_cv.wait(timeout=min(idle_s, max(deadline - now, 0.01)))
                n = len(self._rx)
                if n != last_len:
                    last_len = n
                    last_change = time.monotonic()
                    continue
                if n > mark and (time.monotonic() - last_change) >= idle_s:
                    break
            return bytes(self._rx[mark:])


def _load_stream_fanout():
    """从同目录 serial_hub.py 取出 StreamFanout（进程内 broker 与独立 hub 共用）。"""
    cached = getattr(_load_stream_fanout, "_cls", None)
    if cached is not None:
        return cached
    hub_path = Path(__file__).resolve().with_name("serial_hub.py")
    spec = importlib.util.spec_from_file_location("_vela_serial_hub", hub_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载 serial_hub: {hub_path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    _load_stream_fanout._cls = mod.StreamFanout  # type: ignore[attr-defined]
    return mod.StreamFanout


class MonitorCtlServer:
    """Unix 套接字服务：JSON 行协议，把命令转发到串口。

    两种客户端：

    - **一问一答**（`monitor-ctl --status/--rx/命令`）：连上、发一条、收一条、断开；
    - **流式**（`{"op":"attach"}`，交互式 monitor 用的就是这条）：连接保持打开，
      服务端把串口收到的字节按 `{"ev":"rx","b64":…}` 推过来，客户端可以在同一条
      连接上发 `{"op":"tx","b64":…}` / `{"op":"send",…}`。**客户端断了不影响
      串口**，下次 attach 用 `replay` 把断开期间的那段补上 —— 这正是"退出 monitor
      去烧录，回来数据还在"的实现方式。
    """

    def __init__(self, path: Path, bridge: MonitorBridge):
        self.path = path
        self.bridge = bridge
        # 串口收到的字节 → 流式客户端（见 MonitorBridge.push_hook）。
        bridge.push_hook = self.push_rx
        self._sock: socket.socket | None = None
        self._alive = False
        self._thread: threading.Thread | None = None
        self._streams: set[socket.socket] = set()
        self._stream_lock = threading.Lock()
        self.on_shutdown = None
        self._idle_timer: threading.Thread | None = None
        # 与 serial_hub 同一套异步扇出：读线程/钩子只入队，慢客户端拖不死串口。
        self._fanout = _load_stream_fanout()(on_drop=self._fanout_dropped)

    def start(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                stale.settimeout(0.2)
                stale.connect(str(self.path))
            except OSError:
                try:
                    self.path.unlink()
                except OSError:
                    pass
            else:
                stale.close()
                raise RuntimeError(
                    f"控制套接字已被占用: {self.path}（已有 monitor 在跑？）"
                )

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.bind(str(self.path))
        try:
            os.chmod(self.path, 0o600)
        except OSError:
            pass
        sock.listen(8)
        sock.settimeout(0.5)
        self._sock = sock
        self._alive = True
        self._thread = threading.Thread(
            target=self._serve, name="monitor-ctl", daemon=True
        )
        self._thread.start()

    def close(self) -> None:
        self._alive = False
        try:
            self._fanout.stop()
        except Exception:
            pass
        sock = self._sock
        self._sock = None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        try:
            if self.path.exists():
                self.path.unlink()
        except OSError:
            pass

    def _fanout_dropped(self, conn: socket.socket) -> None:
        with self._stream_lock:
            self._streams.discard(conn)

    def _register_stream(self, conn: socket.socket) -> None:
        with self._stream_lock:
            self._streams.add(conn)
        self._fanout.add(conn)

    def _unregister_stream(self, conn: socket.socket) -> None:
        self._fanout.discard(conn)
        with self._stream_lock:
            self._streams.discard(conn)

    def stream_count(self) -> int:
        with self._stream_lock:
            return len(self._streams)

    def push_rx(self, data: bytes) -> None:
        """把串口新收到的字节推给所有流式客户端（交互式 monitor）。

        只入队、不在读路径上 sendall：慢客户端由 fanout 线程超时摘掉，**串口
        排空不受影响**（与 serial_hub.StreamFanout 同一套）。
        """
        if not data:
            return
        frame = (
            json.dumps(
                {"ev": "rx", "b64": base64.b64encode(data).decode("ascii")},
                ensure_ascii=False,
            )
            + "\n"
        ).encode("utf-8")
        self._fanout.push(frame)

    def _attach_stream(self, conn: socket.socket, req: dict) -> dict:
        """把这条连接变成流式客户端：先回放 replay 字节，之后实时推送。"""
        replay = int(req.get("replay") or 0)
        if replay > 0:
            history = self.bridge.rx_snapshot()[-replay:]
            if history:
                try:
                    conn.sendall(
                        (
                            json.dumps(
                                {
                                    "ev": "rx",
                                    "b64": base64.b64encode(history).decode("ascii"),
                                    "replay": len(history),
                                },
                                ensure_ascii=False,
                            )
                            + "\n"
                        ).encode("utf-8")
                    )
                except OSError:
                    return {"ok": False, "error": "回放失败"}
        self._register_stream(conn)
        self.bridge.note(
            f"monitor 客户端接入（流式，replay={replay}，现在 {self.stream_count()} 个）"
        )
        return {
            "ok": True,
            "port": self.bridge.port,
            "baud": self.bridge.baud,
            "released": self.bridge.released,
        }

    def _serve(self) -> None:
        assert self._sock is not None
        while self._alive:
            try:
                conn, _ = self._sock.accept()
            except TimeoutError:
                continue
            except OSError:
                if self._alive:
                    continue
                break
            threading.Thread(
                target=self._client, args=(conn,), daemon=True
            ).start()

    def _client(self, conn: socket.socket) -> None:
        conn.settimeout(30.0)
        stream = False

        def reply(obj: dict) -> bool:
            try:
                conn.sendall(
                    (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
                )
                return True
            except OSError:
                return False

        try:
            buf = b""
            while self._alive:
                try:
                    chunk = conn.recv(65536)
                except TimeoutError:
                    # **读超时不是断开**：流式客户端（`monitor-ctl --follow`、
                    # 交互式 monitor 的壳）平时只读不写，安静几十秒太正常了。
                    # 原来这里把超时当"连接没了"直接 break，于是任何静默超过
                    # `conn.settimeout(30)` 的客户端都被服务端踢掉 —— 这正是
                    # 2026-09-19 那个"客户端几十秒后自己断开"的真身（`--follow`
                    # 报 "与 monitor 的连接已断开"、monitor 窗口几十秒后不再上屏）。
                    continue
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.strip()
                    if not line:
                        continue
                    try:
                        req = json.loads(line.decode("utf-8"))
                        if not isinstance(req, dict):
                            raise ValueError("request must be object")
                        if str(req.get("op") or "").lower() == "attach":
                            resp = self._attach_stream(conn, req)
                            stream = bool(resp.get("ok"))
                        else:
                            resp = self._handle(req)
                    except Exception as exc:
                        resp = {"ok": False, "error": str(exc)}
                    # 流式连接：这条连接上的写由自己加锁（reader 线程也在写它）。
                    if resp is None:
                        continue
                    if not reply(resp):
                        break
                    if resp.get("_shutdown"):
                        self.on_shutdown and self.on_shutdown()
                        break
        except OSError:
            pass
        finally:
            self._unregister_stream(conn)
            try:
                conn.close()
            except OSError:
                pass
            if stream:
                self._maybe_idle_shutdown()

    def _maybe_idle_shutdown(self) -> None:
        """最后一个流式客户端走了 ⇒ 收工（**文件随之消失**）。

        用户要的语义：`monitor` / `monitor-ctl` 谁都不在了，那个 /tmp 文件就该没了
        （下次谁先跑谁再建）。给一个几秒的宽限，免得一次 `monitor-ctl --rx` 之类的
        一闪而过把串口关了又开（板子的 USB-CDC 会看到一次断开）。
        """
        if self._idle_timer is not None and self._idle_timer.is_alive():
            return

        def _wait() -> None:
            deadline = time.monotonic() + MONITOR_BROKER_IDLE_S
            while time.monotonic() < deadline:
                if not self._alive:
                    return
                if self.stream_count() > 0:
                    return
                if self.bridge.released:
                    # 串口正让给 flash：这期间没有客户端是正常的，别收工。
                    deadline = time.monotonic() + MONITOR_BROKER_IDLE_S
                time.sleep(0.2)
            if self.stream_count() > 0 or self.bridge.released:
                return
            if self.on_shutdown is not None:
                self.bridge.note("最后一个客户端退出，broker 收工（套接字随之消失）")
                self.on_shutdown()

        self._idle_timer = threading.Thread(
            target=_wait, name="monitor-idle", daemon=True
        )
        self._idle_timer.start()

    def _handle(self, req: dict) -> dict:
        op = str(req.get("op") or req.get("cmd") or "send").lower()
        if op in ("ping", "status"):
            return {
                "ok": True,
                "port": self.bridge.port,
                "baud": self.bridge.baud,
                "open": self.bridge.is_open,
                "released": self.bridge.released,
                "clients": self.stream_count(),
                "sock": str(self.path),
            }
        if op == "pulse":
            # 让**持有串口的一方**发复位脉冲（客户端手里没有线状态）。
            ser = getattr(self.bridge, "_ser", None)
            if ser is None or not getattr(ser, "is_open", False):
                return {"ok": False, "error": "串口没开，无法复位"}
            try:
                _pulse_reset(ser, boot_dtr=False)
                ser.dtr = False
                ser.rts = False
            except Exception as exc:
                return {"ok": False, "error": f"复位脉冲失败: {exc}"}
            return {"ok": True}
        if op == "shutdown":
            # 显式收工：关串口、关套接字（"对方断开"只该由这个动作造成）。
            self.bridge.note("收到 shutdown，串口与套接字即将关闭")
            return {"ok": True, "_shutdown": True}
        if op in ("release", "acquire"):
            want_port = str(req.get("port") or "").strip()
            if want_port and self.bridge.port and want_port != self.bridge.port:
                # 另一块板子的 monitor：别把它的串口让出去。
                return {
                    "ok": False,
                    "error": f"monitor 在 {self.bridge.port} 上，不是 {want_port}",
                    "released": self.bridge.released,
                }
            if op == "release":
                # 让出串口给 flash：关 fd、读循环挂起，RX 环与套接字保留。
                ok = self.bridge.release(reason="ctl")
                if ok:
                    self.bridge.note(
                        "串口已让给 flash（日志缓冲保留，烧完自动收回）"
                    )
            else:
                ok = self.bridge.acquire(
                    reason="ctl", pulse=bool(req.get("pulse"))
                )
                if ok:
                    self.bridge.note(f"串口已收回({self.bridge.port})")
                else:
                    return {
                        "ok": False,
                        "error": self.bridge.last_acquire_error
                        or "acquire failed（串口不在？已交给重连逻辑）",
                        "released": self.bridge.released,
                    }
            return {"ok": ok, "released": self.bridge.released}
        if op == "rx":
            text = self.bridge.rx_snapshot().decode("utf-8", "replace")
            return {"ok": True, "rx": text}
        if op == "tx":
            # 原始 TX（按键级、二进制安全）：`attach` 后的流式连接也走这条。
            payload = base64.b64decode(str(req.get("b64") or ""))
            if self.bridge.released:
                return {"ok": False, "error": "串口已让给 flash"}
            self.bridge.write_tx(payload)
            return {"ok": True, "len": len(payload)}
        if op == "attach":
            # 变成流式客户端：回放最近 replay 字节，之后新数据实时推送。
            return self._attach_stream(req)
        if op != "send":
            return {"ok": False, "error": f"unknown op: {op}"}

        hexdata = req.get("hex")
        if hexdata:
            payload = bytes.fromhex(str(hexdata).replace(" ", ""))
        else:
            payload = str(req.get("text") or req.get("data") or "").encode("utf-8")
            nl = req.get("nl", True)
            if nl and payload and not payload.endswith((b"\n", b"\r")):
                payload += b"\n"
            elif nl and not payload:
                payload = b"\n"
        wait_ms = int(req.get("wait_ms", MONITOR_CTL_WAIT_MS))
        idle_ms = int(req.get("idle_ms", MONITOR_CTL_IDLE_MS))
        rx = self.bridge.send_and_collect(
            payload, wait_ms=wait_ms, idle_ms=idle_ms
        )
        return {"ok": True, "rx": rx.decode("utf-8", "replace")}


class BrokerClient:
    """与 broker 的一条长连接：发 op，收 ev。

    只在**流式**模式下会收到 `ev=rx` 推送；`request()` 在等待应答时会把中途收到的
    `rx` 事件交给回调（否则流式期间的 `tx`/`send` 会读到事件而不是应答）。
    """

    def __init__(
        self,
        sock_path: Path | str,
        *,
        timeout: float = 5.0,
        port: str | None = None,
    ):
        self.path = sock_path if isinstance(sock_path, Path) else Path(str(sock_path))
        self._port = port
        # broker 刚被拉起的那几百毫秒里，abstract / lease 可能还没就绪。
        deadline = time.monotonic() + max(timeout, 2.0)
        last: Exception | None = None
        while True:
            target = _mux_connect_target(port, sock_path)
            if target is None:
                last = OSError("no mux endpoint")
                if time.monotonic() >= deadline:
                    raise last
                time.sleep(0.2)
                continue
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            try:
                sock.connect(target)
            except OSError as exc:
                sock.close()
                last = exc
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.2)
                continue
            self._sock = sock
            self._connect_target = target
            break
        self._wlock = threading.Lock()
        self._rlock = threading.Lock()
        self._buf = b""
        self._rx_cb = None
        self._closed = False
        # 泵线程（流式）和 request() 会同时在这条 socket 上读：泵线程把非事件的
        # 应答放这里，request() 先看它，避免把应答吃掉。
        self._pending: deque = deque()

    def close(self) -> None:
        self._closed = True
        try:
            self._sock.close()
        except OSError:
            pass

    def _send(self, obj: dict) -> None:
        if self._closed:
            raise RuntimeError("broker 连接已关闭")
        data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
        with self._wlock:
            self._sock.sendall(data)

    def _read_line(self, timeout: float | None = None) -> bytes | None:
        """读一行：**超时返回 None**，对端关闭/出错抛 ConnectionError。

        这两件事以前混在一起（都返回 None），结果是"板子安静 0.2 s"会被当成
        "连接断了" —— 而 `--follow` 的循环又靠这个返回值判断要不要退出。
        """
        with self._rlock:
            if timeout is not None:
                self._sock.settimeout(timeout)
            while b"\n" not in self._buf:
                try:
                    chunk = self._sock.recv(65536)
                except (TimeoutError,):
                    return None
                except OSError as exc:
                    raise ConnectionError(str(exc)) from exc
                if not chunk:
                    raise ConnectionError("broker 连接已关闭")
                self._buf += chunk
            line, self._buf = self._buf.split(b"\n", 1)
            return line

    def buffered_lines(self) -> int:
        """本地缓冲里已经攒了多少整行（用来一次把积压吃干净）。"""
        with self._rlock:
            return self._buf.count(b"\n")

    @property
    def sock_fd(self) -> int:
        return self._sock.fileno()

    def _dispatch(self, obj: dict) -> None:
        if obj.get("ev") == "rx" and self._rx_cb is not None:
            try:
                self._rx_cb(base64.b64decode(obj.get("b64") or ""))
            except Exception:
                pass

    def request(self, req: dict, *, timeout: float | None = None) -> dict:
        """一问一答；流式期间夹在中间的 rx 事件先交给回调。"""
        self._send(req)
        deadline = time.monotonic() + max(float(timeout or 5.0), 1.0)
        while time.monotonic() < deadline:
            if self._pending:
                return self._pending.popleft()
            line = self._read_line(timeout)
            if line is None:
                continue          # 超时：还没轮到应答，接着等（有界）
            try:
                obj = json.loads(line.decode("utf-8"))
            except json.JSONDecodeError:
                continue
            if obj.get("ev") == "rx":
                self._dispatch(obj)
                continue
            return obj
        raise RuntimeError("broker 应答里全是事件")

    def attach(self, replay: int, on_rx) -> dict:
        self._rx_cb = on_rx
        return self.request({"op": "attach", "replay": int(replay)}, timeout=5.0)

    def send_raw(self, payload: bytes) -> None:
        self._send({"op": "tx", "b64": base64.b64encode(payload).decode("ascii")})

    def pump(self, timeout: float | None = None) -> bool:
        """读一行并分发；**超时算正常**（返回 True），只有断线才返回 False。"""
        try:
            line = self._read_line(timeout)
        except ConnectionError:
            return False
        if line is None:
            return True
        try:
            obj = json.loads(line.decode("utf-8"))
        except json.JSONDecodeError:
            return True
        if obj.get("ev") == "rx":
            self._dispatch(obj)
        else:
            self._pending.append(obj)
        return True


class BrokerSerial:
    """给 miniterm 用的串口壳：read/write 都走 broker 的套接字。

    miniterm 只用到 `read/in_waiting/write/flush/is_open/close/timeout/port/
    baudrate/getSettingsDict` 这几个名字（见 pyserial 的 `tools/miniterm.py`：
    reader 线程只调 `in_waiting`+`read`，writer 只调 `write`），所以这个薄壳足够
    让它以为在直接操作串口。

    意义在于：**真正持有 fd 的是 broker**。交互式 monitor 只是一个客户端 ——
    退出它不会关串口（板子那头的 USB-CDC 不会看到"另一端断开"），/tmp 里的套接字
    也还在，别的工具（`monitor-ctl`、flash）接着用；下次 `monitor` 再 attach，
    断开期间的数据由 broker 的环形缓冲按 `replay` 补回来。
    """

    def __init__(
        self,
        sock_path: Path,
        *,
        replay: int = MONITOR_CLIENT_REPLAY,
        timeout: float = MONITOR_READ_TIMEOUT_S,
        port: str | None = None,
    ):
        self._sock_path = Path(sock_path)
        self._port_hint = port
        self._client = BrokerClient(sock_path, port=port)
        self._replay = replay
        self._cv = threading.Condition()
        self._buf = bytearray()
        self._cancel = False
        self._closed = False
        self.timeout = timeout
        self.port = str(sock_path)
        self.baudrate = MONITOR_BAUD
        self.bytesize = 8
        self.parity = "N"
        self.stopbits = 1
        self.rtscts = False
        self.xonxoff = False
        self.dsrdtr = False
        self._dtr = False
        self._rts = False
        self.is_open = False
        self._info: dict = {}

    # --- 端口开关 ---------------------------------------------------------
    def open(self) -> None:
        if self.is_open:
            return
        resp = self._client.attach(self._replay, self._on_rx)
        if not resp.get("ok"):
            raise RuntimeError(resp.get("error") or "attach 失败")
        self._info = resp
        self.port = str(resp.get("port") or self.port)
        self.baudrate = int(resp.get("baud") or self.baudrate)
        self.is_open = True
        # **必须有泵线程**：attach 只给一次 replay，之后的数据要靠有人读套接字。
        # 少了它就会出现"接上了、历史也补了，但屏幕上再也不动"（2026-09-19 的
        # 一次实测就是这样：clients=1 却没有任何新数据）。
        self._pump_thread = threading.Thread(
            target=self._pump_loop, name="broker-rx", daemon=True
        )
        self._pump_thread.start()

    def close(self) -> None:
        self.is_open = False
        self._closed = True
        self._client.close()
        with self._cv:
            self._cv.notify_all()

    @property
    def client(self) -> BrokerClient:
        return self._client

    def _pump_loop(self) -> None:
        """持续读 broker 的推送；连接断了就自己 attach 回去。

        自愈而不是交还给外层：broker 在，串口就在，掉的是这条客户端连接 ——
        让 miniterm 整个重来会把用户的会话（回显状态、Ctrl-S 缓冲）都丢掉，
        而重来一遍也只是为了重连同一个套接字。实测里客户端连接会在 ~20 s 处
        断一次（原因未定），自愈之后用户只看到一行提示、数据不断。
        """
        while self.is_open and not self._closed:
            try:
                if self._client.pump(timeout=0.5):
                    continue
            except Exception:
                pass
            # 断开：重连（不 replay，历史已经在本地缓冲里了）。
            for _ in range(30):
                if not self.is_open or self._closed:
                    return
                time.sleep(1.0)
                try:
                    self._client.close()
                    self._client = BrokerClient(
                        self._sock_path, port=self._port_hint
                    )
                    self._client.attach(0, self._on_rx)
                except Exception:
                    continue
                self._on_rx(
                    "\r\n*** monitor: 与 broker 断开一次，已重新接上 ***\r\n".encode(
                        "utf-8"
                    )
                )
                break
            else:
                break
        # 真连不上了：唤醒阻塞中的 read()，让外层处理。
        self._closed = True
        with self._cv:
            self._cv.notify_all()

    # --- 数据面 -----------------------------------------------------------
    def _on_rx(self, payload: bytes) -> None:
        with self._cv:
            self._buf.extend(payload)
            self._cv.notify_all()

    @property
    def in_waiting(self) -> int:
        with self._cv:
            return len(self._buf)

    def read(self, size: int = 1) -> bytes:
        deadline = time.monotonic() + max(float(self.timeout), 0.0)
        with self._cv:
            while not self._buf and not self._closed and not self._cancel:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                self._cv.wait(timeout=left)
            self._cancel = False
            if not self._buf:
                return b""
            out = bytes(self._buf[:size])
            del self._buf[:size]
            return out

    def write(self, data) -> int:
        payload = bytes(data)
        try:
            self._client.send_raw(payload)
        except Exception:
            return 0
        return len(payload)

    def flush(self) -> None:
        return None

    def cancel_read(self) -> None:
        with self._cv:
            self._cancel = True
            self._cv.notify_all()

    def getSettingsDict(self) -> dict:
        return {
            "port": self.port,
            "baudrate": self.baudrate,
            "bytesize": self.bytesize,
            "parity": self.parity,
            "stopbits": self.stopbits,
            "xonxoff": self.xonxoff,
            "rtscts": self.rtscts,
            "dsrdtr": self.dsrdtr,
            "timeout": self.timeout,
        }

    # miniterm 的菜单会读这几个状态位；broker 模式下没有意义，一律返回未置位。
    @property
    def cts(self) -> bool:
        return False

    @property
    def dsr(self) -> bool:
        return False

    @property
    def ri(self) -> bool:
        return False

    @property
    def cd(self) -> bool:
        return False

    break_condition = property(lambda self: False, lambda self, v: None)

    @property
    def dtr(self) -> bool:
        return self._dtr

    @dtr.setter
    def dtr(self, value) -> None:
        # 线状态由持有串口的一方（broker）掌握；客户端改它没有意义，只是记下来
        # 让 `_pulse_reset()` 之类的调用不报错。要复位就用 `{"op":"pulse"}`。
        self._dtr = bool(value)

    @property
    def rts(self) -> bool:
        return self._rts

    @rts.setter
    def rts(self, value) -> None:
        self._rts = bool(value)


def _console_disable_ixon(fd: int) -> bool:
    """关掉控制台 tty 的 IXON，让 Ctrl-S/Ctrl-Q 变成普通按键交给我们处理。

    pyserial 的 `Console.setup()` 只清 ICANON/ECHO/ISIG，**IXON 还开着**：
    Ctrl-S 于是被 tty 线路规程当成 XOFF —— 只停终端这一侧的输出，monitor
    仍在收、仍在 write(stdout)，写到 tty 输出队列（几 KB）满就阻塞 → 读线程
    停 → 串口内核缓冲（通常 4 KB）填满 → 板子那一段只能丢；Ctrl-Q 再把积压
    一次性倒出来。关掉 IXON 后 `SifliMiniterm` 自己实现"暂停显示"：串口照读
    （一个字节不丢），只是不往屏幕上写。

    同时清掉输出侧 **ONLCR**：板子 syslog（CONFIG_SYSLOG_CRLF）已经带 `\r\n`，
    主机 tty 再 ONLCR 一次就变成 `\r\r\n` —— 回车行距翻倍（2026-09-21）。

    退出时 pyserial 的 `Console.cleanup()` 会恢复构造时存下的 termios，
    不需要自己还原。返回 True = 已处理（或本就无需改）。
    """
    if os.name != "posix":
        return False
    try:
        import termios

        new = termios.tcgetattr(fd)
        changed = False
        if new[0] & termios.IXON:
            new[0] &= ~termios.IXON
            changed = True
        # oflag: 关掉 ONLCR，避免设备已带的 CR 再被主机翻一遍。
        if new[1] & getattr(termios, "ONLCR", 0):
            new[1] &= ~termios.ONLCR
            changed = True
        if changed:
            termios.tcsetattr(fd, termios.TCSANOW, new)
        return True
    except (ImportError, OSError, ValueError):
        return False


def _normalize_serial_newlines(data: bytes, carry: bytes = b"") -> tuple[bytes, bytes]:
    """把串口上的换行收成单一 `\r\n`，消除 `\r\r\n` 造成的大行距。

    跨 chunk 时若末尾落在 CR 上，把 CR 留下一截等下一包（避免把 `\r`|`\n`
    拆开误判）。ANSI 彩色日志里的裸 `\n` 也会补成 `\r\n`，方便关掉主机
    ONLCR 之后仍能正确回行首。
    """
    if not data and not carry:
        return b"", b""
    buf = carry + data
    if not buf:
        return b"", b""
    # 末尾连续 CR 先留下：可能是下一包 `\n` 的前半。
    i = len(buf)
    while i > 0 and buf[i - 1] == 0x0D:
        i -= 1
    body, tail = buf[:i], buf[i:]
    if not body:
        return b"", tail
    # `\r+\n` → `\r\n`；裸 `\n` → `\r\n`
    out = re.sub(br"\r*\n", b"\r\n", body)
    return out, tail


class _TtyRawStdin:
    """把 stdin 切成 raw（无本地回显、按键即得），退出时还原。

    `monitor-ctl` 第二终端以前按行读：主机 cooked+echo 会把方向键显示成
    `^[[A`、Tab 被本机补全抢走，回车还本地回显一次 —— 和 NSH readline
    叠在一起就是"大间距 + 不会补全/翻历史"。
    """

    def __init__(self, fd: int | None = None):
        self.fd = fd if fd is not None else sys.stdin.fileno()
        self._old = None

    def __enter__(self):
        if os.name != "posix" or not os.isatty(self.fd):
            return self
        try:
            import termios
            import tty

            self._old = termios.tcgetattr(self.fd)
            tty.setraw(self.fd, when=termios.TCSANOW)
            # setraw 会清 ISIG：Ctrl-C/Ctrl+] 都变成字面字节；退出键由调用方认
            # 0x1d（Ctrl+]，与 pyserial miniterm 一致），Ctrl-C 可转发给板子。
            # 输出侧一并关掉 ONLCR（与 _console_disable_ixon 一致）。
            new = termios.tcgetattr(self.fd)
            if new[1] & getattr(termios, "ONLCR", 0):
                new[1] &= ~termios.ONLCR
                termios.tcsetattr(self.fd, termios.TCSANOW, new)
        except (ImportError, OSError, ValueError):
            self._old = None
        return self

    def __exit__(self, *exc) -> None:
        if self._old is None:
            return
        try:
            import termios

            termios.tcsetattr(self.fd, termios.TCSADRAIN, self._old)
        except (ImportError, OSError, ValueError):
            pass


def _install_pause_keys(term) -> bool:
    """把 Ctrl-S / Ctrl-Q 接管成本地"暂停显示 / 恢复"。

    **必须在 `term.start()` 之前挂**：writer 线程一启动就会阻塞在第一次
    `console.getkey()` 上，那次调用绑定的是打补丁前的实现 —— 用户正好在
    那一刻按下的键会绕过暂停逻辑（实测第一个键必被漏掉）。挂的是
    `console.getkey` —— pyserial 的 writer 循环每次都调它，所以不用重写整个
    writer。IXON 在 `Console.setup()` 之前清也一样有效：setup() 只改
    ICANON/ECHO/ISIG/VMIN/VTIME，不碰 IXON。
    """
    if not _console_disable_ixon(getattr(term.console, "fd", -1)):
        print(
            "WARN: 控制台无法关闭 IXON，Ctrl-S/Ctrl-Q 仍是终端行为"
            "（暂停显示、恢复时倒出积压，期间可能丢字节）",
            file=sys.stderr,
        )
        return False

    orig = term.console.getkey

    def getkey():
        while True:
            c = orig()
            if c == "\x13":  # Ctrl-S
                term.pause_rx()
                continue
            if c == "\x11":  # Ctrl-Q
                term.resume_rx()
                continue
            return c

    term.console.getkey = getkey
    return True


def _install_term_cleanup() -> None:
    """SIGTERM/SIGHUP → 走正常退出路径（跑 finally），别把套接字文件留在地上。

    Python 默认收到 SIGTERM 直接终止，`finally` 不执行 —— 于是 `timeout`、`kill`
    或系统收尾之后 `/tmp` 里的转发文件会留着（功能上无害：下次 `start()` 探测到连不上
    会清掉；但用户要的语义是"人都走了它就该没了"）。这里把信号转成
    KeyboardInterrupt，交给既有的 try/finally 收尾（关串口、unlink 套接字与 pid）。
    """
    def _raise(_signum, _frame):
        raise KeyboardInterrupt

    for sig in (signal.SIGTERM, signal.SIGHUP):
        try:
            signal.signal(sig, _raise)
        except (ValueError, OSError):
            pass


def default_monitor_spool(port: str | None = None) -> Path:
    """broker 落盘日志路径（可用环境变量 VELA_MONITOR_SPOOL 覆盖）。"""
    env = os.environ.get("VELA_MONITOR_SPOOL", "").strip()
    if env:
        return Path(env).expanduser()
    if MONITOR_SPOOL:
        return Path(MONITOR_SPOOL).expanduser()
    if port:
        return _serial_hub_mod().spool_path_for(port)
    return Path.home() / ".cache" / "vela" / "serial" / "default.log"


def default_monitor_pidfile(port: str | None = None) -> Path:
    """broker 的 pid 文件（写在 spool 旁，不进 /tmp 信物目录）。"""
    return default_monitor_spool(port).with_suffix(".pid")


def _broker_status(
    sock_path: Path | None = None,
    *,
    port: str | None = None,
    timeout: float = 1.5,
) -> dict | None:
    """多路复用端点活着就返回 status，否则 None。"""
    try:
        client = BrokerClient(
            sock_path if sock_path is not None else default_monitor_ctl_sock(port),
            timeout=timeout,
            port=port,
        )
    except OSError:
        return None
    try:
        resp = client.request({"op": "status"}, timeout=timeout)
        return resp or None
    except Exception:
        return None
    finally:
        client.close()


def _daemonize(log_path: Path) -> bool:
    """fork 出一个脱离终端的子进程；父进程返回 False，子进程返回 True。

    刻意**不用 exec、不构造命令行**：broker 就是本进程 fork 出来的另一个 Python
    进程，继续跑 `cmd_monitor_broker()`。没有 shell、没有参数拼接，也就没有
    "用户输入进命令"这条路径。
    """
    pid = os.fork()
    if pid < 0:
        raise RuntimeError("fork 失败，无法后台启动 broker")
    if pid > 0:
        return False  # 父进程：继续走"客户端"那条路

    os.setsid()
    # 标准输入给 /dev/null，输出/错误进 broker 日志，避免污染父进程的终端。
    try:
        devnull = os.open(os.devnull, os.O_RDONLY)
        os.dup2(devnull, 0)
        out = os.open(str(log_path), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        os.dup2(out, 1)
        os.dup2(out, 2)
    except OSError:
        pass
    return True


def _ensure_broker(
    resolved: str,
    baud: int,
    *,
    dtr: bool,
    rts: bool,
    reset: bool,
    sock_path: Path,
    spool: Path,
) -> None:
    """没有持有者就 fork 拉起 serial_hub（抢 lease）。"""
    if _broker_status(sock_path, port=resolved) is not None:
        return

    # 旧名信物 / 其它 hash 上若已有活 hub，直接当已就绪（勿再拉第二份抢 TTY）。
    hubmod = _serial_hub_mod()
    peers = hubmod.iter_live_leases_for_port(resolved)
    if peers:
        path, meta = peers[0]
        print(
            f"发现已有 hub（lease={path} pid={meta.get('pid')} "
            f"sock={meta.get('sock')}），不再拉起第二份"
        )
        return

    print(f"未发现常驻 hub，后台拉起（lease={sock_path}）")
    if not _daemonize(spool.with_suffix(".broker.log")):
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if _broker_status(sock_path, port=resolved) is not None:
                return
            time.sleep(0.1)
        raise RuntimeError(f"拉起 broker 超时（lease={sock_path}）")

    hub = Path(__file__).resolve().with_name("serial_hub.py")
    argv = [
        sys.executable,
        str(hub),
        resolved,
        "-b",
        str(baud),
        "--lease",
        str(sock_path),
        "--idle-timeout",
        str(int(MONITOR_BROKER_IDLE_S)),
    ]
    if reset:
        argv.append("--reset")
    try:
        os.execv(sys.executable, argv)
    except OSError as exc:
        print(f"启动 serial_hub.py 失败（{exc}），回退为进程内持有者")
        try:
            cmd_monitor_broker(
                resolved,
                baud,
                dtr=dtr,
                rts=rts,
                reset=reset,
                ctl_sock=sock_path,
                spool=spool,
                daemon=True,
            )
        finally:
            os._exit(0)


def cmd_monitor_broker(
    port: str | None,
    baud: int,
    *,
    dtr: bool = MONITOR_DTR,
    rts: bool = MONITOR_RTS,
    reset: bool = True,
    ctl_sock: Path | None = None,
    spool: Path | None = None,
    daemon: bool = False,
) -> None:
    """常驻 broker：**唯一持有串口的一方**（前台运行；monitor 需要时会自己拉一个）。

    为什么要有它：交互式 monitor 会被反复启动/退出（尤其烧录时要让出串口）。
    如果串口和套接字都绑在那个会退出的进程上，那么

    - 退出 monitor = 串口被关 → 板子的 USB-CDC 看到"另一端断开"；
    - `/tmp` 里的套接字随进程消失 → 别的工具（monitor-ctl / agent）断线；
    - 退出期间的数据（含烧录后的开机日志）没有任何人接住。

    broker 把它们分开：串口、环形缓冲（`--rx` 的历史）、落盘日志、套接字都在
    broker 里，交互式 monitor 只是它的一个**流式客户端**。客户端断开不影响串口，
    下次 attach 会把断开期间的那一段（replay）补上。烧录通过 `release`/`acquire`
    借用串口（见 `_flash_sftool`），期间 broker 照旧活着；收工后 `pulse` 一次让板子
    重新启动，开机日志自然进缓冲。
    """
    import serial as serial_mod

    resolved = resolve_port(port)
    _install_term_cleanup()
    sock_path = ctl_sock or default_monitor_ctl_sock(resolved)
    spool_path = spool or default_monitor_spool(resolved)

    bridge = MonitorBridge()
    ser = serial_mod.serial_for_url(resolved, baud, do_not_open=True)
    ser.timeout = MONITOR_READ_TIMEOUT_S
    ser.dtr = dtr
    ser.rts = rts
    ser.open()
    if reset:
        _pulse_reset(ser, boot_dtr=False)
        ser.dtr = False
        ser.rts = False
    bridge.attach(ser, port=resolved, baud=baud, dtr=dtr, rts=rts)

    server = MonitorCtlServer(sock_path, bridge)
    server.start()
    stop = threading.Event()
    server.on_shutdown = stop.set

    pidfile = default_monitor_pidfile(resolved)
    try:
        fd = os.open(str(pidfile), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(f"{os.getpid()}\n")
    except OSError:
        pidfile = None

    # 落盘日志与被控端口的"信物"同级：**只给本用户**（0600）。日志里会有板子打印的
    # 一切（含路径/序列号/业务数据），默认 umask 会给到 0644，没必要。
    fd = os.open(str(spool_path), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    spool_f = os.fdopen(fd, "ab", buffering=0)
    if not daemon:
        print(
            f"monitor broker: {resolved} @ {baud}  sock={sock_path}  "
            f"log={spool_path}（Ctrl+C 退出；monitor 会自动用它）"
        )
    bridge.note(f"broker 启动 port={resolved} baud={baud} reload={reset}")

    def reader() -> None:
        while not stop.is_set():
            if bridge.released:
                time.sleep(0.05)
                continue
            try:
                data = ser.read(ser.in_waiting or 1)
            except serial_mod.SerialException:
                if bridge.released:
                    time.sleep(0.05)
                    continue
                # 板子 USB 重新枚举：关掉等它回来（沿用 monitor 的老经验）。
                try:
                    if ser.is_open:
                        ser.close()
                except Exception:
                    pass
                bridge.note("串口断开，等待重新接入…")
                while not stop.is_set():
                    time.sleep(0.5)
                    try:
                        ser.open()
                    except Exception:
                        continue
                    if not bridge.released:
                        ser.dtr = False
                        ser.rts = False
                    bridge.note("串口已恢复")
                    break
                continue
            except Exception:
                time.sleep(0.05)
                continue
            if not data:
                continue
            bridge.feed_rx(data)      # 内含推送给流式客户端
            try:
                spool_f.write(data)
            except OSError:
                pass

    threading.Thread(target=reader, name="broker-reader", daemon=True).start()

    try:
        while not stop.is_set():
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        bridge.note("broker 收工，串口关闭")
        if pidfile is not None:
            try:
                pidfile.unlink()
            except OSError:
                pass
        try:
            spool_f.close()
        except OSError:
            pass
        try:
            if ser.is_open:
                ser.close()
        except Exception:
            pass
        server.close()


def cmd_monitor_broker_ctl(sock_path: Path, action: str) -> None:
    """`monitor-broker status|stop`：对一个已经在跑的 broker 说话。"""
    # sock_path 现在是 lease；从 JSON 取 port 再 status
    hub = _serial_hub_mod()
    meta = hub.read_lease_json(sock_path) or {}
    port = str(meta.get("port") or "") or None
    resp = _broker_status(sock_path, port=port)
    if resp is None:
        print(f"没有在跑的 hub（lease={sock_path}）")
        return
    if action == "status":
        state = "yielded（让给 flash）" if resp.get("released") else "holding"
        print(
            f"hub {state}  port={resp.get('port')}  baud={resp.get('baud')}  "
            f"clients={resp.get('clients', 0)}  pid={resp.get('hub_pid')}  "
            f"gen={resp.get('gen')}  lease={resp.get('lease') or sock_path}  "
            f"mux={resp.get('sock')}"
        )
        return
    client = BrokerClient(sock_path, port=port or resp.get("port"))
    try:
        client.request({"op": "shutdown"}, timeout=3.0)
    finally:
        client.close()
    for _ in range(20):
        if _broker_status(sock_path, port=port) is None:
            break
        time.sleep(0.2)
    print("hub 已停止，串口已关闭、信物已释放")


def _run_sifli_miniterm(
    resolved: str,
    baud: int,
    *,
    smart_bttool: bool,
    dtr: bool = MONITOR_DTR,
    rts: bool = MONITOR_RTS,
    reset_on_open: bool = False,
    panic_decoder=None,
    bridge: MonitorBridge | None = None,
    via_broker: bool = False,
    device_port: str | None = None,
) -> bool:
    """SiFli monitor：NSH 设备回显；bttool 自动本地回显，按键原样转发。

    返回 True 表示因串口断开退出（外层应重连），False 表示用户主动退出。
    via_broker 时 `resolved` 为 lease 路径，真正设备名用 `device_port`。
    """
    import serial
    from serial.tools.miniterm import Miniterm

    eol = _MONITOR_EOL_MINITERM.get(MONITOR_EOL.lower())
    if eol is None:
        raise RuntimeError(
            f"无效 MONITOR_EOL={MONITOR_EOL!r}，可选: cr | lf | crlf"
        )

    filters: tuple[str, ...] = ()
    if MONITOR_FILTER:
        filters = (MONITOR_FILTER,)

    if via_broker:
        ser = BrokerSerial(Path(resolved), port=device_port)
    else:
        ser = serial.serial_for_url(resolved, baud, do_not_open=True)

    # 读等待必须有界：读线程只在 `read()` 返回时才回到循环顶，而 Ctrl-Q 的
    # "补上屏"就在循环顶做（见 _pause_service）—— 无限超时会让补屏一直等到
    # 下一个字节才动。串口闲着时每 0.2 s 醒一次，有数据立刻返回，对终端无感。
    ser.timeout = MONITOR_READ_TIMEOUT_S
    ser.dtr = dtr
    ser.rts = rts
    ser.open()

    if reset_on_open and not via_broker:
        print("发送硬件复位脉冲，等待 SFBL ...")
        # 复位后必须让 RESET(RTS)/BOOT(DTR) 都处于释放/运行态(低)，
        # 否则保持高电平会把芯片重新按在复位或下载模式。
        _pulse_reset(ser, boot_dtr=False)
        ser.dtr = False
        ser.rts = False

    if bridge is not None:
        bridge.attach(ser, port=resolved, baud=baud, dtr=dtr, rts=rts)
    else:
        _patch_serial_tx(ser)

    class SifliMiniterm(Miniterm):
        """见 nsh>/bttool> 切换本地 echo；bttool 用 getline 需本地回显。"""

        _RX_TAIL_MAX = 512

        def __init__(
            self,
            *args,
            smart_bttool: bool = True,
            panic_decoder=None,
            ctl_bridge: MonitorBridge | None = None,
            **kwargs,
        ):
            super().__init__(*args, **kwargs)
            self.smart_bttool = smart_bttool
            self.panic_decoder = panic_decoder
            self.ctl_bridge = ctl_bridge
            self._rx_tail = b""
            self._data_errors: set[str] = set()
            self.disconnected = False
            # Ctrl-S/Ctrl-Q 暂停显示（见 pause_rx / _pause_service）
            self.paused = False
            self._resume_req = False
            self._pause_chunks: deque = deque()
            self._pause_bytes = 0
            self._nl_carry = b""

        def _on_rx_bytes(self, data: bytes) -> None:
            if not self.smart_bttool:
                return
            self._rx_tail = (self._rx_tail + data)[-self._RX_TAIL_MAX :]
            if b"nsh>" in self._rx_tail:
                self.echo = False
            if b"bttool>" in self._rx_tail:
                self.echo = True

        # --- Ctrl-S 暂停显示 / Ctrl-Q 恢复 ------------------------------------
        # 暂停 = 只是不往屏幕上写：串口照读、ctl 环照喂，期间"本来要写"的内容
        # 按原样攒在内存里；Ctrl-Q 之后由**读线程**分块补上屏。
        # 为什么放在读线程：全在一个线程里做，顺序天然正确、不会和实时数据交错，
        # 也不会像 tty 的 IXON 那样把读线程堵死（见 _console_disable_ixon）。

        def pause_rx(self) -> None:
            """Ctrl-S：冻结屏幕，期间内容进内存缓冲。"""
            if self.paused:
                return

            self.paused = True
            self._resume_req = False
            self._pause_bytes = 0
            self._pause_chunks.clear()

        def resume_rx(self) -> None:
            """Ctrl-Q：请求补上屏（真正的写在读线程里做，保证顺序）。"""
            self._resume_req = True

        def _pause_append(self, payload) -> None:
            """攒下暂停期间"本来要写"的内容；超过上限丢最旧的。"""
            self._pause_chunks.append(payload)
            self._pause_bytes += len(payload)
            while self._pause_bytes > MONITOR_PAUSE_MAX and len(self._pause_chunks) > 1:
                self._pause_bytes -= len(self._pause_chunks.popleft())

        def _pause_write(self, payload) -> None:
            if isinstance(payload, (bytes, bytearray)):
                self.console.write_bytes(bytes(payload))
            else:
                self.console.write(payload)

        def _data_error(self, exc: BaseException) -> None:
            """RX 处理链（解码/打印）抛异常时：报一次、继续读，绝不静默收摊。

            实机 2026-09-19：崩溃解码器在某段输入上抛异常把读线程带走了，
            表现是"串口还开着、monitor-ctl 也正常，就是屏幕上再也不出字"，
            看着像板子哑了 —— 一整天里两次误判都源于此。异常按类型限频
            （同一类只报一次），后面照常读；丢的那一段在 broker 的环形缓冲里
            还有（`monitor-ctl --rx` 可补看）。
            """
            key = type(exc).__name__
            if key in self._data_errors:
                return
            self._data_errors.add(key)
            try:
                import traceback

                detail = "".join(
                    traceback.format_exception_only(type(exc), exc)
                ).strip()
                self.console.write_bytes(
                    f"\r\n*** monitor: RX 处理异常，已跳过这一段并继续："
                    f"{detail} ***\r\n".encode("utf-8", "replace")
                )
            except Exception:
                pass

        def _port_released(self) -> bool:
            """串口是否正被让给 flash；让出超时（flash 崩了）就自己收回。

            由读线程每轮调用。这里也是"让出不会变成永久"的兜底：flash 若被
            Ctrl-C / 崩掉而没发 `acquire`，MONITOR_HANDOVER_MAX_S 之后
            monitor 自己把串口拿回来并留一行痕迹。
            """
            bridge = self.ctl_bridge
            if bridge is None or not bridge.released:
                return False

            if bridge.handover_expired():
                if bridge.acquire(reason="timeout"):
                    bridge.note(
                        f"让出超过 {MONITOR_HANDOVER_MAX_S:g} s，串口已自动收回"
                    )
                    return False
                return True
            return True

        def _recover_serial(self) -> bool:
            """断线/让出竞态之后：关 fd → 等串口回来 → 重开；返回 False = 该收摊了。

            与 broker 侧 `reader()` 的处理一致（那边一直这么干）。monitor 自己的读
            线程以前遇到 SerialException 直接退出，于是"每烧录一次就哑一次"：
            socket 那侧照常答 `status`，串口却再没人排空（见上面 except 处的注释）。
            """
            self.disconnected = True
            bridge = self.ctl_bridge

            def say(text: str) -> None:
                if bridge is not None:
                    try:
                        bridge.note(text)
                        return
                    except Exception:
                        pass
                try:
                    self.console.write_bytes(
                        f"\r\n*** monitor: {text} ***\r\n".encode("utf-8", "replace")
                    )
                except Exception:
                    pass

            say("串口断开，等它回来（读线程不再退出）")

            while self.alive and self._reader_alive:
                try:
                    if self.serial.is_open:
                        self.serial.close()
                except Exception:
                    pass
                if bridge is not None and bridge.released:
                    time.sleep(0.2)     # 让出给 flash：等它 acquire（收回时它自己 open）
                    continue
                try:
                    if not self.serial.is_open:
                        # 先置线状态再 open：RTS 在本板就是 RESET，别顺手复位。
                        self.serial.dtr = False
                        self.serial.rts = False
                        self.serial.open()
                except Exception:
                    time.sleep(0.5)
                    continue
                if not self.serial.is_open:
                    time.sleep(0.5)
                    continue
                self.disconnected = False
                say("串口已恢复，继续读")
                return True
            return False

        def _pause_service(self) -> None:
            """按预算把缓冲补上屏；补空后才解除暂停。由读线程每轮调用。"""
            if not self._resume_req:
                return

            budget = MONITOR_PAUSE_CHUNK
            while budget > 0 and self._pause_chunks:
                chunk = self._pause_chunks.popleft()
                self._pause_bytes -= len(chunk)
                if len(chunk) <= budget:
                    self._pause_write(chunk)
                    budget -= len(chunk)
                    continue

                # 超预算：只写前半段，后半段放回队首，下一轮接着补。
                self._pause_write(chunk[:budget])
                rest = chunk[budget:]
                self._pause_chunks.appendleft(rest)
                self._pause_bytes += len(rest)
                budget = 0

            if not self._pause_chunks:
                self.paused = False
                self._resume_req = False

        def reader(self):
            import serial as serial_mod

            try:
                while self.alive and self._reader_alive:
                    # Ctrl-Q 之后先按预算补上屏（分块写，别把串口缓冲憋爆）。
                    self._pause_service()
                    if self._port_released():
                        # 串口让给 flash：fd 已经关掉，别读、也别当断线。
                        time.sleep(0.05)
                        continue
                    try:
                        data = self.serial.read(self.serial.in_waiting or 1)
                    except serial_mod.SerialException:
                        if self._port_released():
                            # 串口是**我们自己**让出去的（烧录中），不是拔线：
                            # 不算断线，等 acquire。外层那个 handler 会把
                            # SerialException 一律当拔线处理，所以在这里拦住。
                            time.sleep(0.05)
                            continue
                        # **别让这一下把读线程带走**（2026-09-20 现场）：这条线程是
                        # "谁读串口谁喂 feed_rx"里的那个"谁"，它一死就再没人排空串口，
                        # 而 socket 那侧（status/TX）照常应答 —— 表现就是"板子哑了 /
                        # hub 卡死"，实际是输出堆在内核 tty 缓冲里没人读。让出/收回
                        # （烧录）的竞态最容易命中。这里关 fd、等串口回来、重开、接着读。
                        if not self._recover_serial():
                            break
                        continue
                    if data:
                        try:
                            if self.ctl_bridge is not None:
                                self.ctl_bridge.feed_rx(data)
                            self._on_rx_bytes(data)
                            # 显示前收成单一 `\r\n`：设备 SYSLOG_CRLF + 主机
                            # ONLCR 叠出 `\r\r\n` 会让空回车行距翻倍。
                            data, self._nl_carry = _normalize_serial_newlines(
                                data, self._nl_carry
                            )
                            if not data:
                                continue
                            text = None
                            if self.panic_decoder is not None or not self.raw:
                                # MONITOR_RAW 时也要走文本解码，否则 panic_decoder 永不生效
                                text = self.rx_decoder.decode(data)
                                for transformation in self.rx_transformations:
                                    text = transformation.rx(text)
                            if self.panic_decoder is not None:
                                text = self.panic_decoder.feed(text)
                            if self.paused:
                                # 暂停：攒内存（不写屏、不落盘），Ctrl-Q 后补上屏。
                                # 攒的是"本来要写的那个东西"（解码后的文本或原始字节），
                                # 所以补上屏和实时打印长得一模一样。
                                self._pause_append(data if text is None else text)
                            elif text is not None:
                                self.console.write(text)
                            else:
                                self.console.write_bytes(data)
                        except Exception as exc:
                            # **这里以前会把整条读线程带走，而且毫无痕迹**：实机
                            # 2026-09-19 就是解码器在某段输入上抛异常 → 串口还开着、
                            # monitor-ctl 也能用，但屏幕上再也不出数据，看着像"板子
                            # 哑了"。现在：报一次、丢掉这一段、继续读 —— 数据在
                            # broker 的环形缓冲里也有（可直接 `monitor-ctl --rx` 补看）。
                            self._data_error(exc)
                            continue
            except serial_mod.SerialException:
                # 串口被拔出/断开：立即关闭 fd，避免占用 ttyUSB0 导致重枚举到 ttyUSB1
                self.disconnected = True
                self.alive = False
                try:
                    if self.serial.is_open:
                        self.serial.close()
                except Exception:
                    pass
                try:
                    self.console.cancel()
                except Exception:
                    pass
            except Exception:
                # 不要让 rx 线程因解码等异常而静默崩溃，回退到原始字节输出
                try:
                    self.console.write_bytes(data)
                except Exception:
                    pass

    term = SifliMiniterm(
        ser,
        echo=MONITOR_ECHO,
        eol=eol,
        filters=filters,
        smart_bttool=smart_bttool,
        panic_decoder=panic_decoder,
        ctl_bridge=bridge,
    )
    term.raw = MONITOR_RAW
    # 与 miniterm CLI 一致，初始化编解码器（否则 writer 无 tx_encoder）
    term.set_rx_encoding("UTF-8")
    term.set_tx_encoding("UTF-8")
    # Ctrl-S/Ctrl-Q 由 monitor 自己接管（见 _install_pause_keys / pause_rx）：
    # 必须赶在 start() 之前，否则 writer 的首个 getkey 会把第一个按键吃掉。
    _install_pause_keys(term)
    term.start()
    try:
        term.join()
    finally:
        term.stop()
        try:
            term.console.cancel()
        except Exception:
            pass
        try:
            if hasattr(ser, "cancel_read"):
                ser.cancel_read()
        except Exception:
            pass
        term.join(transmit_only=True)
        if term.receiver_thread.is_alive():
            term.receiver_thread.join(timeout=1.0)
        if panic_decoder is not None:
            try:
                tail = panic_decoder.flush()
                if tail:
                    term.console.write(tail)
            except Exception:
                pass
        try:
            if ser.is_open:
                ser.close()
        except Exception:
            pass
        if bridge is not None:
            bridge.detach()
        # 给 udev/驱动一点时间释放节点，避免重连时仍占着旧 ttyUSB0
        if term.disconnected:
            time.sleep(0.15)
    return term.disconnected


def cmd_decode(
    root: Path,
    elf: Path | None,
    addrs: list[str],
    *,
    decode_text: bool = False,
) -> None:
    """独立调用 vela_elf_resolve.py（不打开串口）。"""
    mod = _import_elf_resolve()
    elf_path = (elf or nuttx_elf_path(root)).resolve()
    resolver = mod.ElfSymbolResolver(elf_path)
    if decode_text:
        decoder = mod.PanicSerialDecoder(resolver)
        data = sys.stdin.read()
        sys.stdout.write(decoder.feed(data))
        sys.stdout.write(decoder.flush())
        return
    if not addrs:
        raise RuntimeError("decode 需要地址参数，或 --decode-text 从 stdin 读日志")
    for i, spec in enumerate(addrs):
        addr = int(spec, 0)
        print(resolver.format_frame(i, addr, use_color=sys.stdout.isatty()))


def _probe_serial_port(port: str, baud: int) -> bool:
    """尝试短暂打开串口以确认设备已就绪（非仅存在 /dev 节点）。"""
    import serial

    try:
        ser = serial.serial_for_url(port, baud, do_not_open=True)
        ser.dtr = False
        ser.rts = False
        ser.timeout = 0
        ser.open()
        ser.close()
        return True
    except (serial.SerialException, OSError):
        return False


def _wait_for_serial(preferred: str, baud: int) -> str:
    """等待串口重新出现并可打开。优先原端口；原端口不可用时切换到任一可用口。"""
    announced = False
    while True:
        ports = list_serial_ports()
        ready = [p for p in ports if _probe_serial_port(p, baud)]

        if preferred in ready:
            return preferred

        if ready:
            pick = ready[0]
            if pick != preferred:
                print(f"串口切换到 {pick}")
            return pick

        if not announced:
            print(f"等待串口 {preferred} 重新接入... (Ctrl+C 退出)")
            announced = True
        time.sleep(0.5)


def cmd_monitor(
    port: str | None,
    baud: int,
    *,
    root: Path | None = None,
    smart_bttool: bool | None = None,
    dtr: bool | None = None,
    rts: bool | None = None,
    reset: bool = True,
    decode_elf: bool | None = None,
    elf: Path | None = None,
    ctl_sock: Path | None = None,
    no_ctl: bool = False,
    no_broker: bool = False,
    broker: bool = False,
) -> None:
    resolved = resolve_port(port)
    _install_term_cleanup()
    if root is not None:
        warn_tab_completion(root)
    try:
        import serial  # noqa: F401
    except ImportError as exc:
        raise RuntimeError(
            "monitor 需要 pyserial: pip install pyserial"
        ) from exc

    if smart_bttool is None:
        smart_bttool = MONITOR_SMART_BTTOOL
    if dtr is None:
        dtr = MONITOR_DTR
    if rts is None:
        rts = MONITOR_RTS

    panic_decoder = None
    if root is not None and (decode_elf if decode_elf is not None else MONITOR_DECODE_ELF):
        panic_decoder = make_panic_decoder(root, elf)

    print(
        f"串口监视: {resolved} @ {baud} bps "
        f"(dtr={dtr}, rts={rts}, echo={MONITOR_ECHO}, "
        f"eol={MONITOR_EOL}, raw={MONITOR_RAW}, filter={MONITOR_FILTER}, "
        f"smart_bttool={smart_bttool}，Ctrl+] 退出)"
    )
    if panic_decoder is not None:
        print(
            "崩溃解码: 已启用（无 ALLSYMS 时解析 backtrace| / PC/LR；"
            "Assertion 行红色高亮）"
        )
    print(
        "提示: NSH 用设备回显；bttool 自动开本地回显，回车原样转发。"
        "若不需要可加 --no-smart-bttool，进 bttool 后 Ctrl+T 再按 e 开 echo。"
    )
    if dtr or rts:
        print(
            "注意: CH340(ttyUSB) 的 DTR/RTS 常接 RESET/BOOT，拉高会让芯片停在 "
            "ROM 下载模式(只打印 SFBL)。如启动无输出请加 --dtr 0 --rts 0。"
        )
    if reset:
        print(
            "打开后将发一次硬件复位脉冲以抓取从 SFBL 起的启动日志；"
            "若复位脉冲极性不对(无输出/一直复位)请加 --no-reset 并手动按复位键。"
        )

    bridge: MonitorBridge | None = None
    ctl_server: MonitorCtlServer | None = None
    sock_path = ctl_sock or default_monitor_ctl_sock(resolved)

    # 模式选择（2026-09-21）：**默认挂到 serial_hub**。
    # 直连模式下本进程一退出（或被 agent `pkill`）套接字一起没，另一边的
    # `monitor-ctl` 会被连带关掉 —— 这正是"hub 一直在重启 / 烧完对面死了"的根因。
    # `--no-broker` 才退回老的直连。已有活的端点时无论如何都只挂上去，不开第二把 fd。
    already_running = _broker_status(sock_path, port=resolved) is not None
    broker_mode = not no_broker
    if broker and no_broker:
        print("注意: 同时给了 --broker 与 --no-broker，按 --no-broker 直连")

    if broker_mode:
        # 常驻 hub：串口 + lease 信物 + abstract 多路复用都在它那里。
        try:
            _ensure_broker(
                resolved, baud, dtr=dtr, rts=rts, reset=reset,
                sock_path=sock_path, spool=default_monitor_spool(resolved),
            )
        except Exception as exc:
            print(f"broker 不可用（{exc}）\n回退到直连串口模式。")
            broker_mode = False
        else:
            info = _broker_status(sock_path, port=resolved) or {}
            tag = "已挂到" if already_running else "已拉起"
            print(
                f"客户端模式: {tag}常驻 hub（{info.get('port') or resolved}）"
                f"，退出本窗口不影响串口与其它 monitor 客户端"
            )
            print(f"信物(lease): {sock_path}")
            print(f"  mux: {info.get('sock') or '?'}")
            print(f"  第二终端: python3 {SCRIPT_BASENAME} monitor --follow -p {resolved}")
            print(f"  查状态:   python3 {SCRIPT_BASENAME} monitor --status -p {resolved}")
            print(f"  收工 hub: python3 {SCRIPT_BASENAME} monitor stop -p {resolved}")

    if not broker_mode and not no_ctl:
        bridge = MonitorBridge()
        ctl_server = MonitorCtlServer(sock_path, bridge)
        ctl_server.start()
        print(f"控制通道: {sock_path}")
        print(
            f"  python3 {SCRIPT_BASENAME} monitor --status"
            f"  /  monitor --follow  /  monitor <NSH>"
        )
        print("  （本窗口继续操作串口；另一进程用同一套 monitor 客户端入口）")

    if not smart_bttool:
        if ctl_server is not None:
            print(
                "注意: --no-smart-bttool 走子进程 miniterm，控制通道不可用；"
                "请用默认 monitor。"
            )
            ctl_server.close()
            ctl_server = None
            bridge = None
        try:
            _run_miniterm_subprocess(
                resolved, baud, echo=MONITOR_ECHO, eol=MONITOR_EOL,
                dtr=dtr, rts=rts,
            )
        except KeyboardInterrupt:
            pass
        return

    import serial as serial_mod

    # SIGUSR1 = 往日志里打一次**全线程栈转储**。串口还开着、`monitor-ctl` 也
    # 能用，但屏幕上不再有数据这种问题，以前只能靠猜是"板子哑了"还是
    # "读线程卡了/死了"。有了这条钩子：`kill -USR1 <monitor pid>`，
    # 栈会出现在 monitor 的日志里（faulthandler 写 stderr）。
    try:
        faulthandler.register(signal.SIGUSR1, all_threads=True)
        print(f"栈转储: kill -USR1 {os.getpid()}（线程卡住时用）")
    except Exception:
        pass

    first = True
    try:
        while True:
            try:
                if broker_mode:
                    disconnected = _run_sifli_miniterm(
                        str(sock_path), baud, smart_bttool=True, dtr=dtr, rts=rts,
                        reset_on_open=False,
                        panic_decoder=panic_decoder,
                        bridge=None,
                        via_broker=True,
                        device_port=resolved,
                    )
                else:
                    disconnected = _run_sifli_miniterm(
                        resolved, baud, smart_bttool=True, dtr=dtr, rts=rts,
                        reset_on_open=reset and first,
                        panic_decoder=panic_decoder,
                        bridge=bridge,
                    )
                first = False
            except serial_mod.SerialException as exc:
                print(f"\n串口打开失败: {exc}")
                disconnected = True

            if not disconnected:
                break

            if broker_mode:
                # 客户端断开（broker 被 stop / 自己的套接字出了问题）：
                # 串口本身不受影响，重试接回去就是。
                print("\n与 broker 的连接断开，2 秒后重试…（串口不受影响）")
                time.sleep(2.0)
                continue

            print("\n串口已断开，等待重新连接...")
            resolved = _wait_for_serial(resolved, baud)
            print(f"重新连接 {resolved} ...")
    except KeyboardInterrupt:
        pass
    finally:
        if ctl_server is not None:
            ctl_server.close()


def _monitor_ctl_request(
    sock_path: Path,
    req: dict,
    *,
    timeout: float,
    port: str | None = None,
) -> dict:
    target = _mux_connect_target(port, sock_path)
    if target is None:
        raise RuntimeError(
            f"控制通道不存在: lease={sock_path}\n"
            f"请先在另一终端运行: python3 {SCRIPT_BASENAME} monitor -p <port>"
        )
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(timeout)
    try:
        conn.connect(target)
        conn.sendall((json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"))
        buf = b""
        while b"\n" not in buf:
            chunk = conn.recv(65536)
            if not chunk:
                break
            buf += chunk
    except OSError as exc:
        raise RuntimeError(f"连接控制通道失败 ({target!r}): {exc}") from exc
    finally:
        try:
            conn.close()
        except OSError:
            pass
    if not buf.strip():
        raise RuntimeError("控制通道无应答")
    line = buf.split(b"\n", 1)[0]
    try:
        resp = json.loads(line.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"控制通道应答不是 JSON: {line!r}") from exc
    if not isinstance(resp, dict):
        raise RuntimeError(f"控制通道应答格式错误: {resp!r}")
    return resp


def _cmd_monitor_ctl_follow(
    path: Path,
    *,
    replay: int,
    forward_stdin: bool,
    port: str | None = None,
    line_stdin: bool = False,
) -> None:
    """`monitor-ctl --follow`：把当前 monitor 的串口**当成第二个终端**。

    监听：以流式客户端 attach，收到什么就往 stdout 写什么（含 attach 时回放的
    那一段）。默认 **raw 按键转发**（Tab / 方向键 / 退格直达 NSH readline）；
    `--line-stdin` 才退回"整行+回车再发"（给脚本用）。

    **本终端** Ctrl-S 暂停显示 / Ctrl-Q 补缓冲后恢复（与交互式 monitor 同语义：
    串口与 hub 照收，只是不往这个窗口写；不下发给板子）。

    这条路**不经过 miniterm**，所以和操作者那个 monitor 窗口互不影响：一个串口，
    两个终端 —— 用户照常 `monitor`，AI/脚本用 `--follow` 看同一份数据、也能往同
    一个串口写。**Ctrl+] 只断开这条连接**（与交互式 monitor 相同），串口与另一个
    终端不受影响；Ctrl-C 会原样发给板子（NSH 中断）。

    **断线自愈**（2026-09-21）：烧录 release/acquire、USB-CDC 复位、hub 短暂
    重启都不该逼用户重开本窗口。连接掉了就等套接字回来再 attach；只有 Ctrl+]
    才真正退出。
    """
    import select

    nl_carry = b""
    paused = False
    resume_req = False
    pause_chunks: deque = deque()
    pause_bytes = 0

    def _write_out(data: bytes) -> None:
        if not data:
            return
        try:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        except (BrokenPipeError, ValueError):
            pass

    def _pause_append(data: bytes) -> None:
        nonlocal pause_bytes
        pause_chunks.append(data)
        pause_bytes += len(data)
        while pause_bytes > MONITOR_PAUSE_MAX and len(pause_chunks) > 1:
            pause_bytes -= len(pause_chunks.popleft())

    def _pause_service() -> None:
        """Ctrl-Q 后分块补上屏；补空才解除暂停（新到的仍进缓冲，顺序不错乱）。"""
        nonlocal paused, resume_req, pause_bytes
        if not resume_req:
            return
        budget = MONITOR_PAUSE_CHUNK
        while budget > 0 and pause_chunks:
            chunk = pause_chunks.popleft()
            pause_bytes -= len(chunk)
            if len(chunk) <= budget:
                _write_out(chunk)
                budget -= len(chunk)
                continue
            _write_out(chunk[:budget])
            rest = chunk[budget:]
            pause_chunks.appendleft(rest)
            pause_bytes += len(rest)
            budget = 0
        if not pause_chunks:
            paused = False
            resume_req = False

    def emit(data: bytes) -> None:
        nonlocal nl_carry
        try:
            fixed, nl_carry = _normalize_serial_newlines(data, nl_carry)
        except Exception:
            return
        if not fixed:
            return
        if paused:
            _pause_append(fixed)
            return
        _write_out(fixed)

    def _wait_broker(*, timeout_s: float = 30.0) -> bool:
        """等 hub 活过来；必要时再拉一次。返回是否就绪。"""
        resolved = resolve_port(port) if port else None
        if _broker_status(path, port=resolved) is None:
            try:
                _ensure_broker(
                    resolved or resolve_port(port), MONITOR_BAUD,
                    dtr=MONITOR_DTR, rts=MONITOR_RTS, reset=False,
                    sock_path=path, spool=default_monitor_spool(port),
                )
            except Exception as exc:
                print(f"# 拉起 hub 失败: {exc}", file=sys.stderr)
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if _broker_status(path, port=resolved) is not None:
                return True
            time.sleep(0.25)
        return False

    stdin_fd = None
    if forward_stdin:
        try:
            stdin_fd = sys.stdin.fileno()
        except (AttributeError, ValueError, OSError):
            stdin_fd = None
    use_raw = (
        forward_stdin
        and not line_stdin
        and stdin_fd is not None
        and os.isatty(stdin_fd)
    )

    raw_ctx = _TtyRawStdin(stdin_fd) if use_raw else None
    if raw_ctx is not None:
        raw_ctx.__enter__()
    try:
        out_fd = sys.stdout.fileno()
        if os.isatty(out_fd):
            _console_disable_ixon(out_fd)
    except (AttributeError, ValueError, OSError):
        pass

    session = 0
    try:
        while True:
            session += 1
            if session == 1:
                if _broker_status(path, port=port) is None:
                    print(f"# 未发现串口信物，先拉 hub：{path}", file=sys.stderr)
                if not _wait_broker(timeout_s=10.0):
                    raise RuntimeError(
                        f"连不上控制通道 (lease={path})\n"
                        f"请先运行: python3 {SCRIPT_BASENAME} monitor -p {port}"
                    )
            else:
                print(
                    "\n# 与 hub 断开（烧录/复位常见），正在重连… "
                    "（Ctrl+] 退出本窗口）",
                    file=sys.stderr,
                )
                if not _wait_broker(timeout_s=120.0):
                    print("# 等 hub 超时，继续重试…", file=sys.stderr)
                    continue

            client = None
            try:
                client = BrokerClient(path, timeout=5.0, port=port)
                # 重连只回放一小段，避免把烧录前的旧环整页倒出来。
                attach_replay = replay if session == 1 else min(replay, 4096)
                resp = client.attach(attach_replay, emit)
                if not resp.get("ok"):
                    raise RuntimeError(resp.get("error") or "attach 失败")
                mode = (
                    "raw 按键；Ctrl-S 暂停本窗 / Ctrl-Q 补屏；Ctrl+] 退出"
                    if use_raw
                    else (
                        "按行转发（--line-stdin）"
                        if forward_stdin
                        else "只听不写"
                    )
                )
                tag = "已接到" if session == 1 else "已重新接到"
                print(
                    f"# {tag} {resp.get('port') or '?'}（{mode}）",
                    file=sys.stderr,
                )

                pending = b""
                while True:
                    _pause_service()
                    watch = [client.sock_fd] + (
                        [stdin_fd] if stdin_fd is not None else []
                    )
                    try:
                        ready, _, _ = select.select(watch, [], [], 0.05)
                    except (OSError, ValueError):
                        ready = []
                    if stdin_fd is not None and stdin_fd in ready:
                        chunk = os.read(stdin_fd, 4096)
                        if not chunk:
                            stdin_fd = None
                        elif use_raw:
                            out = bytearray()
                            for b in chunk:
                                if b == 0x1D:  # Ctrl+]：与 miniterm 相同，只断本窗
                                    raise KeyboardInterrupt
                                if b == 0x13:  # Ctrl-S：暂停本窗
                                    if out:
                                        client.send_raw(
                                            bytes(out).replace(b"\r", b"\n")
                                        )
                                        out.clear()
                                    if not paused:
                                        paused = True
                                        resume_req = False
                                        pause_chunks.clear()
                                        pause_bytes = 0
                                    continue
                                if b == 0x11:  # Ctrl-Q：请求补屏
                                    if out:
                                        client.send_raw(
                                            bytes(out).replace(b"\r", b"\n")
                                        )
                                        out.clear()
                                    if paused:
                                        resume_req = True
                                    continue
                                out.append(b)
                            if out:
                                client.send_raw(
                                    bytes(out).replace(b"\r", b"\n")
                                )
                        else:
                            pending += chunk
                            while b"\n" in pending:
                                line, pending = pending.split(b"\n", 1)
                                client.send_raw(line.rstrip(b"\r") + b"\n")
                    if client.sock_fd in ready or client.buffered_lines() > 0:
                        for _ in range(10000):
                            if not client.pump(timeout=0.3):
                                raise ConnectionError("与 monitor 的连接已断开")
                            if client.buffered_lines() == 0:
                                break
            except KeyboardInterrupt:
                raise
            except (ConnectionError, OSError, RuntimeError) as exc:
                # 断线 → 外层重连；别的错误也当可恢复（hub 正在起来）。
                print(f"# {exc}", file=sys.stderr)
                time.sleep(0.4)
            finally:
                if client is not None:
                    try:
                        client.close()
                    except Exception:
                        pass
    except KeyboardInterrupt:
        print("\n# 已断开（串口与另一个终端不受影响）", file=sys.stderr)
    finally:
        if raw_ctx is not None:
            raw_ctx.__exit__(None, None, None)


def cmd_monitor_ctl(
    argv: list[str],
    *,
    sock_path: Path | None = None,
) -> None:
    """经 Unix 套接字和正在运行的 monitor 说话。

    三种用法：**不带命令 = 当第二终端**（持续监听 + raw 按键转发进同一个串口）；
    `--status` / `--rx` = 一次性只读；带命令 = 发一条、收它的输出。
    """
    wait_ms = MONITOR_CTL_WAIT_MS
    idle_ms = MONITOR_CTL_IDLE_MS
    no_nl = False
    hex_mode = False
    follow = False
    follow_replay = MONITOR_CLIENT_REPLAY
    forward_stdin = True
    line_stdin = False
    rest: list[str] = []
    i = 0

    # 先扫一遍 -p/--port 与 --sock：路径要**在解析循环之前**定下来（--status / --rx /
    # release 这些分支在循环里就用它），而 `-p` 决定套接字文件名（见
    # default_monitor_ctl_sock：工具路径 + 串口名）。循环里的处理只做提示。
    want_port: str | None = None
    for k, a in enumerate(argv):
        if a in ("-p", "--port") and k + 1 < len(argv):
            want_port = argv[k + 1]
        elif a == "--sock" and k + 1 < len(argv):
            sock_path = Path(argv[k + 1]).expanduser()
    path = sock_path or default_monitor_ctl_sock(want_port)

    def _ensure_hub_here() -> None:
        if not want_port:
            return
        resolved = resolve_port(want_port)
        if _broker_status(path, port=resolved) is not None:
            return
        _ensure_broker(
            resolved,
            MONITOR_BAUD,
            dtr=MONITOR_DTR,
            rts=MONITOR_RTS,
            reset=False,
            sock_path=path,
            spool=default_monitor_spool(resolved),
        )

    while i < len(argv):
        arg = argv[i]
        if arg in ("-h", "--help"):
            print(
                f"用法: python3 {SCRIPT_BASENAME} monitor [选项] [<NSH命令...>]\n"
                "  （推荐统一入口；旧名 monitor-ctl / ctl 仍可用）\n"
                "  （不带命令）      第二终端：持续监听 + raw 按键转发进串口\n"
                "                    （交互主窗口请用: monitor，不要加 --follow）\n"
                "  --status          只查一行状态（hub 是否在线、串口是否已让出）\n"
                "  --rx              只读最近串口输出，不发送\n"
                "  --follow          当第二个终端：持续监听并入屏，raw 按键转发进串口\n"
                "                    （Tab/方向键直达 NSH；Ctrl-S 暂停 / Ctrl-Q 补屏；\n"
                "                     Ctrl+] 退出本窗；--no-stdin 只听；\n"
                "                     --line-stdin 整行再发；--follow-replay N 回放）\n"
                "  release           请 hub 让出串口（给 sftool；flash 会自动做）\n"
                "  acquire           请 hub 收回串口\n"
                "  stop              请 hub 收工（释放 lease；一般不必）\n"
                "  --hex AABB        发送原始十六进制字节（默认不加换行）\n"
                "  --wait SEC        等待设备输出的最长时间（秒，默认 "
                f"{MONITOR_CTL_WAIT_MS / 1000:g}）\n"
                "  --idle SEC        输出静默多久视为结束（秒，默认 "
                f"{MONITOR_CTL_IDLE_MS / 1000:g}）\n"
                "  --no-nl           发送文本时不自动补 \\n\n"
                "  --sock PATH       信物(lease)路径（默认 "
                f"{default_monitor_ctl_sock()}）\n"
                "\n"
                "人机交互主窗口: `monitor`；第二窗口/脚本: `monitor --follow` 或 "
                "`monitor --status` / `monitor <NSH>`。\n"
                "`flash` 会自动做 release/acquire，不需要手工调用。"
            )
            return
        if arg == "stop" and not rest:
            cmd_monitor_broker_ctl(path, "stop")
            return
        if arg in ("release", "acquire") and not rest:
            _ensure_hub_here()
            resp = _monitor_ctl_request(
                path, {"op": arg}, timeout=5.0, port=want_port
            )
            if not resp.get("ok"):
                raise RuntimeError(resp.get("error") or f"{arg} failed")
            print(
                f"monitor 已{'让出' if arg == 'release' else '收回'}串口"
                f"（released={1 if resp.get('released') else 0}）"
            )
            return
        if arg in ("--status", "status", "ping") and not rest:
            _ensure_hub_here()
            resp = _monitor_ctl_request(
                path, {"op": "ping"}, timeout=3.0, port=want_port
            )
            if not resp.get("ok"):
                raise RuntimeError(resp.get("error") or "status failed")
            state = "open" if resp.get("open") else "idle"
            if resp.get("released"):
                state = "yielded（让给 flash 中）"
            print(
                f"hub {state}  port={resp.get('port') or '-'}  "
                f"baud={resp.get('baud') or '-'}  "
                f"lease={resp.get('lease') or path}  "
                f"mux={resp.get('sock') or '-'}  "
                f"gen={resp.get('gen', '-')}  "
                f"clients={resp.get('clients', '-')}"
            )
            return
        if arg == "--follow":
            follow = True
            i += 1
            continue
        if arg == "--no-stdin":
            forward_stdin = False
            i += 1
            continue
        if arg == "--line-stdin":
            line_stdin = True
            i += 1
            continue
        if arg == "--follow-replay" and i + 1 < len(argv):
            follow_replay = int(argv[i + 1])
            i += 2
            continue
        if arg == "--rx" and not rest:
            _ensure_hub_here()
            resp = _monitor_ctl_request(
                path, {"op": "rx"}, timeout=3.0, port=want_port
            )
            if not resp.get("ok"):
                raise RuntimeError(resp.get("error") or "rx failed")
            sys.stdout.write(str(resp.get("rx") or ""))
            if resp.get("rx") and not str(resp["rx"]).endswith("\n"):
                sys.stdout.write("\n")
            return
        if arg in ("-p", "--port") and i + 1 < len(argv):
            # 只用于**校验**：monitor 在别的串口上时给个明确提示（一个套接字服务
            # 一个串口；`monitor-ctl -p /dev/ttyACM0` 是操作者的习惯写法）。
            want_port = argv[i + 1]
            try:
                st = _monitor_ctl_request(path, {"op": "ping", "port": want_port},
                                          timeout=3.0)
                got = str(st.get("port") or "")
                if got and got != want_port:
                    print(
                        f"注意: 控制通道在 {got} 上，不是 {want_port}",
                        file=sys.stderr,
                    )
            except Exception:
                pass
            i += 2
            continue
        if arg == "--sock" and i + 1 < len(argv):
            path = Path(argv[i + 1]).expanduser()
            i += 2
            continue
        if arg in ("--wait", "-w") and i + 1 < len(argv):
            wait_ms = int(float(argv[i + 1]) * 1000)
            i += 2
            continue
        if arg == "--idle" and i + 1 < len(argv):
            idle_ms = int(float(argv[i + 1]) * 1000)
            i += 2
            continue
        if arg == "--no-nl":
            no_nl = True
            i += 1
            continue
        if arg == "--hex":
            hex_mode = True
            i += 1
            continue
        rest.append(arg)
        i += 1

    # 不带命令、也没指定任何 op = **当第二终端**（持续监听 + stdin 转发）。
    # 想只要一行状态就显式 `--status`；这条默认是用户要的语义：
    # 「monitor-ctl -p /dev/ttyACM0」应该挂上去看/写同一个串口。
    if follow or not rest:
        _cmd_monitor_ctl_follow(
            path,
            replay=follow_replay,
            forward_stdin=forward_stdin,
            port=want_port,
            line_stdin=line_stdin,
        )
        return

    if hex_mode:
        req = {
            "op": "send",
            "hex": "".join(rest),
            "wait_ms": wait_ms,
            "idle_ms": idle_ms,
        }
    else:
        req = {
            "op": "send",
            "text": " ".join(rest),
            "nl": not no_nl,
            "wait_ms": wait_ms,
            "idle_ms": idle_ms,
        }
    timeout = max(wait_ms / 1000.0 + 2.0, 5.0)
    resp = _monitor_ctl_request(path, req, timeout=timeout)
    if not resp.get("ok"):
        raise RuntimeError(resp.get("error") or "send failed")
    sys.stdout.write(str(resp.get("rx") or ""))
    if resp.get("rx") and not str(resp["rx"]).endswith("\n"):
        sys.stdout.write("\n")


def cmd_clean(root: Path) -> None:
    """仅清理编译产物，保留 .config 与 CMake 缓存。"""
    out = cmake_out_dir(root)
    if not out.is_dir():
        print(f"构建目录不存在，跳过 clean: {out}")
        return
    if not (out / "build.ninja").is_file():
        print(f"未找到 Ninja 构建文件，跳过 clean: {out}")
        return
    run(["cmake", "--build", str(out), "-t", "clean"], cwd=root)


def cmd_distclean(root: Path) -> None:
    """通过 build.sh 删除 CMake 输出目录（用 -b 钉死目录，别删到别的树的）。"""
    run(
        ["./build.sh", board_config_arg(root), "--cmake",
         "-b", str(cmake_out_dir(root)), "distclean"],
        cwd=root,
    )


def cmd_fullclean(root: Path) -> None:
    """删除整个 CMake 输出目录，并清理 nuttx 树内残留 .config。"""
    out = cmake_out_dir(root)
    if out.is_dir():
        print(f"删除构建目录: {out}")
        shutil.rmtree(out)

    for name in (".config", ".config.old", ".version"):
        path = root / "nuttx" / name
        if path.exists() or path.is_symlink():
            print(f"删除: {path}")
            path.unlink()

    print("fullclean 完成。")


def _commands_str() -> str:
    return " ".join(COMMANDS)


def completion_bash() -> str:
    """生成 bash 补全：纯 bash 词表，**Tab 时不启动 Python**（快），但覆盖要够。

    以前只有第一层：选了子命令后 `case "$prev"` 只认几个词，于是
    `monitor-ctl ` 后面按 Tab 什么都没有。现在按子命令给词表，并且把常用的
    板级命令（`monitor-ctl ctl radio on` 这条链）也补上。
    """
    cmds = _commands_str()
    script = str(SCRIPT_PATH)
    basename = SCRIPT_BASENAME
    body = '''# bash completion for __BASENAME__
#   生效: source <(python3 __SCRIPT__ complete bash)
#   或:   python3 __SCRIPT__ complete install

_vela_tools_ports() {
    # 串口候选优先，其次才是普通文件名
    COMPREPLY=()
    local p
    for p in /dev/ttyACM* /dev/ttyUSB* /dev/serial/by-id/*; do
        [[ -e "$p" ]] && COMPREPLY+=( "$p" )
    done
    if (( ${#COMPREPLY[@]} == 0 )); then
        COMPREPLY=( $(compgen -f -- "$1") )
    fi
    return 0
}

_vela_tools_completion() {
    # 默认所有子命令都可链式（`build flash monitor`、`build monitor fl<Tab>` ⇒ flash）：
    # 候选为空时 bash 会退化成补文件名 —— 这正是"`build monitor fl` 补出 frameworks/ 而不是
    # flash"的原因。只有"自己有参数位置"的几个子命令例外，见下面的 chainable=0。
    local cur prev i word cmd chainable=1
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # 脚本本身在哪儿（./x.py、python3 x.py、包装脚本都认）
    local sidx=-1
    for ((i = 0; i < COMP_CWORD; i++)); do
        case "${COMP_WORDS[i]}" in
            __BASENAME__|./__BASENAME__|*/__BASENAME__|*vela_my_vendor_tools.py|*build_board.py)
                sidx=$i; break ;;
        esac
    done
    (( sidx < 0 )) && return 0

    # 当前子命令 = 最后一个已知子命令（支持 build flash monitor 这种链）
    cmd=""
    for ((i = sidx + 1; i < COMP_CWORD; i++)); do
        word="${COMP_WORDS[i]}"
        case " ${ALL_CMDS} " in
            *" $word "*) cmd="$word" ;;
        esac
    done

    # ---- 选项的参数 ----
    case "$prev" in
        -p|--port)                     _vela_tools_ports "$cur"; return 0 ;;
        --sock|--elf|-i|--image|--sd)  COMPREPLY=( $(compgen -f -- "$cur") ); return 0 ;;
        -M|--flash-medium)             COMPREPLY=( $(compgen -W "nand sd emmc nor" -- "$cur") ); return 0 ;;
        -b|-B|--baud|-fb|--flash-baud) COMPREPLY=( $(compgen -W "1000000 921600 460800 230400 115200" -- "$cur") ); return 0 ;;
        -j|--jobs)                     COMPREPLY=( $(compgen -W "1 2 4 8 16" -- "$cur") ); return 0 ;;
        complete)                      COMPREPLY=( $(compgen -W "bash zsh install" -- "$cur") ); return 0 ;;
        install)                       COMPREPLY=( $(compgen -W "bash zsh" -- "$cur") ); return 0 ;;
        # 板级命令（经 monitor-ctl 转发进串口的那一层）
        ctl)                COMPREPLY=( $(compgen -W "radio sensor bl sd wt watch mtp notif log dvfs gnss" -- "$cur") ); return 0 ;;
        radio|sensor|mtp|notif) COMPREPLY=( $(compgen -W "on off" -- "$cur") ); return 0 ;;
        dvfs)               COMPREPLY=( $(compgen -W "auto off low high 48 72 96 120 144 192 240" -- "$cur") ); return 0 ;;
        gnss)               COMPREPLY=( $(compgen -W "ver b1c bdsonly still dump_in dump_out" -- "$cur") ); return 0 ;;
        bl|log)             COMPREPLY=( $(compgen -W "err warn notice info all debug silent none" -- "$cur") ); return 0 ;;
    esac

    # ---- 子命令自己的词表 ----
    local words=""
    case "$cmd" in
        monitor|monitor-ctl)
            words="--status --rx --follow --no-stdin --line-stdin --follow-replay --wait --idle --hex --no-nl --sock -p --no-decode --no-reset --no-smart-bttool --no-ctl --no-broker --broker --ctl-sock --elf -b --dtr --rts release acquire stop ctl sys test"
            chainable=0 ;;
        monitor-broker)
            words="status stop"; chainable=0 ;;
        # 可以链式跟下一个子命令（`build flash monitor`）——默认就是，见上面的 chainable=1
        flash|flash-all|flash-fs|flash-factory)
            words="--force --repack --yes -M --flash-medium -fb --flash-baud -p --sd -i --image" ;;
        build|build-all|build-boot|build-factory|build-fs)
            words="-j --jobs" ;;
        burn-sd|pack-sd-img)
            words="--sd -i --image --yes --repack" ;;
        complete)
            words="bash zsh install"; chainable=0 ;;
        decode)
            words="--decode-text --elf"; chainable=0 ;;
    esac

    if [[ -z "$cmd" ]]; then
        # 还没选子命令：列子命令；正在敲选项就给全局选项
        if [[ "$cur" == -* ]]; then
            words="${GLOBAL_OPTS}"
        else
            words="${ALL_CMDS}"
        fi
    elif [[ "$cur" == -* ]]; then
        # 已选子命令且在敲选项：该子命令的选项 + 全局项
        words="$words -p --port -h --help"
    fi

    # 可链式的子命令：非选项位置还能再跟一个子命令（`build flash monitor`）
    if [[ -n "$cmd" && "$cur" != -* && "$chainable" == 1 ]]; then
        words="$words ${ALL_CMDS}"
    fi

    COMPREPLY=( $(compgen -W "$words" -- "$cur") )
    return 0
}

# 注意：**不要**加 `-o default`/`-o bashdefault` —— 那样"我们没给出候选"时会自动退化成
# 补文件名，于是在子命令位置按 Tab 会冒出 frameworks/ connectivity/ 这些目录（用户 2026-09-19
# 报的现象）。需要文件的地方（-p/-i/--image/--elf/--sd/--sock）由函数里显式 compgen -f 提供。
complete -F _vela_tools_completion __SCRIPT__
complete -F _vela_tools_completion ./__BASENAME__
complete -F _vela_tools_completion __BASENAME__
'''
    globals_opts = (
        "-p --port -j --jobs -M --flash-medium -fb --flash-baud -b -B --baud "
        "--sd -i --image --yes --repack -h --help"
    )
    return (
        body.replace("__BASENAME__", basename)
        .replace("__SCRIPT__", script)
        .replace("${ALL_CMDS}", cmds)
        .replace("${GLOBAL_OPTS}", globals_opts)
    )


def completion_zsh() -> str:
    script = str(SCRIPT_PATH)
    basename = SCRIPT_BASENAME
    return f'''#compdef -P 'python3 *{basename}' {basename} {script}

local -a commands
commands=(
  {" ".join(f'"{c}"' for c in COMMANDS)}
)

_arguments -C \\
  '(-p)--port[串口]' \\
  '(-j)--jobs[并行任务数]' \\
  '(-M)--flash-medium[烧录介质 nand|sd|emmc|nor]' \\
  '(-fb)--flash-baud[烧录波特率 sftool -b]' \\
  '--sd[burn-sd 目标整盘]:device:_files' \\
  '(-i)--image[burn-sd 镜像文件]:image:_files' \\
  '--yes[burn-sd 跳过确认]' \\
  '--repack[burn-sd 前重新 pack-sd-img]' \\
  '(-b -B)--baud[监视波特率]' \\
  '(-h)--help[帮助]' \\
  '*:command:->cmd'

case $state in
  cmd)
    _describe 'command' commands
    ;;
esac
'''


def cmd_complete(shell: str) -> None:
    if shell == "bash":
        print(completion_bash(), end="")
    elif shell == "zsh":
        print(completion_zsh(), end="")
    else:
        raise RuntimeError(f"不支持的 shell: {shell}，可选: bash, zsh")


def cmd_complete_install(shell: str) -> None:
    script = str(SCRIPT_PATH)
    marker_begin = f"# >>> {SCRIPT_BASENAME} completion >>>"
    marker_end = f"# <<< {SCRIPT_BASENAME} completion <<<"
    block = f"""{marker_begin}
eval "$(python3 {script} complete {shell})"
{marker_end}
"""
    if shell == "bash":
        rcfile = Path.home() / ".bashrc"
    elif shell == "zsh":
        rcfile = Path.home() / ".zshrc"
    else:
        raise RuntimeError(f"不支持的 shell: {shell}")

    text = rcfile.read_text(encoding="utf-8") if rcfile.exists() else ""
    for old_begin in (
        "# >>> build_board.py completion >>>",
        "# >>> sf_build_board.py completion >>>",
        "# >>> sf_sdk_tools.py completion >>>",
    ):
        if old_begin in text:
            old_end = old_begin.replace(">>>", "<<<")
            text = re.sub(
                rf"{re.escape(old_begin)}.*?{re.escape(old_end)}\n?",
                "",
                text,
                flags=re.DOTALL,
            )
            rcfile.write_text(text, encoding="utf-8")
            break

    text = rcfile.read_text(encoding="utf-8") if rcfile.exists() else ""
    if marker_begin in text:
        print(f"补全已存在于 {rcfile}")
        return

    with rcfile.open("a", encoding="utf-8") as fh:
        fh.write("\n" + block)
    print(f"已写入 {rcfile}，请执行: source {rcfile}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="openvela My Vendor (SF32LB52) 板级辅助（menuconfig / build / flash / monitor）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""
板级配置（仅改脚本顶部常量，无 -b 参数）:
  BOARD_CONFIG = {BOARD_CONFIG}
  PORT         = {PORT or '(自动检测)'}
  MONITOR_BAUD = {MONITOR_BAUD}
  FLASH        = flasher_args.json (ptab.json 地址 + sftool_param 清单)
  WRAP         = <本固件树>/scripts/wrap_nuttx_image.sh

示例:
  %(prog)s menuconfig
  %(prog)s build              # nuttx + boot，不打任何文件系统
  %(prog)s build-fs           # kv + lfs + fat（全国图）
  %(prog)s pack-sd-img        # SD 整盘 .img → boot_loader/bin/my_vendor_sd.img
  %(prog)s burn-sd --sd /dev/sdb          # 默认刻录 my_vendor_sd.img
  %(prog)s burn-sd --sd /dev/sdb -i foo.img --yes
  %(prog)s burn-sd --sd /dev/sdb --repack # 先 pack-sd-img 再刻录
  %(prog)s build-all          # main → factory → boot → fs
  %(prog)s wrap               # nuttx.bin → nuttx.flash.bin（烧录）
  %(prog)s pack-fw            # OVNX+CRC，改名为 1.0.0-Helm-One.bin 供 OTA
  %(prog)s build -j8
  %(prog)s flash -p /dev/ttyUSB0          # 不含文件系统 / factory（烧录默认 1M）
  %(prog)s flash -fb 3000000 -p /dev/ttyUSB0
  %(prog)s flash -M sd -p /dev/ttyUSB0    # 指定介质（仍默认 1M，加 -fb 提速）
  %(prog)s build-factory                  # 工厂变体 → cmake_out/my_vendor_nsh-factory
  %(prog)s flash-factory -p /dev/ttyUSB0  # 仅 factory@0x62500000
  %(prog)s flash-fs -p /dev/ttyUSB0       # 仅 fs_root.bin
  %(prog)s flash-all -p /dev/ttyUSB0      # boot + factory + main + fs（串口 sftool）
  %(prog)s monitor -p /dev/ttyACM0          # 交互主窗口（自动挂 hub）
  %(prog)s monitor --status -p /dev/ttyACM0
  %(prog)s monitor --follow --no-stdin -p /dev/ttyACM0   # 脚本跟日志
  %(prog)s monitor sys -p /dev/ttyACM0      # 发一条 NSH
  %(prog)s decode 0x10000251 0x10123456
  %(prog)s decode --decode-text < crash.log
  %(prog)s build flash monitor -p /dev/ttyACM0
  %(prog)s build-all flash-all -p /dev/ttyACM0
  %(prog)s savedefconfig
  %(prog)s complete install

NSH Tab 补全（需已编入固件）:
  CONFIG_NSH_READLINE=y + CONFIG_READLINE_TABCOMPLETION=y
  改 defconfig 后须 build；monitor 时按 Tab（发送 0x09）
""",
    )
    parser.add_argument(
        "-p",
        "--port",
        default=None,
        help=f"串口（默认: 顶部 PORT 或自动检测；当前 PORT={PORT!r}）",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=None,
        metavar="N",
        help="并行编译任务数",
    )
    parser.add_argument(
        "-M",
        "--flash-medium",
        choices=("nand", "sd", "emmc", "nor"),
        default=None,
        metavar="MEDIUM",
        dest="flash_medium",
        help="烧录介质（sftool -m）：nand|sd|emmc|nor；"
             "未指定时读 boot_loader/storage.conf，否则默认 nand",
    )
    parser.add_argument(
        "-fb",
        "--flash-baud",
        type=int,
        default=None,
        metavar="RATE",
        dest="flash_baud",
        help=f"烧录波特率 sftool -b（默认 {FLASH_BAUD}）",
    )
    parser.add_argument(
        "-b",
        "-B",
        "--baud",
        type=int,
        default=MONITOR_BAUD,
        metavar="RATE",
        dest="baud",
        help=f"monitor 串口波特率，-b 与 -B 等价（默认 MONITOR_BAUD={MONITOR_BAUD}）",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="flash 时先擦除整片 NAND 再写入（避免 skip 旧 ftab）",
    )
    parser.add_argument(
        "--sd",
        dest="sd_device",
        default=None,
        metavar="DEVICE",
        help="burn-sd 目标整盘，如 /dev/sdb 或 /dev/mmcblk0（不要用分区）",
    )
    parser.add_argument(
        "-i",
        "--image",
        dest="sd_image",
        default=None,
        metavar="FILE",
        help="burn-sd 镜像文件（默认 <本固件树>/boot_loader/bin/my_vendor_sd.img）",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="burn-sd 跳过 YES 确认",
    )
    parser.add_argument(
        "--repack",
        action="store_true",
        help="burn-sd 前强制重新 pack-sd-img（与 --image 同时用时忽略）",
    )
    parser.add_argument(
        "--dtr",
        type=int,
        choices=(0, 1),
        default=None,
        metavar="{0,1}",
        help="monitor 时 DTR 电平；CH340 上 DTR/RTS 常接 RESET/BOOT，"
             "启动无输出请用 --dtr 0（默认 MONITOR_DTR）",
    )
    parser.add_argument(
        "--rts",
        type=int,
        choices=(0, 1),
        default=None,
        metavar="{0,1}",
        help="monitor 时 RTS 电平，启动无输出请用 --rts 0（默认 MONITOR_RTS）",
    )
    parser.add_argument(
        "--no-reset",
        action="store_true",
        help="monitor 打开时不发硬件复位脉冲（默认会复位芯片以抓取 SFBL 起的启动日志）",
    )
    parser.add_argument(
        "--no-smart-bttool",
        action="store_true",
        help="monitor 禁用 bttool 智能回显（使用原始 miniterm）",
    )
    parser.add_argument(
        "--elf",
        type=Path,
        default=None,
        metavar="PATH",
        help="monitor/decode 用的 nuttx ELF（默认 cmake_out/<board>_<cfg>/nuttx）",
    )
    parser.add_argument(
        "--no-decode",
        action="store_true",
        help="monitor 禁用 ELF 崩溃地址解析（默认开启，无需 CONFIG_ALLSYMS）",
    )
    parser.add_argument(
        "--no-ctl",
        action="store_true",
        help="monitor 不启动控制套接字（默认会启动，供 monitor-ctl 转发命令）",
    )
    parser.add_argument(
        "--broker",
        action="store_true",
        help="（默认行为）monitor 走常驻 serial_hub：退出本窗口不关串口、"
             "monitor-ctl 不断线、flash 可 release/acquire 借用",
    )
    parser.add_argument(
        "--no-broker",
        action="store_true",
        help="直连串口（本进程持有；退出会关套接字，另一边的 monitor-ctl 会掉）",
    )
    parser.add_argument(
        "--ctl-sock",
        default=None,
        metavar="PATH",
        help="monitor / monitor-ctl 控制套接字路径"
             f"（默认 /tmp/vela-my-vendor-monitor-$UID.sock）",
    )
    parser.add_argument(
        "--decode-text",
        action="store_true",
        help="decode 子命令：从 stdin 解析整段串口日志（非交互 monitor）",
    )
    parser.add_argument(
        "commands",
        nargs="*",
        metavar="COMMAND",
        help="子命令，可多个: build flash monitor decode ...",
    )
    return parser


def dispatch_command(
    name: str,
    *,
    root: Path,
    out: Path,
    jobs: int,
    port: str | None,
    baud: int,
    smart_bttool: bool = True,
    flash_force: bool = False,
    flash_medium: str | None = None,
    flash_baud: int | None = None,
    dtr: bool | None = None,
    rts: bool | None = None,
    reset: bool = True,
    decode_elf: bool = True,
    elf: Path | None = None,
    ctl_sock: Path | None = None,
    no_ctl: bool = False,
    no_broker: bool = False,
    broker_args: list[str] | None = None,
    broker: bool = False,
    sd_device: str | None = None,
    sd_image: str | None = None,
    yes: bool = False,
    repack: bool = False,
) -> None:
    if name == "build":
        cmd_build(root, jobs)
    elif name == "build-all":
        cmd_build_all(root, jobs)
    elif name == "build-boot":
        cmd_build_boot(root, out, jobs)
    elif name == "build-factory":
        cmd_build_factory(root, jobs)
    elif name == "flash-factory":
        cmd_flash_factory(
            root, port,
            erase_all=flash_force,
            flash_medium=flash_medium,
            flash_baud=flash_baud,
        )
        time.sleep(0.3)
    elif name == "build-fs":
        cmd_build_fs(root)
    elif name == "pack-sd-img":
        cmd_pack_sd_img(root, out)
    elif name == "burn-sd":
        cmd_burn_sd(
            root, out,
            device=sd_device,
            image=sd_image,
            yes=yes,
            repack=repack,
        )
    elif name == "wrap":
        cmd_wrap_nuttx(root, out)
    elif name in ("pack-fw", "pack-ota"):
        cmd_pack_fw(root, out)
    elif name == "menuconfig":
        cmd_menuconfig(root)
    elif name == "savedefconfig":
        cmd_savedefconfig(root)
    elif name == "flash":
        cmd_flash(
            out, port, root,
            erase_all=flash_force,
            flash_medium=flash_medium,
            flash_baud=flash_baud,
        )
        time.sleep(0.3)
    elif name == "flash-all":
        cmd_flash_all(
            out, port, root,
            erase_all=flash_force,
            flash_medium=flash_medium,
            flash_baud=flash_baud,
        )
        time.sleep(0.3)
    elif name == "flash-fs":
        cmd_flash_fs(
            out, port, root,
            erase_all=flash_force,
            flash_medium=flash_medium,
            flash_baud=flash_baud,
        )
        time.sleep(0.3)
    elif name == "monitor":
        cmd_monitor(
            port, baud, root=root, smart_bttool=smart_bttool, dtr=dtr, rts=rts,
            reset=reset, decode_elf=decode_elf, elf=elf,
            ctl_sock=ctl_sock, no_ctl=no_ctl, no_broker=no_broker,
            broker=broker,
        )
    elif name == "monitor-broker":
        # 前台常驻；`monitor-broker status|stop` 是对已在跑的那个说话。
        broker_argv = list(broker_args or [])
        if broker_argv and broker_argv[0] in ("status", "stop"):
            cmd_monitor_broker_ctl(ctl_sock or default_monitor_ctl_sock(),
                                   broker_argv[0])
        else:
            cmd_monitor_broker(
                port, baud, dtr=dtr, rts=rts, reset=reset,
                ctl_sock=ctl_sock, spool=None,
            )
    elif name == "monitor-ctl":
        raise RuntimeError(
            "请改用统一入口，例如:\n"
            f"  python3 {SCRIPT_BASENAME} monitor --status -p /dev/ttyACM0\n"
            f"  python3 {SCRIPT_BASENAME} monitor --follow -p /dev/ttyACM0\n"
            f"  python3 {SCRIPT_BASENAME} monitor sys -p /dev/ttyACM0"
        )
    elif name == "clean":
        cmd_clean(root)
    elif name == "distclean":
        cmd_distclean(root)
    elif name == "fullclean":
        cmd_fullclean(root)
    else:
        raise RuntimeError(f"未知命令: {name}")


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "complete":
        target = sys.argv[2]
        if target in ("bash", "zsh"):
            cmd_complete(target)
            return 0
        if target == "install":
            shell = "bash"
            if "--shell" in sys.argv:
                idx = sys.argv.index("--shell")
                if idx + 1 < len(sys.argv):
                    shell = sys.argv[idx + 1]
            cmd_complete_install(shell)
            return 0

    # monitor 客户端入口（含旧名 monitor-ctl / ctl）：后面的 --status/--follow
    # 等不能交给主 argparse。交互主窗口 `monitor`（无客户端参数）走正常分发。
    entry = _monitor_early_entry(sys.argv[1:])
    if entry is not None:
        idx, _kind = entry  # idx 相对 sys.argv[1:]
        argv1 = sys.argv[1:]
        pre_opts = list(argv1[:idx])
        tail = list(argv1[idx + 1 :])
        sock = None
        if pre_opts:
            args = build_parser().parse_args(pre_opts + ["monitor"])
            if args.ctl_sock:
                sock = Path(args.ctl_sock).expanduser()
            if args.port and not any(a in ("-p", "--port") for a in tail):
                tail = ["-p", args.port, *tail]
        try:
            cmd_monitor_ctl(tail, sock_path=sock)
        except RuntimeError as exc:
            print(f"错误: {exc}", file=sys.stderr)
            return 1
        return 0

    parser = build_parser()

    if len(sys.argv) == 1:
        parser.print_help()
        return 0
    if len(sys.argv) == 2 and sys.argv[1] in ("help", "-h", "--help"):
        parser.print_help()
        return 0

    args = parser.parse_args()
    raw_commands = args.commands or []

    if not raw_commands:
        parser.print_help()
        return 0

    if raw_commands[0] == "help":
        parser.print_help()
        return 0

    if raw_commands[0] == "complete":
        if len(raw_commands) < 2:
            parser.error("complete 需要参数: bash | zsh | install")
        if raw_commands[1] == "install":
            shell = "bash"
            if "--shell" in sys.argv:
                idx = sys.argv.index("--shell")
                if idx + 1 < len(sys.argv):
                    shell = sys.argv[idx + 1]
            cmd_complete_install(shell)
        else:
            cmd_complete(raw_commands[1])
        return 0

    if raw_commands[0] == "decode":
        root = find_openvela_root(SCRIPT_PATH.parent)
        addrs = [
            c for c in raw_commands[1:]
            if c.startswith(("0x", "0X")) or c.isdigit()
        ]
        if args.decode_text:
            cmd_decode(root, args.elf, [], decode_text=True)
        elif addrs:
            cmd_decode(root, args.elf, addrs)
        else:
            parser.error(
                "decode 需要 0x... 地址，或 --decode-text 从 stdin 读日志"
            )
        return 0

    # `monitor-broker status|stop` 是它的参数，不当命令校验。
    extra_ok: set[str] = set()
    if "monitor-broker" in raw_commands:
        extra_ok = {"status", "stop"}
    unknown = [
        c for c in raw_commands if c not in COMMANDS and c not in extra_ok
    ]
    if unknown:
        parser.error(f"未知命令: {', '.join(unknown)}；可选: {_commands_str()}")

    try:
        port = args.port if args.port is not None else PORT
        baud = args.baud
        jobs = job_count(args.jobs)
        smart_bttool = not args.no_smart_bttool
        flash_force = args.force
        flash_medium = args.flash_medium
        flash_baud = args.flash_baud
        sd_device = args.sd_device
        sd_image = args.sd_image
        yes = args.yes
        repack = args.repack
        dtr = None if args.dtr is None else bool(args.dtr)
        rts = None if args.rts is None else bool(args.rts)
        reset = not args.no_reset
        decode_elf = not args.no_decode
        elf = args.elf
        ctl_sock = Path(args.ctl_sock).expanduser() if args.ctl_sock else None
        no_ctl = args.no_ctl
        no_broker = args.no_broker
        broker = args.broker
        # `monitor-broker status|stop` 后面的词交给它自己解析（status/stop）。
        broker_args: list[str] = []
        if "monitor-broker" in raw_commands:
            i = raw_commands.index("monitor-broker")
            broker_args = [
                c for c in raw_commands[i + 1:] if c in ("status", "stop")
            ]

        if raw_commands == ["monitor"]:
            try:
                root = find_openvela_root(SCRIPT_PATH.parent)
                warn_tab_completion(root)
            except RuntimeError:
                root = None
            cmd_monitor(
                port, baud, root=root, smart_bttool=smart_bttool, dtr=dtr, rts=rts,
                reset=reset, decode_elf=decode_elf, elf=elf,
                ctl_sock=ctl_sock, no_ctl=no_ctl, no_broker=no_broker,
                broker=broker,
            )
            return 0

        root = find_openvela_root(SCRIPT_PATH.parent)
        board_config_path(root)
        out = cmake_out_dir(root)

        if BOARD_COMMANDS.intersection(raw_commands):
            print(f"openvela 根目录 : {root}")
            print(f"板级配置       : {BOARD_CONFIG}")
            print(f"CMake 输出目录 : {out}")

        for command in [c for c in raw_commands if c not in extra_ok]:
            dispatch_command(
                command,
                root=root,
                out=out,
                jobs=jobs,
                port=port,
                baud=baud,
                smart_bttool=smart_bttool,
                flash_force=flash_force,
                flash_medium=flash_medium,
                flash_baud=flash_baud,
                dtr=dtr,
                rts=rts,
                reset=reset,
                decode_elf=decode_elf,
                elf=elf,
                ctl_sock=ctl_sock,
                no_ctl=no_ctl,
                no_broker=no_broker,
                broker_args=broker_args,
                broker=broker,
                sd_device=sd_device,
                sd_image=sd_image,
                yes=yes,
                repack=repack,
            )

    except subprocess.CalledProcessError as exc:
        print(f"\n命令失败，退出码: {exc.returncode}", file=sys.stderr)
        return exc.returncode
    except RuntimeError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n已中断。", file=sys.stderr)
        return 130

    return 0


if __name__ == "__main__":
    sys.exit(main())
