# 上游必改件（pitch）

`vendor/my_vendor` 跟 `repo sync` 走。`nuttx` / `apps` / `zblue` 每次整树同步都会回到上游干净树。

本目录只收 **不能还原、也不在 vendor 编译单元里的功能性改动**。漏掉它们时 NSH 往往仍能起来，但：

- 手机已连接时扫 HR/CSC/CPS **起不来**（BLE）
- 本板 `CONFIG_MYVENDOR_LVGL_STACK` 与 Vela 自带 `lvgl` target **撞车**，或走官方 LVGL + EPIC 时缺 SiFli 头文件（LVGL）

HCI H4（`nuttx/drivers/serial/uart_bth4.c`）**不在本目录**。环满丢扫描包已还原；读侧超时和卡住恢复在
**Vela 自己的 `apps/frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c`** 与 BLE 诊断里
（见 [ble-hci-h4-rx-reopen.md](ble-hci-h4-rx-reopen.md)）。

## 抽换件是否真的编进去了（每天用得上）

2026-09-18 踩过一次：有个抽换件**根本不在编译库里**，改了半天的补丁没有任何效果（该文件已剔除）。
现在每个抽换件都带两行标记：

1. `#pragma message("myvendor override compiled: vela_override/<rel> -- 上游 <仓库>:<路径>@<blob> -- <原因>")`
   —— 编译期 **note（info）**，带上游 id 与原因；
2. 一个 `.myvendor_marker` 段常量（进镜像，见文末"镜像侧标记的坑与做法"）。

**为什么编译期那行只写 note 不写警告（2026-09-19 用户要求）**：`#pragma GCC warning` 在量产构建里
会被 `-w` / 全局警告压制关掉，note 关不掉（GCC 没有 `-Wno-notes`）。
裸 `#warning` 更不行：抽换件 target 带 `-Wno-cpp`，正是用来关它的，实测**一个字都不出**。

两条命令各自回答一个问题：

```sh
# ① 本次构建真的编译了哪些抽换件、各自为什么抽换（原因自动取自 vela_override/CMakeLists.txt 的表）
ninja 2>&1 | grep "override compiled"

# ② 哪些抽换件进了镜像：数标记常量本身（在 ELF 或裸 bin 上都行，strip 后也还在）
strings -a cmake_out/<cfg>/nuttx.bin | grep -c '^vela_override/'      # 期望 = 抽换件+补丁件总数（现 19）
```

- `#pragma message` 只在文件真的被**重新编译**时出现（ninja 判定无变更就不重编），所以
  "为什么改了没生效"看第二条，"这次构建编了什么"看第一条。
- 新增抽换件请照抄那两行标记（要带原因）；**原因优先写进 `vela_override/CMakeLists.txt` 头部那张表**，
  标记从表里取，别在两处各写一遍。
- 反过来：标记在日志/镜像里没出现 = 该文件没参与编译（或没进镜像），先查接线，别再改代码。

**镜像侧标记的坑与做法（2026-09-19 实测）**：标记常量没有任何代码引用，链接期 `--gc-sections` 会把它
**当垃圾段整个丢掉** —— 最小复现里 `used`、`section(".rodata")`、`used + section + retain` 三种写法
一条都不留（`retain` 在本工具链 GCC 13.4 上还被 `-Wattributes` 忽略）。所以它原来**根本没进镜像**，
`strings nuttx | grep vela_override/` 那 42 条全是 DWARF 调试路径（未 strip 才有）。

现在的做法（已生效）：

- 标记常量放进独立段 **`.myvendor_marker`**（不是 `.rodata`）；
- 板级链接脚本 `boards/sf32lb52/my_vendor/scripts/ld.script` 里 `KEEP(*(.myvendor_marker))`，
  `KEEP` 是 GC 根，段就不会被丢；段在 `.text` 里 → 落在 **flash**，**不占 SRAM**；
