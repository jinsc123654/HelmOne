# NuttX 启动顺序与隐性资源注册（my_vendor / SF32LB52）

本文面向在 `vendor/my_vendor` 上日常开发的工程师：说明 **上电后代码跳转顺序**，并重点展开那些 **不会在业务代码里显式 `register()`、却在 CMake/Kconfig/ROMFS 里“隐性登记”** 的脚本与资源——弄不清它们，容易出现「改了 rcS 不生效」「命令找不到」「编译报 rc.sysinit missing」等问题。

**默认参考配置**：`boards/sf32lb52/my_vendor/configs/nsh/defconfig`  
（`CONFIG_INIT_ENTRYPOINT="nsh_main"`、`CONFIG_ETC_ROMFS=y`、`CONFIG_BOARD_LATE_INITIALIZE=y`）  
**`test_app` 自启**：在 `sf32lb52_devkit_lcd_bringup()` 末尾 **`task_spawn`**（非 rcS）；详见 [test_app_guide.md](test_app_guide.md) **§4 方式 D**。

---

## 目录

1. [阅读指引：显式 vs 隐性注册](#1-阅读指引显式-vs-隐性注册)
2. [启动总览（时间线）](#2-启动总览时间线)
3. [隐性资源注册全集](#3-隐性资源注册全集)（**重点**）
   - 3.1 [板级 ROMFS `/etc`](#31-板级-romfs-etc)
   - 3.2 [`RCSRCS` 与 `RCRAWS` 的本质区别](#32-rcsrcs-与-rcraws-的本质区别)
   - 3.3 [启动脚本：`rc.sysinit` / `rcS`](#33-启动脚本rcsysinit--rcs)（含 [两脚本之间](#rcsysinit-与-rcs-之间运行了什么重点) 步骤）
   - 3.4 [可选：`login` 脚本与 `group`](#34-可选login-脚本与-group)
   - 3.5 [Builtin 命令表（应用“注册”）](#35-builtin-命令表应用注册)
   - 3.6 [首任务与环境变量](#36-首任务与环境变量)
   - 3.7 [Kconfig `select` 带来的连带开关](#37-kconfig-select-带来的连带开关)
   - 3.8 [board bringup 内拉起应用（test_app）](#38-board-bringup-内拉起应用test_app)
4. [Shell 与脚本的执行关系](#4-shell-与脚本的执行关系)
5. [启动阶段详解](#5-启动阶段详解)
   - 5.1 [Bootloader 与 `__start`](#51-bootloader-与-__start)
   - 5.2 [`nx_start` / `hardware_initialize`](#52-nx_start--hardware_initialize)
   - 5.3 [`nx_bringup` / `nx_start_application`](#53-nx_bringup--nx_start_application)
   - 5.4 [`board_late` 驱动注册](#54-board_late-驱动注册)
6. [应用如何编入与拉起](#6-应用如何编入与拉起)
7. [开发踩坑与排查清单](#7-开发踩坑与排查清单)
8. [Kconfig 与源码索引](#8-kconfig-与源码索引)
9. [相关文档](#9-相关文档)

---

## 1. 阅读指引：显式 vs 隐性注册

| 类型 | 你能直接看到的 | 典型例子 |
|------|----------------|----------|
| **显式注册** | C 代码里调用 `*_register()`、`nx_mount()` | `i2c_register`、`pwm_register`（`sifli_ap.c`） |
| **隐性注册** | CMake / Kconfig / 链接阶段写入镜像，运行期“自动出现” | `/etc` ROMFS、`rcS`、builtin 表、`defconfig` 打开的 `hello` |

开发业务时，**驱动**多在 `board_late` 显式完成；**开机跑什么命令、NSH 能敲哪些字** 则大量依赖隐性层。下文第 3 节是防踩坑核心。

---

## 2. 启动总览（时间线）

```
[Bootloader SFBL]     加载镜像、Flash/NAND、打印 SFBL
        │
        ▼
[__start]             MPU/Cache、BSS/DATA、HAL、早期 UART（仍单线程）
        │
        ▼
[nx_start]            调度器、内存、up_initialize()、board_early（my_vendor 为空）
        │
        ▼
[nx_bringup]          work queue、创建 AppBringUp 线程
        │
        ▼
[nx_start_application]
        ├─ nx_romfsetc()              ← 隐性：挂载编译进固件的 /etc
        ├─ board_late_initialize()    ← 显式：/dev、FS、LCD 任务等
        │     └─ bringup 末尾 task_spawn(test_app)  ← 方式 D，见 §3.8
        └─ task_spawn(nsh_main)       ← 隐性：由 CONFIG_INIT_ENTRYPOINT 决定
        │
        ▼
[nsh_main]
        ├─ nsh_initialize()             ← §3.3：rc.sysinit → netinit 等 → rcS
        │     ├─ /etc/init.d/rc.sysinit
        │     ├─ netinit_bringup 等（非 rc 文件，常未开）
        │     └─ /etc/init.d/rcS
        └─ nsh_consolemain()            ← 交互 Shell 循环
```

| 阶段 | 上下文 | my_vendor 主要文件 |
|------|--------|-------------------|
| Bootloader | 裸机 | `boot_loader/project/butterflmicro/board/` |
| `__start` | 关中断 | `chips/sf32lb52/sifli_start.c` |
| `board_late` | AppBringUp 内核线程 | `boards/.../bsp/sifli_ap.c`（含 `board_start_test_app_async`） |
| NSH + 脚本 | 用户任务 | `apps/system/nsh/nsh_main.c`、`bsp/etc/init.d/*` |

---

## 3. 隐性资源注册全集

本节列出 **“你没写 register，但系统认为已经登记好的东西”**：从哪里来、何时生效、怎么改、改错会怎样。

### 3.1 板级 ROMFS `/etc`

#### 登记位置（编译期）

`boards/sf32lb52/my_vendor/bsp/CMakeLists.txt`：

```cmake
nuttx_add_romfs(
  NAME etc
  MOUNTPOINT etc          # 逻辑卷名，生成 romfs_etc 目标
  RCSRCS etc/init.d/rcS etc/init.d/rc.sysinit
  RCRAWS etc/group etc/1.txt
  PATH ${CMAKE_CURRENT_LIST_DIR}/etc)

target_link_libraries(board PRIVATE romfs_etc)
```

| 字段 | 含义 |
|------|------|
| `NAME etc` | 生成 C 数组 `romfs_etc.c` / 链接目标 `romfs_etc` |
| `PATH .../etc` | 整目录复制进镜像的根（与 `RC*` 条目合并） |
| `MOUNTPOINT etc` | 与 Kconfig `CONFIG_ETC_ROMFSMOUNTPT`（默认 `/etc`）配合使用 |

#### 运行期挂载（自动，无需你写 mount 代码）

`nuttx/sched/init/nx_bringup.c` → `nx_romfsetc()`（需 `CONFIG_ETC_ROMFS=y`）：

1. `romdisk_register()`：把 `romfs_img[]` 注册为块设备（如 `/dev/ram0`）
2. `nx_mount(..., CONFIG_ETC_ROMFSMOUNTPT, "romfs", ...)`：挂到 **`/etc`**

此后脚本路径 **`/etc/init.d/rcS`** 才存在。若关闭 `CONFIG_ETC_ROMFS`，则 **不会** 执行 rc 脚本（除非另配 `CONFIG_NSH_ROMFSRC` 等）。

#### my_vendor 当前 `/etc` 内容

| 镜像内路径 | 源文件 | 登记方式 | 当前状态 |
|------------|--------|----------|----------|
| `/etc/init.d/rcS` | `bsp/etc/init.d/rcS` | `RCSRCS` | 示例：`echo rcS start` |
| `/etc/init.d/rc.sysinit` | `bsp/etc/init.d/rc.sysinit` | `RCSRCS` | 示例：`echo rc.sysinit start` |
| `/etc/group` | `bsp/etc/group` | `RCRAWS` | `root:*:0:root,admin` |
| `/etc/1.txt` | `bsp/etc/1.txt` | `RCRAWS` | 示例文本 |

验证：启动后在 NSH 执行 `ls /etc`、`ls /etc/init.d`。

---

### 3.2 `RCSRCS` 与 `RCRAWS` 的本质区别

这是 **最容易踩坑** 的一点：二者都进 ROMFS，但构建处理 **完全不同**。

| | **`RCSRCS`** | **`RCRAWS`** |
|--|--------------|--------------|
| **构建处理** | 经 **C 预处理器**（`cpp`）生成后再打包 | **原样拷贝**（`configure_file COPYONLY`） |
| **典型用途** | 启动脚本：可按 `CONFIG_*` 条件编译内容 | `group`、证书、二进制、任意文本 |
| **风险** | 以 `#` 开头的行可能被当成 **cpp 指令** 删掉或报错 | 路径必须在 CMake 列表中 **显式列出**，否则不进镜像 |
| **改后生效** | 改源文件 → 重编固件 | 同左 |

实现见 `nuttx/cmake/nuttx_add_romfs.cmake`：`RCSRCS` 走 `nuttx_generate_preprocess_target`，`RCRAWS` 走 `COPYONLY`。

**实践建议**：

- `rcS` / `rc.sysinit` 若要用 `#ifdef CONFIG_FS_FAT` 等按配置裁剪内容 → 放 **`RCSRCS`**（与 NuttX 上游 template 一致）。
- 若只是 `#` 注释、不想被 cpp 干扰 → 用 **`RCRAWS`** 登记同名路径，或注释改用 `rem` 等非 cpp 行首形式（不推荐，易混）。
- 新增 `etc/my.conf` → 必须加到 **`RCRAWS`**（或 `RCSRCS` 并理解 cpp 规则）。

**编译失败典型报错**：`rc.sysinit missing` / `ninja: no rule` → CMake 里写了 `RCSRCS etc/init.d/rc.sysinit`，但 **源文件不存在**（需创建空文件或从 `vendor/sifli` 复制 `init.d/`）。

---

### 3.3 启动脚本：`rc.sysinit` / `rcS`

#### 配置项（Kconfig → 运行路径）

| Kconfig | 默认值 | 运行时完整路径 |
|---------|--------|----------------|
| `CONFIG_ETC_ROMFSMOUNTPT` | `/etc` | 挂载点 |
| `CONFIG_NSH_SYSINITSCRIPT` | `init.d/rc.sysinit` | `/etc/init.d/rc.sysinit` |
| `CONFIG_NSH_INITSCRIPT` | `init.d/rcS` | `/etc/init.d/rcS` |

宏定义（`apps/nshlib/nsh.h`）：

```c
#define NSH_SYSINITPATH CONFIG_ETC_ROMFSMOUNTPT "/" CONFIG_NSH_SYSINITSCRIPT
#define NSH_INITPATH    CONFIG_ETC_ROMFSMOUNTPT "/" CONFIG_NSH_INITSCRIPT
```

#### 谁执行、执行几次

`apps/nshlib/nsh_init.c` → `nsh_initialize()`，由 `nsh_main` 在进交互循环 **之前** 调用一次。  
脚本执行依赖：`CONFIG_ETC_ROMFS=y` 且 **未** 设置 `CONFIG_NSH_DISABLESCRIPT`。

| 顺序 | 函数 | 是否脚本文件 | 次数 | 失败是否打日志 |
|------|------|--------------|------|----------------|
| — | `nsh_sysinitscript()` | **rc.sysinit** | 每次 `nsh_initialize` | **否**（缺失静默） |
| — | `nsh_initscript()` | **rcS** | **全系统仅一次** | **是** |

实现：`apps/nshlib/nsh_script.c` → `nsh_script()` 按行读文件，当作 **NSH 命令** 执行（不是 bash）。

#### rc.sysinit 与 rcS 之间运行了什么（重点）

两脚本 **不是** 紧挨着执行；中间还有 NSH 库里的 **固定 C 逻辑**（与 rc 文件无关）。  
完整顺序以源码 `apps/nshlib/nsh_init.c` 为准：

```
nsh_main()
  ├─ sched_setparam()                    # 调到 CONFIG_SYSTEM_NSH_PRIORITY
  └─ nsh_initialize()
        ├─ [A] nsh_update_prompt()       # 生成 nsh> 提示符字符串
        ├─ [B] readline_prompt / Tab 补全 # CONFIG_NSH_READLINE（my_vendor: 开）
        ├─ [C] usbtrace_enable()         # CONFIG_NSH_USBDEV_TRACE（my_vendor: 关）
        ├─ [D] boardctl(BOARDIOC_APP_SYMTAB) # CONFIG_NSH_SYMTAB（按需）
        ├─ [E] boardctl(BOARDIOC_INIT)   # CONFIG_NSH_ARCHINIT → board_app_initialize
        │                                 # my_vendor: 未开；板级 init 已在 board_late 做完
        ├─ [F] nsh_newconsole(false)     # 为执行脚本创建临时 console 状态
        ├─ [G] nsh_sysinitscript()       ★ 执行 /etc/init.d/rc.sysinit
        ├─ [H] netinit_bringup()         # CONFIG_NSH_NETINIT：起网卡/协议栈
        │                                 # my_vendor nsh defconfig: 通常未开 → 此步跳过
        ├─ [I] boardctl(BOARDIOC_FINALINIT) # 需 ARCHINIT + BOARDCTL_FINALINIT
        │                                 # my_vendor: 未开
        ├─ [J] nsh_initscript()          ★ 执行 /etc/init.d/rcS（仅首次有效）
        ├─ [K] nsh_release()             # 释放 [F] 的临时 console
        └─ [L] nsh_telnetstart()         # CONFIG_NSH_TELNET + 非 NETLOCAL
                                          # my_vendor: 一般未开
  └─ nsh_consolemain()                   # 交互 Shell 循环，出现 nsh>
```

**夹在 rc.sysinit（[G]）与 rcS（[J]）之间的只有：**

| 步骤 | 代码 | my_vendor 典型状态 | 对开发的意义 |
|------|------|-------------------|--------------|
| **[H] 网络初始化** | `netinit_bringup()` | **未启用** `CONFIG_NSH_NETINIT` → **不执行** | 若要用 `rcS` 起需要 TCP/IP 的服务，需先在 menuconfig 开网络 + `NSH_NETINIT`，或自己在 `rc.sysinit`/`rcS` 里 `ifup` 等 |
| **[I] 板级收尾** | `boardctl(BOARDIOC_FINALINIT)` | **未启用** | 与 [E] 成对；开了 `NSH_ARCHINIT` 时才会在 rcS 前再调一次 `board_app_finalinitialize` |

因此在你当前的 **my_vendor nsh** 配置上，串口上 **`echo rc.sysinit start` 与 `echo rcS start` 之间通常没有额外可见输出**——中间步骤 [H][I] 被 Kconfig 裁掉；仍可能有 **其它任务**（如 `lcd_async_init`）的 syslog 插在两行 echo 之间，那是 **并行线程** 的输出，不是「第三个 rc 脚本」。

**在 rc.sysinit 之前（[A]–[E]）** 也还没有执行任何 rc 文件，只是配置 Shell；**[E] `BOARDIOC_INIT`** 若打开会再跑一遍 `board_app_initialize()`，与 `board_late` 重复，故 my_vendor 保持关闭。

**在 rcS 之后（[K][L]）** 才进入 `nsh_consolemain()`；**不会**再自动执行 `rc.sysinit`/`rcS`（除非复位或再次调用 `nsh_initialize`，后者正常启动路径不会）。

**分工建议（写脚本时）：**

| 放哪里 | 适合内容 |
|--------|----------|
| **rc.sysinit** | 仅依赖 `/etc`、希望 **最早** 的 NSH 命令：早期 `mount`、环境变量（若 NSH 支持）、为 [H] 网络准备前的本地配置 |
| **rcS** | 依赖 `board_late` 已注册的 `/dev/*`、NAND 已挂载后的自启：**`myapp &`** 等（**当前 test_app 不在 rcS**，见 §3.8） |
| **中间 [H][I]** | 不要往 rc 里塞；用 **menuconfig 打开对应 CONFIG**，由 NSH 库自动调用 |

若要用 `echo` 验证中间是否有网络初始化，可在 `rc.sysinit` 末尾加一行占位，在 `rcS` 开头再加一行，例如：

```text
echo rc.sysinit end
```

```text
echo rcS start
```

则串口顺序为：`rc.sysinit start` → `rc.sysinit end` →（此处仅 [H][I] 等 C 代码，my_vendor 常无输出）→ `rcS start` → `nsh>`。

#### 脚本里能写什么

- NSH 支持的命令：`mount`、`hello`、`test nand`、已注册的 builtin 名等。
- 行尾 **`&`**：后台任务，不阻塞进入 `nsh>`。
- 无 **`&`**：前台；程序不退出则 **一直占住**，看不到提示符。
- **不是** Linux bash：无完整 bash 管道/函数；以 NSH 实际支持的命令为准。

#### 与 `board_late` 的先后

| 时刻 | 已完成 | 脚本里可假设 |
|------|--------|--------------|
| `rc.sysinit` / `rcS` 执行时 | `board_late` **已结束**（在 `nsh_main` 之前） | `/dev/i2c0`、NAND 挂载等（若 defconfig 打开） |
| | LCD 可能仍在 **异步任务** `lcd_async_init` 中 | 访问 LCD 可能需延时或轮询就绪 |

#### 示例：`echo` 验证执行顺序（my_vendor 默认已写入）

仓库里已用最小示例标出 **谁先谁后**（改完后需重编烧录）：

**`boards/sf32lb52/my_vendor/bsp/etc/init.d/rc.sysinit`**

```text
echo rc.sysinit start
```

**`boards/sf32lb52/my_vendor/bsp/etc/init.d/rcS`**

```text
echo rcS start
```

说明：

- `echo` 是 NSH 内置命令（`cmd_echo`），**不是** bash；在 rc 脚本里可直接写。
- 两行都会在 **`nsh>` 提示符出现之前** 打印到控制台（与 `printf` 走同一输出）。
- 预期串口顺序（`board_late`、**test_app** `launch=board`、LCD 异步等 syslog 可能插在任意时刻；**两脚本之间**见上节 [H][I]）：

```text
INFO: test_app async started pid=...
test_app: started ... launch=board
rc.sysinit start
rcS start
nsh>
```

据此可确认：**执行顺序** 为 `board_late`（含可选 §3.8 test_app）→ `nsh_main` → `rc.sysinit` →（`netinit_bringup` 等，my_vendor 常跳过）→ `rcS` → `nsh_consolemain`。若 `rc.sysinit` 里访问 `/dev/i2c0` 失败，应查驱动而非「中间还有隐藏 rc 文件」。

**在板子上核对镜像内容**（启动后手动敲）：

```text
cat /etc/init.d/rc.sysinit
cat /etc/init.d/rcS
```

**扩展示例**（在 `rcS` 末尾追加，勿在行首用 `#` 注释——`RCSRCS` 会走 C 预处理器，见 3.2 节）：

```text
echo rcS start
hello &
```

`hello &` 会在打印 `rcS start` 后后台启动例程，仍会出现 `nsh>`。

**恢复为“无自启”**：把两个文件清空或只留空行，重新编译即可。

---

### 3.8 board bringup 内拉起应用（test_app）

当前 my_vendor **默认** 不在 `rcS` 里写 `test_app rcs &`，而在 **`sf32lb52_devkit_lcd_bringup()` 返回前** 调用 `board_start_test_app_async()`（`sifli_ap.c`），与 `lcd_async_init` 同类。

| 项目 | 说明 |
|------|------|
| 时机 | `board_late_initialize()` 内，**早于** `task_spawn(nsh_main)` |
| 调度 | 多任务 **已就绪**（`OSINIT_OSREADY` 之后）；bringup 跑在 AppBringUp 线程 |
| 机制 | `task_spawn("test_app", test_app_main, ...)`，**明文**入口，**不**走 `nsh_builtin` |
| 仍须编入 | `nuttx_add_application` + `CONFIG_MYVENDOR_TEST_APP`（链出 `test_app_main` 符号） |
| 日志 | `launch=board`；`argv` 只传 `{ "board", NULL }`（勿把 `"test_app"` 再放进 argv，见 [test_app_guide §4.4](test_app_guide.md#44-task_spawn-与-argv重要)） |
| 与 rcS | **二选一**；同时启用会起 **两个** test_app |

**与 §3.3 rcS 自启的对比**

| | **§3.8 board** | **§3.3 rcS** |
|--|----------------|--------------|
| 改哪里 | `sifli_ap.c` | `bsp/etc/init.d/rcS` |
| 何时跑 | bringup 末尾，NSH 之前/并行 | `nsh_initialize` 里解析 rcS |
| `INIT_ENTRYPOINT` | 保持 `nsh_main` | 保持 `nsh_main` |

完整步骤、优劣、改回 rcS 的方法见 **[test_app_guide.md](test_app_guide.md)**（方式 D / A / B / C）。

---

### 3.4 可选：`login` 脚本与 `group`

#### `/etc/group`（`RCRAWS`）

源：`boards/.../bsp/etc/group` → 镜像 `/etc/group`。

- 格式与 Unix `group(5)` 类似；NSH 部分功能（如 `ps` 显示、权限相关）可能读取。
- my_vendor 当前仅一行占位，**未启用登录认证**时影响很小。
- 修改后需仍在 `RCRAWS` 列表中。

#### Login 脚本（my_vendor **默认未启用**）

需同时满足：

- `CONFIG_NSH_ROMFSRC=y`
- `CONFIG_NSH_RCSCRIPT`（默认 `.nshrc`）→ 路径 `/etc/.nshrc`

在 **每个 NSH 会话** 启动时由 `nsh_loginscript()` 执行（`nsh_session.c`），与 **只跑一次的 `rcS`** 不同。

Telnet 登录、`passwd` 等另需 `CONFIG_NSH_TELNET_LOGIN` 等，当前 defconfig 未走这条路径。

---

### 3.5 Builtin 命令表（应用“注册”）

#### 登记方式（链接期自动生成）

1. 某处 `nuttx_add_application(NAME hello ...)`（`apps/examples` 或板级 `test/`）
2. `defconfig` 中 `CONFIG_EXAMPLES_HELLO=y` / `CONFIG_MYVENDOR_SELFTEST=y` 等
3. 构建最后 `apps/builtin/CMakeLists.txt` 汇总 `NUTTX_APPS_LIBRARIES` → 生成 **`builtin_list.h`**

条目形如：

```c
{ "hello", priority, stack, hello_main },
```

条件：`CONFIG_BUILTIN=y` + `CONFIG_NSH_BUILTIN_APPS=y`（my_vendor 已开）。

#### 调用方式

用户在 Shell 输入命令名 → `nsh_builtin()` 查表 → `task_spawn` 执行 `*_main`。

| 现象 | 原因 |
|------|------|
| `hello: command not found` | defconfig 未开 `CONFIG_EXAMPLES_HELLO` 或未重编 |
| 有 `test` 无 POSIX `test` | `CONFIG_MYVENDOR_SELFTEST` **select** `NSH_DISABLE_TEST`（见 3.7） |
| `rcS` 里写 `hello` 报错 | 命令未编入 builtin，或拼写与 `APP_NAME` 不一致 |

也可在 **`rcS`** 里写 `myapp &` 做隐性自启，或在 **bringup** 里 `task_spawn`（§3.8，**当前 `test_app` 默认**）。

#### my_vendor 板级 builtin 示例

| 命令 | 登记文件 | Kconfig | 开机自启（当前） |
|------|----------|---------|------------------|
| `test_app` | `boards/.../nsh/test_app/CMakeLists.txt` | `CONFIG_MYVENDOR_TEST_APP` | **§3.8 bringup**，非 rcS |
| `test` | `boards/.../nsh/test/CMakeLists.txt` | `CONFIG_MYVENDOR_SELFTEST` | 仅手敲 / rcS |
| `md5_test` | `boards/.../nsh/CMakeLists.txt` | `CONFIG_CRYPTO` | 仅手敲 / rcS |

---

### 3.6 首任务与环境变量

#### 第一个用户任务（隐性：仅一个 Kconfig 字符串）

| 配置 | my_vendor 值 | 效果 |
|------|--------------|------|
| `CONFIG_INIT_ENTRY` | y | 使用入口符号 spawn |
| `CONFIG_INIT_ENTRYPOINT` | `"nsh_main"` | 链接 `nsh_main` 作为首任务 |
| `CONFIG_INIT_PRIORITY` / `CONFIG_INIT_STACKSIZE` | 100 / 16096 | spawn 属性 |

改产品行为：

- **保留 Shell + 自启**：保持 `nsh_main`，改 `rcS`。
- **不要 Shell**：`INIT_ENTRYPOINT=myapp_main`，并视情况 `# CONFIG_SYSTEM_NSH is not set`。

#### `nx_bringup` 里设置的环境（隐性）

若开启 `CONFIG_PATH_INITIAL` 等，在 `nx_bringup()` 里 `setenv("PATH", ...)`，子进程继承。my_vendor 若未配置则 PATH 可能为空或默认，影响 `exec` 外部程序路径搜索。

---

### 3.7 Kconfig `select` 带来的连带开关

打开一个选项时，**自动打开/关闭** 其它选项，容易“不知道为什么行为变了”。

| 你打开的选项 | 连带效果 | 开发影响 |
|--------------|----------|----------|
| `CONFIG_MYVENDOR_SELFTEST` | `select NSH_DISABLE_TEST` | NSH 内置 POSIX **`test`** 被禁用；板级 **`test nand`** 占用 `test` 名 |
| `CONFIG_ETC_ROMFS` | 依赖 ROMFS、挂载 `/etc` | 启用 rc 脚本机制 |
| `CONFIG_NSH_LIBRARY` | 可能 `select BOARDCTL` 等 | 与 `boardctl`、mkrd 相关 |

改 Kconfig 后务必 **`savedefconfig`** 并全量重编，避免 `.config` 与 `defconfig` 不一致。

---

### 3.8 其它隐性登记入口（扩展用）

| 机制 | API / 位置 | 用途 |
|------|------------|------|
| `add_board_rcsrcs()` / `add_board_rcraws()` | `nuttx_add_romfs.cmake` | 其它模块向 **board** 目标追加 ROMFS 文件，合并进 `romfs_etc` |
| `add_dynamic_rcsrcs()` | 同上 | 构建过程中 **生成** 的脚本再打进 ROMFS |
| `nuttx_add_application` on **board** | `bsp/CMakeLists.txt`、`test/` | 不放在 `apps/` 也能进 builtin 表 |

---

## 4. Shell 与脚本的执行关系

| | **Shell（NSH）** | **`rcS` / `rc.sysinit`** |
|--|------------------|-------------------------|
| 本质 | 常驻任务 `nsh_main` + `nsh_consolemain()` 循环 | ROMFS 中的 **文本命令列表** |
| 触发 | 上电 spawn 一次，然后一直交互 | `nsh_initialize()` 里 **自动执行** |
| 次数 | 循环直到复位 | `rcS` **全局仅一次**；`rc.sysinit` 每次 `nsh_initialize` |
| 用途 | 调试、手动敲命令 | 上电 `myapp &`、挂 FS、起服务 |

```
nsh_main
  └─ nsh_initialize()     ← rc.sysinit → netinit 等 → rcS（§3.3）
  └─ nsh_consolemain()    ← 你看到的 nsh>
```

**Shell 不是业务进程**：它解析命令并 spawn builtin；业务在 `hello_main`、`test_main` 或 `rcS` 拉起的任务里。

---

## 5. 启动阶段详解

### 5.1 Bootloader 与 `__start`

- **Bootloader**：`boot_flash.c`、`main.c` — 加载 NuttX、MPI/NAND/SD、跳转向量表。自定义 msh、KV 只读、烧录坑见 **[boot_2sfbl.md](boot_2sfbl.md)**。
- **`__start`**（`sifli_start.c`）：关中断 → VTOR → `mpu_config()` → BSS/DATA → `HAL_Init()` → `arm_earlyserialinit()` → `nx_start()`。  
  NAND 启动 **不在此** 提频 240MHz（在 `board_late` 存储成功之后的 `hclk240` 步；之后由 [dvfs.md](dvfs.md) 在 72/96/144/240 之间切）。

### 5.2 `nx_start` / `hardware_initialize`

`nuttx/sched/init/nx_start.c`：

- `up_initialize()`：`arm_serialinit()` 等 → **`/dev/console`**
- `board_early_initialize()`：my_vendor **空实现**；**禁止**在此 `mount`/阻塞等待

### 5.3 `nx_bringup` / `nx_start_application`

`nx_bringup.c`：work queue → **AppBringUp** 线程 → `nx_start_application()`：

1. `nx_romfsetc()` — **隐性挂载 `/etc`**
2. `board_late_initialize()` — 驱动与 FS（见下节）；末尾可 **spawn 业务 app**（§3.8）
3. `task_spawn(nsh_main)` — **隐性首应用**

### 5.4 `board_late` 驱动注册

`board_late_initialize()` → `sf32lb52_devkit_lcd_bringup()`（`sifli_ap.c`）。

**显式注册**设备节点，与第 3 节 **隐性脚本** 互补：

| 类别 | 示例 API | 访问路径 |
|------|----------|----------|
| 字符设备 | `i2c_register`、`pwm_register` | `/dev/i2c0`、`/dev/pwm0` |
| 块/MTD | `sf32lb_nand_automount` | 挂载点由 chip 定义 |
| 输入 | `ft6146_touch_initialize` | input 子系统 |
| 异步任务 | `task_create("lcd_async_init", ...)` | LCD 就绪可能晚于 `rcS` |
| 异步应用 | `task_spawn(test_app_main, ...)` | **§3.8**，`launch=board`，早于/并行 `nsh_main` |

`CONFIG_BOARD_LATE_INITIALIZE=y` 时，`board_app_initialize()` 直接返回 OK；**未**启用 `CONFIG_NSH_ARCHINIT`，NSH **不会**再调 `boardctl(BOARDIOC_INIT)` 做板级 init。

---

## 6. 应用如何编入与拉起

### 6.1 三种编入路径

| 路径 | 位置 | 登记机制 | 如何“跑起来” |
|------|------|----------|--------------|
| 上游例程 | `apps/examples/*` | `nuttx_add_application` + `CONFIG_EXAMPLES_*` | NSH 命令 / `rcS` |
| 板级命令 | `boards/.../nsh/test/`、`md5_test` | 同上 + 板级 CMake | `test nand`、`md5_test` |
| 板级 demo | `boards/.../nsh/test_app/` | 同上 + `MYVENDOR_TEST_APP` | **§3.8 bringup `task_spawn`**（当前默认）或 rcS / 手敲 |
| 产品主程序 | 自建目录 | `nuttx_add_application` | `rcS` 里 `myapp &`、**bringup spawn** 或改 `INIT_ENTRYPOINT` |

### 6.2 `nuttx_add_application` 隐性规则

- 生成目标 `apps_<NAME>`，`main` 重命名为 `<name>_main`（除非 `NO_MAIN_ALIAS`）。
- **不会**仅凭登记就开机运行；还须其一：**bringup 里 `task_spawn`**（§3.8）、**Shell 手动**、**rcS**、或 **改 `INIT_ENTRYPOINT`**。

### 6.3 开机拉起策略（my_vendor 相关）

| 策略 | 配置 / 代码 | Shell | test_app 示例 |
|------|-------------|-------|----------------|
| **默认** | `nsh_main` + `board_start_test_app_async()` | 有 | `launch=board`，见 [test_app_guide §4](test_app_guide.md#4-方式-dboard-bringup-异步当前默认) |
| rcS 自启 | `nsh_main` + `rcS`: `test_app rcs &` | 有 | `launch=rcs`；须关 §3.8 |
| 纯产品入口 | `INIT_ENTRYPOINT=test_app_main` | 可关 NSH | `launch=entry` |
| FS 启动 | `INIT_FILE` + `INIT_FILEPATH` | 视配置 | 视 mount |

---

## 7. 开发踩坑与排查清单

| 症状 | 优先检查 |
|------|----------|
| 编译 `rc.sysinit missing` | `bsp/etc/init.d/` 是否存在；`CMakeLists.txt` `RCSRCS` 路径是否一致 |
| 改了 `rcS` 没变化 | 是否重编烧录；是否改错目录（应改 **源** `bsp/etc/`，不是 `cmake_out`） |
| `rcS` 里命令 not found | builtin 是否 `CONFIG_*=y`；命令名是否与 `nuttx_add_application(NAME ...)` 一致 |
| `rcS` 有 `#` 注释导致怪异 | **RCSRCS 会走 cpp**；纯注释行慎用 `#`，或改 `RCRAWS` |
| 开机没有 `rc.sysinit start` / `rcS start` | 未烧新固件；`CONFIG_NSH_DISABLESCRIPT`；未开 `ETC_ROMFS`；或改了 `cmake_out` 而非 `bsp/etc` |
| 开机没有自动跑服务 | 未开 bringup spawn（§3.8）且 `rcS` 仅 echo；或 `CONFIG_NSH_DISABLESCRIPT` |
| test_app 打印 **Usage** | `task_spawn` argv 多写了 `"test_app"`；见 [test_app_guide §4.4](test_app_guide.md#44-task_spawn-与-argv重要) |
| test_app 跑两次 | §3.8 与 rcS `test_app rcs &` 同时启用 |
| 有 `nsh>` 但设备 open 失败 | 命令在 `board_late` **之前** 不应出现在 rc（实际 rc 在 late **之后**）；查 LCD 异步未就绪 |
| `test` 行为不对 | 是否开启 `MYVENDOR_SELFTEST`（占用 `test` 名并 disable POSIX test） |
| `hello` 找不到 | `CONFIG_EXAMPLES_HELLO` 与重编 |
| `/etc` 下无文件 | `CONFIG_ETC_ROMFS`；`romfs_etc` 是否链接进 board |
| 想加 `etc/foo.conf` | 加入 **`RCRAWS`** 并 `savedefconfig` 无关，但必须重编 |

**建议自检命令**（NSH 启动后）：

```text
ls /etc
ls /etc/init.d
cat /etc/init.d/rcS
help
```

---

## 8. Kconfig 与源码索引

### 8.1 与隐性资源强相关

| 配置项 | 作用 |
|--------|------|
| `CONFIG_ETC_ROMFS` | 编译并挂载 `/etc` |
| `CONFIG_ETC_ROMFSMOUNTPT` | 挂载点，默认 `/etc` |
| `CONFIG_NSH_INITSCRIPT` | `rcS` 相对路径 |
| `CONFIG_NSH_SYSINITSCRIPT` | `rc.sysinit` 相对路径 |
| `CONFIG_NSH_DISABLESCRIPT` | 为 y 则 **不执行** rc 脚本 |
| `CONFIG_NSH_NETINIT` | 为 y 时在 **rc.sysinit 与 rcS 之间** 调 `netinit_bringup()` |
| `CONFIG_NSH_ARCHINIT` | 为 y 时在 rc.sysinit **前** `BOARDIOC_INIT`、可与 `BOARDCTL_FINALINIT` 在 rcS **前** 再调板级钩子 |
| `CONFIG_INIT_ENTRYPOINT` | 首任务符号 |
| `CONFIG_NSH_BUILTIN_APPS` | 启用 builtin 表 |
| `CONFIG_MYVENDOR_SELFTEST` | 板级 `test` 命令 |
| `CONFIG_MYVENDOR_TEST_APP` | 编译 `test_app`；为 y 时 bringup 可 spawn（§3.8） |
| `CONFIG_NSH_DISABLE_TEST` | 关闭 NSH POSIX `test` |

### 8.2 关键源文件

| 路径 | 说明 |
|------|------|
| `boards/.../bsp/CMakeLists.txt` | ROMFS `/etc` |
| `boards/.../nsh/CMakeLists.txt` | 板级 NSH applet |
| `boards/.../bsp/etc/init.d/rcS` | 启动脚本源 |
| `boards/.../bsp/bringup.c` | `g_bringup[]` 开机表 |
| `boards/.../bsp/sifli_ap.c` | `board_late`、NuttX 板级钩子 |
| `boards/.../nsh/test_app/` | demo 应用源与 CMake |
| `chips/sf32lb52/sifli_start.c` | `__start` |
| `nuttx/sched/init/nx_bringup.c` | `/etc` mount、spawn init |
| `apps/nshlib/nsh_init.c` | 执行 rc 脚本 |
| `apps/nshlib/nsh_script.c` | 按行解析脚本 |
| `apps/builtin/CMakeLists.txt` | 生成 builtin 表 |
| `nuttx/cmake/nuttx_add_romfs.cmake` | RCSRCS/RCRAWS 规则 |

---

## 9. 相关文档

- **`test_app` 启动方式（含 board bringup §4）**：[test_app_guide.md](test_app_guide.md)  
- 板级 `/etc` 简要说明：[board_guide.md](board_guide.md) 第 5 节  
- 编译烧录：[build_guide.md](build_guide.md)  
- SD 分区与 `/mnt/kv` `/mnt/lfs`：[sd_partition.md](sd_partition.md)  
- 二级 boot（2SFBL msh / KV / sftool）：[boot_2sfbl.md](boot_2sfbl.md)
- 文件系统 A/B 固件槽：[fs_firmware.md](fs_firmware.md)  
- Bootloader / OVNX：`scripts/nuttx_ovnx_image.md`、`boot_loader/build.sh`  
- 串口与烧录工具：[tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)  
- HCPU 72/96/144/240 MHz 动态调频：[dvfs.md](dvfs.md)  

Bootloader 只负责到 **`__start` 之前**；**`/etc`、rc 脚本、builtin、NSH** 均在 NuttX 链路中由上文隐性机制完成。
