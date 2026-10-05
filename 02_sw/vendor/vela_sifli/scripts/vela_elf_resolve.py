#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Resolve NuttX firmware PC/LR addresses via ELF + addr2line (no CONFIG_ALLSYMS).

Used by vela_my_vendor_tools.py monitor (IDF-style panic decode) and as a CLI.

Examples (openvela root):
  python3 vendor/my_vendor/scripts/vela_elf_resolve.py \\
      --elf cmake_out/my_vendor_nsh/nuttx 0x10000251

  python3 vendor/my_vendor/scripts/vela_elf_resolve.py decode-text \\
      --elf cmake_out/my_vendor_nsh/nuttx < crash.log

  echo 'backtrace|2: 0x10000251' | python3 .../vela_elf_resolve.py decode-text --elf nuttx
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# NuttX without CONFIG_ALLSYMS (sched_dumpstack.c)
RE_BACKTRACE_RAW = re.compile(
    r"^(?P<prefix>.*?backtrace\|\s*(?P<pid>\d+):\s*)"
    r"(?P<addrs>(?:0x[0-9a-fA-F]+\s*)+)\s*$"
)
# With CONFIG_ALLSYMS: [pid] [<addr>] sym
RE_BACKTRACE_SYM = re.compile(
    r"^\[\s*(?P<pid>\d+)\]\s*\[<(?P<addr>0x[0-9a-fA-F]+)>\]\s*(?P<sym>.*)$"
)
RE_ASSERT = re.compile(r"Assertion failed", re.IGNORECASE)
RE_ABORT = re.compile(
    r"\b(panic|abort\(\)|HardFault|MemManage|BusFault|UsageFault|stack overflow)\b",
    re.IGNORECASE,
)
RE_HEX_ADDR = re.compile(r"\b(0x[0-9a-fA-F]{8})\b")
RE_PC_LR = re.compile(
    r"^(?P<head>.*\b(?:PC|LR):\s*)"
    r"(?P<addr>0x[0-9a-fA-F]{8}|[0-9a-fA-F]{8})"
    r"(?P<tail>.*)$"
)
RE_FAULT_CASE_LINE = re.compile(r"^(?:>>> )?FAULT_CASE=(\w+)\s*$")
RE_FAULT_BEGIN_LINE = re.compile(r"^(?:!!! )?FAULT_BEGIN=(\w+)\s*$")
RE_ASSERT_FILE = re.compile(
    r"Assertion failed\s*:.*?at file:\s*(?P<file>[^\s]+):(?P<line>\d+)",
    re.IGNORECASE,
)

# 与 test_fault.c g_fault_cases[] / test_fault_harness.py 同步
FAULT_CASE_ZH: dict[str, str] = {
    "bt": "打印当前任务栈（不崩溃）",
    "assert": "DEBUGASSERT(0) 调试断言失败",
    "panic": "PANIC() 内核恐慌",
    "verify": "DEBUGVERIFY 校验失败",
    "abort": "abort() 异常终止",
    "sigabrt": "raise(SIGABRT) 异常信号",
    "sigsegv": "raise(SIGSEGV) 段错误信号",
    "sigfpe": "raise(SIGFPE) 浮点异常信号",
    "sigill": "raise(SIGILL) 非法指令信号",
    "nullwr": "向 NULL 地址写入",
    "nullrd": "从 NULL 地址读取",
    "badcode": "跳转到 0x1 触发 HardFault",
    "execbad": "调用非法地址 0xDEADBEEF",
    "badread": "从非法地址 0xFFFFFFFF 读取",
    "badwrite": "向 SCB 寄存器区写入",
    "unaligned": "非对齐 32 位访问",
    "undef": "执行 UDF 未定义指令",
    "bkpt": "执行 BKPT 断点指令",
    "div0": "整数除零",
    "execram": "跳入 SRAM 中的可执行缓冲",
    "stack": "子任务栈溢出（768B 栈）",
    "stackhw": "深度递归触发硬件栈检查",
    "stackovf": "当前任务栈帧过大",
    "heapdf": "堆 double-free",
    "heapuaf": "堆 use-after-free",
    "heapovf": "写越界 malloc 块",
    "childassert": "子任务 DEBUGASSERT",
    "childnull": "子任务 NULL 写",
    "childundef": "子任务 UDF",
    "childdiv0": "子任务除零",
    "null": "向 NULL 地址写入",
    "heap": "堆 double-free",
}