- 于是裸 `nuttx.bin`、strip 过的镜像里都有：`strings -a <bin> | grep '^vela_override/'`（现 19 条，
  约 2.7 KB flash）；`strings -a -e S` 还能看整行（含上游 blob 与原因）；
- 判断"进没进镜像"用这条命令；"这次构建编了什么"仍看构建日志（命令 ①）。
- ⚠️ 新增抽换件时**段名必须写 `.myvendor_marker`** —— 写成 `.rodata` 会被静默丢掉。

- **被补丁脚本生成的副本**（`build/myvendor_zblue/myvendor_buf.c` / `myvendor_gatt.c` / `myvendor_hci_core.c`）
  在生成时被脚本**追加了同样的两行标记**，所以它们被编译时也会打出
  `note: '#pragma message: myvendor override compiled(patch): vela_override/patch_*.py -- 上游 <仓库> / <路径>@<blob> -- <原因>'` ——
  一条 note 同时回答"这份 .o 是补丁产物 + 打的是哪个脚本 + 基于哪个上游版本"。

详细背景仍以 [required_patches.md](../required_patches.md) 为准。这里给的是 **改法 + 可直接覆盖的文件 + 补丁**。

### 每个补丁文档必须留的两样（2026-09-19 起的要求）

版本一换，原来写的"改哪个函数"就可能对不上号，所以每份补丁文档都要带：

1. **组件 git id**：仓库名 + 写入时的 `HEAD` + **该文件在 HEAD 的 blob hash**
   （`git -C <repo> rev-parse HEAD:<path>`），再给一条 `git apply --check` 自检命令 ——
   HEAD 动了但 blob 没动 ⇒ 文件没被改过，照文档改；blob 也变了 ⇒ 先看上游怎么改的再重做。
2. **改动前 / 改动后的整段函数**（不是片段、不是省略号），照着能直接抄回去。

两个范例：[zblue-scan-stop-null-ctx.md](zblue-scan-stop-null-ctx.md)（带 blob 与自检）、
[hci_h4](ble-hci-h4-rx-reopen.md)。`ble.md` / `lvgl.md` 是更早的写法，版本对照以本文上方的
HEAD 表为准。

### hci_h4.c 怎么应用（2026-09-18 新收，整文件 + 补丁两份都留）

`repo sync` 之后这个文件一定会回到上游版本，所以两份都放在本目录里：

```bash
# A. 与上表 HEAD 一致时（最省事）
cp vendor/my_vendor/docs/pitch/replace/frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c \
   frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c

# B. 上游同一处有漂移时（注意仓库根是 frameworks/connectivity/bluetooth）
git -C frameworks/connectivity/bluetooth apply \
  ../../vendor/my_vendor/docs/pitch/patches/vela-hci-h4-rx-reopen.patch

# 打完确认：应能看到 h4_rx_reopen 与 syslog(LOG_WARNING, "H4: rx reopen
git -C frameworks/connectivity/bluetooth diff -- service/stacks/zephyr/hci_h4.c
```

**别按 `apps/frameworks/...` 去改**：`apps/frameworks` 只是指向 `../frameworks/` 的符号链接，
两者是同一个文件（`compile_commands.json` 里只有一条
`libbluetooth.dir/service/stacks/zephyr/hci_h4.c.o`）。想确认改动进没进镜像：
`strings nuttx | grep "H4: rx reopen"`。

---

## 目录