# addr2line 路径 → 模块简述
_PATH_HINTS: tuple[tuple[str, str], ...] = (
    ("/vendor/my_vendor/boards/", "板级 / bicycle / test"),
    ("/vendor/my_vendor/", "my_vendor 驱动或组件"),
    ("/nuttx/sched/misc/assert", "NuttX 断言处理"),
    ("/nuttx/libs/libc/assert", "libc 断言入口"),
    ("/nuttx/arch/", "架构 / 异常入口"),
    ("/nuttx/sched/", "调度器"),
    ("/nuttx/libs/", "NuttX 库"),
    ("/apps/", "apps 应用"),
)

# 栈回溯由 assert 崩溃转储产生，不能当作用户调用链
_DUMP_STACK_FUNCS = frozenset({
    "up_backtrace",
    "sched_backtrace",
    "sched_dumpstack",
    "dump_running_task",
    "dump_backtrace",
    "dump_tasks",
    "nxsched_foreach",
})

# 栈回溯里视为“系统帧”、小结时不作为【大致位置】
_SYS_FRAME_PREFIXES = (
    "up_backtrace",
    "sched_backtrace",
    "sched_dumpstack",
    "dump_running_task",
    "dump_backtrace",
    "dump_tasks",
    "nxsched_foreach",
    "arm_svcall",
    "arm_doirq",
    "arm_hardfault",
    "arm_memfault",
    "arm_busfault",
    "arm_usagefault",
    "irq_dispatch",
    "nxtask_start",
    "nxtask_startup",
    "_assert",
    "__assert",
    "abort",
    "panic",
)

# ANSI (match ESP-IDF monitor feel)
RED = "\033[91m"
YELLOW = "\033[93m"
CYAN = "\033[36m"
DIM = "\033[2m"
BOLD = "\033[1m"
RESET = "\033[0m"


def parse_addr(spec: str) -> int:
    """Parse NuttX log address (0x10005111 or bare hex 10005111)."""
    s = spec.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 16)


def shorten_location(loc: str) -> str:
    """把 addr2line 绝对路径收成可读短路径。"""
    if not loc or loc == "??:0" or loc.startswith("??"):
        return "未知位置"
    if ":" in loc:
        path, line = loc.rsplit(":", 1)
        line = re.sub(r"\s*\(discriminator[^)]*\)", "", line).strip()
    else:
        path, line = loc, "?"
    for marker in ("/vendor/my_vendor/", "/nuttx/", "/apps/"):
        idx = path.find(marker)
        if idx >= 0:
            path = path[idx + 1 :]
            break
    else:
        path = Path(path).name
    return f"{path}:{line}"


def location_hint(loc: str) -> str:
    """根据源文件路径给出中文模块提示。"""
    if not loc or loc.startswith("?"):
        return ""
    path = loc.rsplit(":", 1)[0]
    name = Path(path).name
    hints: dict[str, str] = {
        "test_fault.c": "板级故障注入",
        "test_main.c": "NSH test 命令分发",
        "lib_assert.c": "断言库",
        "assert.c": "内核断言处理",
        "sched_dumpstack.c": "栈打印",
        "task_start.c": "任务启动",
        "task_startup.c": "任务入口",
    }
    if name in hints:
        return hints[name]
    for needle, hint in _PATH_HINTS:
        if needle in path:
            return hint
    return ""


def fault_case_zh(name: str) -> str:
    aliases = {"null": "nullwr", "heap": "heapdf"}
    name = aliases.get(name, name)
    return FAULT_CASE_ZH.get(name, f"故障用例 {name}")