```
docs/pitch/
├── README.md                       本文件：总览、打法、规范、抽换层指向
├── ble.md                          zblue id.c 扫描 -EACCES（含 git id + 整段函数前后）
├── lvgl.md                         apps 两个构建脚本 + 官方 LVGL 的 Kconfig（含 git id + 完整 diff）
├── ble-hci-h4-rx-reopen.md         Vela hci_h4.c 读侧有界重开（含 git id + 整段函数前后）
├── zblue-scan-stop-null-ctx.md     zblue scan.c 空指针（板子 panic 根因；含 git id + 整段函数前后）
├── overrides.md                    ★ 抽换层索引：19 个抽换件替的上游/HEAD/blob/原因 + 校验脚本
├── replace/                        相对 OpenVela 根的可覆盖树
│   ├── README.md
│   ├── apps/graphics/lvgl/{CMakeLists.txt,Makefile}
│   ├── external/zblue/zblue/subsys/bluetooth/host/id.c
│   └── frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c
└── patches/
    ├── zblue-id-scan-eacces.patch
    ├── apps-lvgl-myvendor-stack.patch
    ├── apps-lvgl-kconfig.patch          ← 在 apps/graphics/lvgl/lvgl（嵌套 project）里打
    ├── vela-hci-h4-rx-reopen.patch
    └── zblue-scan-stop-null-ctx.patch
```

| 项 | 上游路径 | 替换文件 | 补丁 | 说明 |
|----|----------|----------|------|------|
| BLE | `external/zblue/zblue/subsys/bluetooth/host/id.c` | [replace/.../id.c](replace/external/zblue/zblue/subsys/bluetooth/host/id.c) | [zblue-id-scan-eacces.patch](patches/zblue-id-scan-eacces.patch) | [ble.md](ble.md) |
| LVGL | `apps/graphics/lvgl/CMakeLists.txt` | [replace/.../CMakeLists.txt](replace/apps/graphics/lvgl/CMakeLists.txt) | 含在 [apps-lvgl-myvendor-stack.patch](patches/apps-lvgl-myvendor-stack.patch) | [lvgl.md](lvgl.md) |
| LVGL | `apps/graphics/lvgl/Makefile` | [replace/.../Makefile](replace/apps/graphics/lvgl/Makefile) | 同上 | [lvgl.md](lvgl.md) |
| BLE H4 | `frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c`（**独立仓库 `frameworks_bluetooth`**；`apps/frameworks` 是指向 `../frameworks/` 的符号链接） | [replace/.../hci_h4.c](replace/frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c) | [vela-hci-h4-rx-reopen.patch](patches/vela-hci-h4-rx-reopen.patch) | [ble-hci-h4-rx-reopen.md](ble-hci-h4-rx-reopen.md) |
| BLE scan | `external/zblue/zblue/subsys/bluetooth/host/scan.c` | 只留补丁（文件 2000+ 行，整文件覆盖不划算） | [zblue-scan-stop-null-ctx.patch](patches/zblue-scan-stop-null-ctx.patch) | [zblue-scan-stop-null-ctx.md](zblue-scan-stop-null-ctx.md) |
| LVGL config | `apps/graphics/lvgl/lvgl/Kconfig`（**嵌套 project `apps_graphics_lvgl`，不是 `apps`**） | 只留补丁（46 KB，整文件覆盖不划算） | [apps-lvgl-kconfig.patch](patches/apps-lvgl-kconfig.patch) | [lvgl.md](lvgl.md) |
| NuttX cmake | `nuttx/cmake/nuttx_kconfig.cmake`（**`nuttx` 仓库**，构建脚本，抽换层够不着） | 只留补丁 | [nuttx-kconfig-quote-blank.patch](patches/nuttx-kconfig-quote-blank.patch) | [nuttx-kconfig-quote-blank.md](nuttx-kconfig-quote-blank.md) |

对照本工作区当时的上游 HEAD：

| 仓库 | HEAD |
|------|------|
| `external/zblue/zblue` | `6f79fb2a0f8` |
| `apps` | `017abdbef` |
| `apps/graphics/lvgl/lvgl`（嵌套 project） | `0f6336dab` |
| `frameworks/connectivity/bluetooth` | `43945bc1`（`frameworks_bluetooth` 仓库，h4 改动在这里） |

上游若已前进，优先打 `patches/`；整文件覆盖可能把无关上游改动冲掉。

---

## 一键打上全部补丁（在 OpenVela 根目录执行）

**六份补丁 = 上游的全部改动**（逐份说明、id 表、`--check` 命令见 [patches/README.md](patches/README.md)）。
下面这段整段复制到 shell 里跑即可，**幂等**：已打过的会显示"已打过，跳过"，打不上的会指路。

```bash
cd <OpenVela 根目录>
P=vendor/my_vendor/docs/pitch/patches
apply() {   # $1=仓库（相对根目录） $2=补丁文件名
  repo=$1 patch=$2
  if git -C "$repo" apply --check -R "$PWD/$P/$patch" 2>/dev/null; then
    echo "  =   已打过，跳过   $patch"
  elif git -C "$repo" apply --check "$PWD/$P/$patch" 2>/dev/null; then
    git -C "$repo" apply "$PWD/$P/$patch" && echo "  ok  已打好        $patch"
  else
    echo "  !!  打不上          $patch   —— 基版本对不上，按同名说明文档手工改" >&2
  fi
}
apply external/zblue/zblue              zblue-id-scan-eacces.patch
apply external/zblue/zblue              zblue-scan-stop-null-ctx.patch        # panic 修复
apply frameworks/connectivity/bluetooth vela-hci-h4-rx-reopen.patch
apply apps                              apps-lvgl-myvendor-stack.patch
apply apps/graphics/lvgl/lvgl           apps-lvgl-kconfig.patch               # ← 嵌套仓库
apply nuttx                             nuttx-kconfig-quote-blank.patch       # ← 构建脚本（空行不再当参数）

# 复用已有的构建目录时，让 config 重新推导一次（全新机器不用管）
for c in cmake_out/*/.config; do
  [ -f "$c" ] || continue
  echo "  提醒：删掉 $c 再编，否则缺的 CONFIG_LV_* 不会自己回来"
done
```

三个容易踩的点：

1. 最后一份补丁的仓库是 **嵌套 project `apps/graphics/lvgl/lvgl`**（manifest 里叫 `apps_graphics_lvgl`），
   在 `apps` 里执行会报"路径不在 HEAD 中"；它不打的话，config 里会缺 `LV_SUNDAY_STR` 之类的符号，
   编 `widgets/calendar/lv_calendar.c` 直接报 `error: 'CONFIG_LV_SUNDAY_STR' undeclared`。
2. 打完 Kconfig 那份**必须**让 `.config` 重新推导一次（删掉旧的，或换个干净的构建目录），
   否则旧配置里缺的符号不会自己回来。
3. `git apply` 失败不要强行 `--3way`/覆盖，按各说明文档里的"上游 HEAD + 文件 blob"确认基版本后手工改。

不想用 `git apply` 时，也可以整文件覆盖：见 [replace/README.md](replace/README.md)。

---

## 打完怎么确认

```bash
# ① 全树只应剩这 6 个文件被改（外加 nist-sts 那种重叠检出的噪音）
repo status -j16 | grep -v '\.mimosa' | grep -B1 '^ [-mad]' | grep '^project'
# ② 六份补丁反向 dry-run 全过（= 补丁与本机现状逐字相符）
P=vendor/my_vendor/docs/pitch/patches
for x in "external/zblue/zblue:zblue-id-scan-eacces.patch" \
         "external/zblue/zblue:zblue-scan-stop-null-ctx.patch" \
         "frameworks/connectivity/bluetooth:vela-hci-h4-rx-reopen.patch" \
         "apps:apps-lvgl-myvendor-stack.patch" \
         "apps/graphics/lvgl/lvgl:apps-lvgl-kconfig.patch" \
         "nuttx:nuttx-kconfig-quote-blank.patch"; do
  git -C "${x%%:*}" apply --check -R "$PWD/$P/${x##*:}" && echo "  ok ${x##*:}"
done
# ③ 编完之后：镜像里应能数到抽换件标记（数量 = vela_override 里的 .c + patch_*.py）
strings -a cmake_out/my_vendor_nsh/nuttx.bin | grep -c '^vela_override/'
```