def sym_rel_location(sym: ResolvedSymbol) -> str:
    """vendor/my_vendor/.../test_fault.c:111"""
    if not sym.ok:
        return "?"
    return shorten_location(sym.location)


@dataclass(frozen=True)
class ResolvedSymbol:
    addr: int
    function: str
    location: str  # file:line or "?"

    @property
    def ok(self) -> bool:
        return self.function != "??" and self.location != "??:0"


class ElfSymbolResolver:
    """addr2line-backed resolver with cache."""

    def __init__(
        self,
        elf: Path,
        *,
        toolchain_prefix: str | None = None,
        demangle: bool = True,
    ) -> None:
        self.elf = elf.resolve()
        if not self.elf.is_file():
            raise FileNotFoundError(f"ELF not found: {self.elf}")
        self._addr2line = self._find_addr2line(toolchain_prefix)
        self._demangle = demangle
        self._cache: dict[int, ResolvedSymbol] = {}

    @staticmethod
    def _find_addr2line(prefix: str | None) -> str:
        if prefix:
            tool = f"{prefix}addr2line"
            if shutil.which(tool) or Path(tool).is_file():
                return tool
        for cand in ("arm-none-eabi-addr2line",):
            if shutil.which(cand):
                return cand
        root = default_openvela_root()
        prebuilt = (
            root
            / "prebuilts/gcc/linux-x86_64/arm-none-eabi/bin/arm-none-eabi-addr2line"
        )
        if prebuilt.is_file():
            return str(prebuilt)
        raise RuntimeError(
            "arm-none-eabi-addr2line not found; add prebuilts GCC to PATH"
        )

    @staticmethod
    def _addr_candidates(addr: int) -> list[int]:
        a = addr & 0xFFFFFFFF
        out: list[int] = []
        for v in (a, a & ~1, a - 1, a + 1):
            if v not in out:
                out.append(v)
        return out

    def resolve(self, addr: int) -> ResolvedSymbol:
        key = addr & 0xFFFFFFFF
        if key in self._cache:
            return self._cache[key]

        sym = ResolvedSymbol(key, "??", "??:0")
        for cand in self._addr_candidates(key):
            try:
                args = [self._addr2line, "-e", str(self.elf), "-f", "-C"]
                if not self._demangle:
                    args.append("-n")
                args.append(f"0x{cand:x}")
                proc = subprocess.run(
                    args,
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=10,
                )
            except (OSError, subprocess.TimeoutExpired):
                break
            if proc.returncode != 0 or not proc.stdout.strip():
                continue
            lines = proc.stdout.strip().splitlines()
            func = lines[0].strip() if lines else "??"
            loc = lines[1].strip() if len(lines) > 1 else "??:0"
            if func == "??" or loc.startswith("?:"):
                continue
            sym = ResolvedSymbol(key, func, loc)
            break

        self._cache[key] = sym
        return sym

    def format_frame(self, index: int, addr: int, *, use_color: bool) -> str:
        sym = self.resolve(addr)
        if use_color:
            if sym.ok:
                return (
                    f"  {DIM}#{index}{RESET}  "
                    f"{YELLOW}{addr:#010x}{RESET}  "
                    f"{BOLD}{sym.function}{RESET}  "
                    f"{CYAN}at {shorten_location(sym.location)}{RESET}"
                )
            return (
                f"  {DIM}#{index}{RESET}  "
                f"{YELLOW}{addr:#010x}{RESET}  "
                f"{DIM}(no debug line){RESET}"
            )
        if sym.ok:
            return (
                f"  #{index}  {addr:#010x}  {sym.function}  "
                f"at {shorten_location(sym.location)}"
            )
        return f"  #{index}  {addr:#010x}  (no debug line)"