开机：`ble_companion: advertising started`；手机已连接时 `test sensor scan` 能出表。

---

## 为什么不能全部放进 vendor

`vela_override/` 只能按 **同名 `.c`** 抽换已经进 target 的编译单元（`zblue`、`libbluetooth`、`fs` …）。

| 改动 | 能否只靠 vendor |
|------|-----------------|
| `id.c` | 长期可以：放到 `vela_override/zblue/id.c`（CMake 已写「预留」）。在做成抽换前，每次 `repo sync` 仍要打本目录这一份。 |
| `apps/graphics/lvgl/CMakeLists.txt` | **不能**。这是 apps 入口脚本，不是 `lvgl` target 里的 `.c`。`CONFIG_MYVENDOR_LVGL_STACK` 的 `return()` 必须写在这份官方 CMake 里，否则会和 board `add_subdirectory(.../lvgl myvendor_lvgl)` 重复定义 `lvgl`。 |
| `apps/graphics/lvgl/Makefile` | 不能。本板走 CMake；Makefile 给 make 构建 / 官方 LVGL+EPIC 留后路。 |

`id.c` 做成抽换之后，本目录 BLE 替换件可以删，LVGL 两份构建脚本仍要留。

---

## 抽换层（`vela_override/`）：不收录补丁，但收录**索引**

抽换件（同名 `.c` 顶掉上游、`patch_*.py` 构建期改写）不会随 `repo sync` 丢失，所以不需要在本目录留补丁；
但它们**替的是谁、基于哪个上游版本**必须能查到 —— 见 [overrides.md](overrides.md)：
19 个抽换件的「上游仓库 / 上游文件 / 写入时 HEAD / 该文件 blob / 为什么抽换」一张表，
外加"每次 sync 后校验这张表还准不准"的脚本，以及上游真变了时的两条路（跟改 / 退役）。

一手信息在各抽换件的**文件头**（`vela_override/**/*.{c,py}` 顶部"抽换件说明"块），`overrides.md` 是它的快照。

## 明确不收录

表里是**不进本目录**的东西：每一项分开写清"做了什么 / 为什么"，并带**上游仓库 @ HEAD + 文件路径 + 该文件 blob**。
版本一换就看最后一列 —— blob 对不上 ⇒ 上游这段被改过，先 `git -C <repo> diff <blob> HEAD -- <path>` 看改了什么，
再决定"跟改 / 放弃这条抽换"。抽换件的完整索引与自动校验脚本见 [overrides.md](overrides.md)（两处同源生成）。

**bluetooth → `libbluetooth`**

| 文件 | 做了什么 | 为什么 | 上游（仓库 @ HEAD / 路径 / blob） |
|---|---|---|---|
| `vela_override/bluetooth/sal_le_advertise_interface.c` | 广播改走 legacy `bt_le_adv_start/stop`（去掉 Ext Adv 与那个 1 s 超时定时器），并与 scan 共用跨角色 ticket 队列 | SF32 走 legacy bt_le_adv_start（无 Ext Adv）；与 scan 共用跨角色 ticket 队列，等待有界 2 s | `frameworks/connectivity/bluetooth` @ `43945bc1d`<br>`service/stacks/zephyr/sal_le_advertise_interface.c`<br>blob `9d4ba4033133` |
| `vela_override/bluetooth/sal_le_scan_interface.c` | `-EALREADY` 重试、`options=0`；跨角色 ticket 的等待从"无限"改成"有界 2 s 后强制推进" | -EALREADY 重试、options=0；与 adv 共用跨角色 ticket 队列，等待有界 2 s | `frameworks/connectivity/bluetooth` @ `43945bc1d`<br>`service/stacks/zephyr/sal_le_scan_interface.c`<br>blob `24d8275d5d8b` |
| `vela_override/bluetooth/sal_gatt_client_interface.c` | `keep_db`（断开保留 GATT 缓存）、缓冲溢出不拆链 | keep_db / 溢出不拆链 | `frameworks/connectivity/bluetooth` @ `43945bc1d`<br>`service/stacks/zephyr/sal_gatt_client_interface.c`<br>blob `a5bc28c7ed71` |
| `vela_override/bluetooth/scan_manager.c` | LOCAL 配置下不编译 `bt_socket.h` | LOCAL 下不编 bt_socket.h | `frameworks/connectivity/bluetooth` @ `43945bc1d`<br>`service/src/scan_manager.c`<br>blob `bf4ef311849d` |

**文件系统 → `lib fs`**

| 文件 | 做了什么 | 为什么 | 上游（仓库 @ HEAD / 路径 / blob） |
|---|---|---|---|
| `vela_override/fs/fs_procfs_mount.c` | `df -h` 的容量字段改用 uint64 运算 | df -h 用 uint64，避免 >=4GiB wrap | `nuttx` @ `2ce740a0a`<br>`fs/mount/fs_procfs_mount.c`<br>blob `86004c0e59e5` |
| `vela_override/fs/fs_fat32util.c` | `FatSz16==0` 也按 FAT32 解析 | FatSz16==0 当 FAT32（大簇 1GiB 卷） | `nuttx` @ `2ce740a0a`<br>`fs/fat/fs_fat32util.c`<br>blob `c03df449c07e` |

**驱动 → `lib drivers`**

| 文件 | 做了什么 | 为什么 | 上游（仓库 @ HEAD / 路径 / blob） |
|---|---|---|---|
| `vela_override/mtd/ftl.c` | `O_DIRECT` 写成功时返回写了几个扇区（不再返回 leftover 0） | O_DIRECT 写成功返回扇区数（不是 leftover 0） | `nuttx` @ `2ce740a0a`<br>`drivers/mtd/ftl.c`<br>blob `89130f2c1894` |
| `vela_override/usbdev/mtp.c` | `iProduct` / `iSerialNumber` 变成可写（串号附 MAC 后缀） | iProduct / iSerialNumber 可写（MAC 后缀） | `nuttx` @ `2ce740a0a`<br>`drivers/usbdev/mtp.c`<br>blob `b97b5ec3e9cb` |

**字体 → `lib freetype`**

| 文件 | 做了什么 | 为什么 | 上游（仓库 @ HEAD / 路径 / blob） |
|---|---|---|---|
| `vela_override/freetype/ftsystem.c` | FreeType 的堆改从 PSRAM 分配 | FT heap 走 PSRAM | `external/freetype/freetype` @ `b9d36e18c`<br>`src/base/ftsystem.c`<br>blob `9beb7e245d2f` |

**调度器 → `lib sched`**

| 文件 | 做了什么 | 为什么 | 上游（仓库 @ HEAD / 路径 / blob） |
|---|---|---|---|
| `vela_override/sched/wd_start.c` | PSRAM 上的 waitdog 视为合法；只在链真断了才剪环，并打印现场（why/prev/next/原始字） | PSRAM waitdog 视为合法；断链才剪环，避免 wd_insert HardFault | `nuttx` @ `2ce740a0a`<br>`sched/wdog/wd_start.c`<br>blob `f9d8340186a9` |
| `vela_override/sched/wd_cancel.c` | 未入链的节点不做 `list_delete`，并把异常记进 wdog 日志 | 未入链不 list_delete | `nuttx` @ `2ce740a0a`<br>`sched/wdog/wd_cancel.c`<br>blob `5056f617b123` |
| `vela_override/sched/sem_waitirq.c` | `waitobj==NULL` 记 schedmon 快照，不再 `DEBUGASSERT` | waitobj==NULL 记 schedmon，不 DEBUGASSERT | `nuttx` @ `2ce740a0a`<br>`sched/semaphore/sem_waitirq.c`<br>blob `70552158c26c` |
| `vela_override/sched/sem_post.c` | mutex holder≠tid 记 schedmon 快照，不再断言 | mutex holder≠tid 记 schedmon，不 DEBUGASSERT | `nuttx` @ `2ce740a0a`<br>`sched/semaphore/sem_post.c`<br>blob `491012b9b144` |