class PanicSerialDecoder:
    """Line-at-a-time serial decoder for monitor / log replay."""

    # Incomplete lines containing these are held until '\\n' (crash decode).
    # NSH readline echoes single keypresses without '\\n' and must pass through.
    _DECODE_PARTIAL_MARKERS = (
        "backtrace",
        "Assertion",
        "FAULT_CASE",
        "FAULT_BEGIN",
        ">>> FAULT",
        "!!! FAULT",
        "up_dump",
        "dump_stack",
        "register dump",
    )

    def __init__(
        self,
        resolver: ElfSymbolResolver | None,
        *,
        use_color: bool | None = None,
    ) -> None:
        self.resolver = resolver
        self.use_color = (
            use_color
            if use_color is not None
            else sys.stdout.isatty() and os.environ.get("NO_COLOR") is None
        )
        self._buf = ""
        self._warned_no_elf = False
        self._fault_case: str | None = None
        self._fault_summary_done = False
        self._best_bt: tuple[int, list[int], str] | None = None  # score, addrs, pid

    def _fmt_cn_line(self, text: str, *, color: str = "") -> str:
        if self.use_color and color:
            return f"{color}{text}{RESET}\n"
        return text + "\n"

    def _is_user_fault_frame(self, sym: ResolvedSymbol) -> bool:
        if not sym.ok:
            return False
        fn = sym.function
        if fn.startswith("fault_body_") or fn in ("fault_fire", "fault_run", "fault_usage"):
            return True
        if "/vendor/my_vendor/" in sym.location or "/apps/" in sym.location:
            return True
        if any(fn.startswith(p) for p in _SYS_FRAME_PREFIXES):
            return False
        return fn.startswith("fault_")

    def _pick_primary_frame(
        self, addrs: list[int]
    ) -> tuple[ResolvedSymbol | None, list[ResolvedSymbol], ResolvedSymbol | None]:
        """用户触发点、用户链、异常入口（若有）。"""
        if self.resolver is None:
            return None, [], None
        user: list[ResolvedSymbol] = []
        exc: ResolvedSymbol | None = None
        for addr in addrs:
            sym = self.resolver.resolve(addr)
            if not sym.ok:
                continue
            fn = sym.function
            if fn.startswith("arm_") and fn.endswith("fault"):
                if exc is None:
                    exc = sym
                continue
            if self._is_user_fault_frame(sym):
                user.append(sym)
        primary = next(
            (s for s in user if s.function.startswith("fault_body_")),
            user[0] if user else exc,
        )
        return primary, user, exc

    def _is_dump_handler_stack(self, addrs: list[int]) -> bool:
        if self.resolver is None:
            return False
        for addr in addrs[:4]:
            sym = self.resolver.resolve(addr)
            if sym.ok and sym.function in _DUMP_STACK_FUNCS:
                return True
        return False

    def _is_complete_user_stack(self, user_frames: list[ResolvedSymbol]) -> bool:
        fns = {s.function for s in user_frames}
        has_body = any(f.startswith("fault_body_") for f in fns)
        has_fire = "fault_fire" in fns or "fault_run" in fns
        return has_body and has_fire

    def _score_backtrace(self, addrs: list[int]) -> int:
        if self._is_dump_handler_stack(addrs):
            return -1
        _, user_frames, exc = self._pick_primary_frame(addrs)
        if not user_frames:
            return -1
        score = len(user_frames) * 10
        fns = {s.function for s in user_frames}
        if any(f.startswith("fault_body_") for f in fns):
            score += 30
        if "fault_fire" in fns or "fault_run" in fns:
            score += 40
        if "test_main" in fns:
            score += 20
        if exc is not None:
            score += 5
        return score

    def _consider_backtrace(self, addrs: list[int], pid: str) -> str:
        """记录最佳用户栈；完整链就绪时输出一次【故障分析】。"""
        score = self._score_backtrace(addrs)
        if score < 0:
            return ""
        _, user_frames, _ = self._pick_primary_frame(addrs)
        if self._best_bt is None or score > self._best_bt[0]:
            self._best_bt = (score, addrs, pid)
        if self._fault_summary_done:
            return ""
        if not self._is_complete_user_stack(user_frames):
            return ""
        best_score, best_addrs, best_pid = self._best_bt
        if score < best_score:
            return ""
        self._fault_summary_done = True
        return self._backtrace_cn_summary(best_addrs, best_pid)

    def _backtrace_cn_summary(self, addrs: list[int], pid: str) -> str:
        primary, user_frames, exc = self._pick_primary_frame(addrs)
        if not user_frames or primary is None:
            return ""

        case = self._fault_case or "?"
        desc = fault_case_zh(case)
        line_ref = sym_rel_location(primary)
        hint = location_hint(primary.location)
        loc_line = (
            f"【大致位置】{line_ref}，函数 {primary.function}"
            + (f"（{hint}）" if hint else "")
        )

        chain_parts: list[str] = []
        if exc is not None:
            chain_parts.append(
                f"{exc.function}@{sym_rel_location(exc)}"
            )
        for s in user_frames:
            chain_parts.append(f"{s.function}@{sym_rel_location(s)}")
        chain = " → ".join(chain_parts) if chain_parts else (
            f"{primary.function}@{line_ref}"
        )

        lines = [
            f"【故障分析】用例 {case}：{desc}",
            loc_line,
            f"【调用链】{chain}",
            f"【任务】tid {pid}",
        ]
        color = BOLD + YELLOW if self.use_color else ""
        return "".join(self._fmt_cn_line(ln, color=color) for ln in lines)

    def _should_buffer_partial(self, buf: str) -> bool:
        """True: hold fragment until a full line (crash decode). False: emit now (NSH echo)."""
        if not buf:
            return False
        # readline echoes one key at a time; Tab completion also streams without '\\n' first.
        if len(buf) <= 3:
            return False
        lowered = buf.lower()
        return any(marker.lower() in lowered for marker in self._DECODE_PARTIAL_MARKERS)

    def feed(self, data: str) -> str:
        if not data:
            return ""
        self._buf += data
        out: list[str] = []
        while True:
            nl = self._buf.find("\n")
            if nl < 0:
                if self._buf and not self._should_buffer_partial(self._buf):
                    out.append(self._buf)
                    self._buf = ""
                break
            line = self._buf[: nl + 1]
            self._buf = self._buf[nl + 1 :]
            out.append(self._process_line(line))
        return "".join(out)

    def flush(self) -> str:
        tail = ""
        if self._buf:
            if self._should_buffer_partial(self._buf):
                tail = self._process_line(self._buf)
            else:
                tail = self._buf
            self._buf = ""
        if not self._fault_summary_done and self._best_bt is not None:
            _, addrs, pid = self._best_bt
            summary = self._backtrace_cn_summary(addrs, pid)
            if summary:
                self._fault_summary_done = True
                tail += summary
        return tail

    def _highlight_severity(self, line: str) -> str:
        if not self.use_color:
            return line
        if RE_ASSERT.search(line):
            return f"{RED}{BOLD}{line.rstrip()}{RESET}\n"
        if RE_ABORT.search(line):
            return f"{RED}{line.rstrip()}{RESET}\n"
        return line

    def _process_line(self, line: str) -> str:
        stripped = line.rstrip("\r\n")
        m = RE_FAULT_CASE_LINE.match(stripped)
        if m:
            self._fault_case = m.group(1)
            self._fault_summary_done = False
            self._best_bt = None
            if self.use_color:
                return f"{YELLOW}>>> FAULT_CASE={m.group(1)}{RESET}\n"
            return f">>> FAULT_CASE={m.group(1)}\n"
        m = RE_FAULT_BEGIN_LINE.match(stripped)
        if m:
            self._fault_case = m.group(1)
            if self.use_color:
                return f"{CYAN}!!! FAULT_BEGIN={m.group(1)}{RESET}\n"
            return f"!!! FAULT_BEGIN={m.group(1)}\n"

        if self.resolver is None:
            if not self._warned_no_elf:
                self._warned_no_elf = True
                note = (
                    "[vela decode] ELF 未配置或不存在，仅透传串口"
                    "（build 后 monitor 会自动用 cmake_out/.../nuttx）\n"
                )
                return note + self._highlight_severity(line)
            return self._highlight_severity(line)

        m = RE_BACKTRACE_RAW.match(line.rstrip("\r\n"))
        if m:
            addrs = [parse_addr(x) for x in RE_HEX_ADDR.findall(m.group("addrs"))]
            head = self._highlight_severity(f"{m.group('prefix').rstrip()}\n")
            if not addrs:
                return head
            frames = [
                self.resolver.format_frame(i, a, use_color=self.use_color)
                for i, a in enumerate(addrs)
            ]
            if self.use_color:
                banner = (
                    f"{YELLOW}--- ELF backtrace (tid {m.group('pid')}) ---{RESET}\n"
                )
            else:
                banner = f"--- ELF backtrace (tid {m.group('pid')}) ---\n"
            body = head + banner + "\n".join(frames) + "\n"
            body += self._consider_backtrace(addrs, m.group("pid"))
            return body

        m = RE_BACKTRACE_SYM.match(line.rstrip("\r\n"))
        if m:
            addr = parse_addr(m.group("addr"))
            sym = self.resolver.resolve(addr)
            base = line if not self.use_color else (
                f"[{m.group('pid')}] [<{m.group('addr')}>] "
                f"{YELLOW}{m.group('sym').strip()}{RESET}\n"
            )
            if sym.ok:
                extra = self.resolver.format_frame(0, addr, use_color=self.use_color)
                return base + extra + "\n"
            return base

        m = RE_PC_LR.match(line.rstrip("\r\n"))
        if m:
            addr = parse_addr(m.group("addr"))
            sym = self.resolver.resolve(addr)
            highlighted = self._highlight_severity(line)
            if sym.ok:
                frame = self.resolver.format_frame(0, addr, use_color=self.use_color)
                return highlighted.rstrip("\n") + "\n" + frame + "\n"
            return highlighted

        if RE_ASSERT_FILE.search(stripped):
            return self._highlight_severity(line)

        # Inline code addresses in XIP / SRAM text range
        if RE_ASSERT.search(line) or "backtrace" in line.lower():
            return self._highlight_severity(line)

        return line