**zblue → `lib zblue`（前三个同名替换，后三个构建期补丁）**

| 文件 | 做了什么 | 为什么 | 上游（仓库 @ HEAD / 路径 / blob） |
|---|---|---|---|
| `vela_override/zblue/init.c` | `z_sys_init` 做成幂等（重复调用直接返回） | z_sys_init 幂等（SAL BREDR+LE 都可能调用） | `external/zblue/zblue` @ `6f79fb2a0`<br>`port/kernel/init.c`<br>blob `f17a4edb88fa` |
| `vela_override/zblue/system_work_q.c` | 第二条 sysworkq 不再复用同一块 BSS 栈 | 禁止第二条 sysworkq 共用 BSS 栈 | `external/zblue/zblue` @ `6f79fb2a0`<br>`kernel/system_work_q.c`<br>blob `5cc26fd82e14` |
| `vela_override/zblue/long_wq.c` | 第二条 BT LW WQ 不再复用同一块 BSS 栈 | 禁止第二条 BT LW WQ 共用 BSS 栈 | `external/zblue/zblue` @ `6f79fb2a0`<br>`subsys/bluetooth/host/long_wq.c`<br>blob `b7b6e3dc03b1` |
| `vela_override/zblue/patch_hci_core.py` | 命令超时返回 `-ETIMEDOUT`（不改断言）；`skip_sync` 的轮询退出不再走不可中断等待 | 超时返回 -ETIMEDOUT；skip_sync 轮询退出不可中断等待 | `external/zblue/zblue` @ `6f79fb2a0`<br>`subsys/bluetooth/host/hci_core.c`<br>blob `ab23d63b2da4` |
| `vela_override/zblue/patch_gatt_ccc_pool.py` | 断开时释放 CCC 槽、池满时回收已断开的 peer；CCC 池加金丝雀 + 30 s 校验 | 断开释放 CCC 槽 + 池满时回收已断开的 peer；CCC 池金丝雀 + 30 s 校验 | `external/zblue/zblue` @ `6f79fb2a0`<br>`subsys/bluetooth/host/gatt.c`<br>blob `8e31d1cfa353` |
| `vela_override/zblue/patch_net_buf_timeout.py` | `net_buf` 的 `K_FOREVER` 申请改成有上限的轮询（拿不到就丢包继续） | 缓冲区 K_FOREVER 改成有上限的轮询（否则卡死不可杀） | `external/zblue/zblue` @ `6f79fb2a0`<br>`subsys/bluetooth/host/buf.c`<br>blob `b7ed19303be2`<br>`external/zblue/zblue` @ `6f79fb2a0`<br>`subsys/bluetooth/host/hci_core.c`<br>blob `ab23d63b2da4` |

**非抽换层（不是抽换件，只是不进本目录）**

| 东西 | 做了什么 | 为什么 | 上游（仓库 @ HEAD / 路径 / blob） |
|---|---|---|---|
| 官方 `apps/graphics/lvgl/lvgl/` 整树（9.1→9.5） | 用工作区里的 `vendor/my_vendor/apps/graphics/lvgl/` 顶替，官方树不动 | 整树版本替换不是功能补丁；且 `CONFIG_MYVENDOR_LVGL_STACK` 会与官方 `lvgl` target 撞车（见 [lvgl.md](lvgl.md)） | `apps` @ `017abdbef`<br>`graphics/lvgl/lvgl/`（子树）<br>tree `` |
| nist-sts 未跟踪文件 | 不处理 | 与本板无关，不进镜像 | 与本板无对应上游，无需 id |