def default_openvela_root() -> Path:
    here = Path(__file__).resolve()
  # vendor/my_vendor/scripts/vela_elf_resolve.py
    return here.parent.parent.parent.parent


def default_nuttx_elf(root: Path | None = None) -> Path:
    root = root or default_openvela_root()
    return root / "cmake_out" / "my_vendor_nsh" / "nuttx"


def _cmd_decode_addrs(resolver: ElfSymbolResolver, addrs: list[str]) -> int:
    for i, spec in enumerate(addrs):
        addr = parse_addr(spec)
        sym = resolver.resolve(addr)
        print(resolver.format_frame(i, addr, use_color=sys.stdout.isatty()))
    return 0


def _cmd_decode_text(resolver: ElfSymbolResolver) -> int:
    decoder = PanicSerialDecoder(resolver)
    data = sys.stdin.read()
    sys.stdout.write(decoder.feed(data))
    sys.stdout.write(decoder.flush())
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Resolve NuttX crash PCs from ELF (addr2line), no ALLSYMS.",
    )
    parser.add_argument(
        "--elf",
        type=Path,
        default=None,
        help=f"nuttx ELF (default: {default_nuttx_elf()})",
    )
    parser.add_argument(
        "--toolchain-prefix",
        default="arm-none-eabi-",
        help="Cross toolchain prefix for addr2line",
    )
    parser.add_argument(
        "--decode-text",
        action="store_true",
        help="Read monitor log from stdin and annotate lines",
    )
    parser.add_argument(
        "addrs",
        nargs="*",
        help="0x... addresses (default when not using --decode-text)",
    )

    args = parser.parse_args(argv)
    elf = args.elf or default_nuttx_elf()
    resolver = ElfSymbolResolver(
        elf, toolchain_prefix=args.toolchain_prefix
    )

    if args.decode_text:
        return _cmd_decode_text(resolver)
    if args.addrs:
        return _cmd_decode_addrs(resolver, args.addrs)
    parser.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
