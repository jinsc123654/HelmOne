# SF32LB52 板卡制作指南

示例仓库：<https://gitee.com/jinsc123654/vela_sifli>（远端已存在，下面把它加入 OpenVela repo）

## 1. 配置 Git 凭据（HTTPS 需要）

**作用：** 让本机 `repo sync` 能访问 Gitee 私有仓库。

```bash
git config --global credential.helper store
```

验证（输入一次用户名 `jinsc123654` 和 Gitee **私人令牌**）：

```bash
git clone https://gitee.com/jinsc123654/vela_sifli.git
```

能 clone 成功，`repo sync` 就不再单独配账号。SSH 方式可跳过本节，manifest 改用 `fetch="git@gitee.com:jinsc123654"`。

---

## 2. 修改 repo manifest

**作用：** 告诉 repo 从 Gitee **拉取** `vela_sifli` 到本地 `vendor/my_vendor/`。

`local_manifests` **默认不存在**，需要你自己建。它是 repo 留给用户的可选目录，官方 manifest 不会自动生成。

```bash
# OpenVela 根目录（.repo 是隐藏目录，可用 ls -a .repo 查看）
mkdir -p .repo/local_manifests
```

新建 `.repo/local_manifests/my_vendor.xml`：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <remote name="my"
          fetch="https://gitee.com/jinsc123654" />

  <project path="vendor/my_vendor"
           name="vela_sifli"
           remote="my"
           revision="master" />
</manifest>
```

SSH 示例：`fetch="git@gitee.com:jinsc123654"`

### 配置项说明

| 配置 | 示例值 | 说明 |
|------|--------|------|
| `<?xml ...?>` | — | XML 声明，固定写法，不用改 |
| `<manifest>` | — | 根节点，包裹下面所有配置 |
| `<remote name="my">` | `my` | remote 的别名，下面 `<project remote="my">` 引用它；可改成任意名字（如 `gitee`） |
| `<remote fetch="...">` | `https://gitee.com/jinsc123654` | Git 服务器地址 + 用户名/组织名；**不含仓库名**。repo 会拼成 `fetch/name.git` |
| `<project path="...">` | `vendor/my_vendor` | 同步到 OpenVela 工程里的**本地目录**；与官方 `vendor/sifli` 同级 |
| `<project name="...">` | `vela_sifli` | Gitee 上的**仓库名**；完整 URL 为 `fetch/vela_sifli.git` |
| `<project remote="...">` | `my` | 指定从哪个 `<remote>` 拉代码；必须与 `<remote name="...">` **同名**。`my` 只是别名，可改成 `gitee` 等 |
| `<project revision="...">` | `master` | 拉取的分支或 tag；**须与 Gitee 远端实际分支一致**（Gitee 默认多为 `master`，不是 `main`） |

**`remote="my"` 是什么？**

`<remote>` 定义 Git 服务器地址，`<project remote="my">` 引用它，告诉 repo「去哪个服务器拉这个仓库」：

```xml
<remote name="my" fetch="https://gitee.com/jinsc123654" />   ← 定义：服务器叫 "my"
<project name="vela_sifli" remote="my" ... />              ← 引用：用 "my" 这个服务器
```

等价于手动执行：

```bash
git clone https://gitee.com/jinsc123654/vela_sifli.git vendor/my_vendor
```

只有一个 Gitee 时用一个 remote 即可；多个服务器时可定义多个 `<remote>`，各 project 用 `remote="..."` 分别指定。

**一个 xml 可配多个板卡**（多个 Git 仓库时，每个仓库一个 `<project>`；同一仓库多块板则共用一个 `<project>`，用不同子目录区分）：

```xml
<remote name="my" fetch="https://gitee.com/jinsc123654" />

<project path="vendor/my_vendor"  name="vela_sifli"  remote="my" revision="master" />
<project path="vendor/my_board2" name="vela_board2" remote="my" revision="master" />
```

也可拆成多个 xml 文件放在 `.repo/local_manifests/`，repo 会合并读取。

**拼出来的 clone 地址：**

```
https://gitee.com/jinsc123654/vela_sifli.git
  └─ fetch ─┘└─ 账号 ─┘  └─ name ─┘
```

**对应关系：**

```
.repo/local_manifests/my_vendor.xml   ← 文件名随意，repo 会读取该目录下所有 .xml
vendor/my_vendor/                     ← path（本地放哪）
gitee.com/jinsc123654/vela_sifli      ← fetch + name（从哪拉）
master                                ← revision（Gitee 默认分支）
```

**常见修改：**

- 换分支：先查远端分支 `git ls-remote --heads https://gitee.com/jinsc123654/vela_sifli.git`，再设 `revision="master"`
- 换本地目录：`path="vendor/vela_sifli"`（需与 defconfig 路径一致）
- 用 SSH：`fetch="git@gitee.com:jinsc123654"`，其余不变

---

## 3. repo 同步

**作用：** 从 Gitee 下载远端仓库到 `vendor/my_vendor/`。

```bash
# OpenVela 根目录
repo sync vendor/my_vendor
```

`repo sync` 整棵 OpenVela 后，NuttX / apps / zblue 会回到上游干净树。SD 启动在 vendor 里即可；**手机广播 + 扫传感器同时开**还要改 zblue `id.c`，**本板 LVGL 栈**还要改官方 `apps/graphics/lvgl` 构建脚本。改法和可覆盖文件见 [pitch/README.md](pitch/README.md)，原因见 [required_patches.md](required_patches.md)。

---

## 4. 编译（官方 build.sh / cmake）

在 **OpenVela 根目录**执行。路径**不要**加 `../` 前缀。

### 4.1 my_vendor 构建

**build.sh（推荐）：**

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ --cmake -j16
```

固件：`cmake_out/my_vendor_nsh/nuttx.bin`

**menuconfig / savedefconfig：**

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ --cmake menuconfig
./build.sh vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ --cmake savedefconfig
```

**清除重编：**

```bash
rm -rf cmake_out/my_vendor_nsh
rm -f nuttx/.config nuttx/.config.old nuttx/.version
./build.sh vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ --cmake -j16
```

**cmake 直接构建（CLion 同款）：**

```bash
cmake -B cmake_out/my_vendor_nsh -S nuttx -GNinja \
  -DBOARD_CONFIG=../vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ \
  -DCUSTOM_MODULE_PATH=$PWD/build/cmake \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"

cmake --build cmake_out/my_vendor_nsh -j16
```

### 4.2 官方 sifli 参考构建（对照用）

```bash
./build.sh vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh/ --cmake -j16
```

固件：`cmake_out/sf32lb52_devkit_lcd_nsh/nuttx.bin`

```bash
cmake -B cmake_out/sf32lb52_devkit_lcd_nsh -S nuttx -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh/ \
  -DCUSTOM_MODULE_PATH=$PWD/build/cmake \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"

cmake --build cmake_out/sf32lb52_devkit_lcd_nsh -j16
```

### 4.3 对比

| | my_vendor | 官方 sifli |
|--|-----------|------------|
| build.sh 路径 | `vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/` | `vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh/` |
| 输出目录 | `cmake_out/my_vendor_nsh/` | `cmake_out/sf32lb52_devkit_lcd_nsh/` |

> 可选：`python3 vendor/my_vendor/build_board.py build` 是对上述 build.sh 的封装，命令说明见 [tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)。

编译报错 `rc.sysinit missing` 见第 5 节；输出目录命名见第 6 节。

---

## 5. `bsp/etc/` 是什么

板级目录 `boards/.../bsp/etc/` 是 **ROMFS 根文件系统** 源码，编译时打包进固件，启动后挂载为设备上的 **`/etc`**（需 `CONFIG_ETC_ROMFS=y`）。

在 `bsp/CMakeLists.txt` 中登记：

```
nuttx_add_romfs(... RCSRCS etc/init.d/rcS etc/init.d/rc.sysinit
                   RCRAWS etc/group etc/1.txt ...)
```

| 文件 | 作用 |
|------|------|
| `init.d/rc.sysinit` | 系统早期启动脚本（示例：`echo rc.sysinit start`） |
| `init.d/rcS` | NSH 启动时执行的 rc 脚本（示例：`echo rcS start`） |
| `group` | 用户组配置，如 `root:*:0:root,admin` |
| `1.txt` | 示例/测试数据 |

**与 BSP 的区别：**

| 目录 | 内容 |
|------|------|
| `bsp/bsp_*.c`、`sifli_ap.c` | 引脚、上电、驱动 bring-up（硬件） |
| `bsp/etc/` | 根文件系统配置与启动脚本（软件） |

CMake 列表里写了的路径**必须存在**，否则 ninja 报 `rc.sysinit missing`。若 Gitee 仓库缺 `init.d/`，从官方 `vendor/sifli` 复制空文件后 push：

```bash
cp -r vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/etc/init.d \
      vendor/my_vendor/boards/sf32lb52/my_vendor/bsp/etc/
```

开机自动执行命令：编辑 `rcS` / `rc.sysinit` 写入 NSH 命令（如 `echo rcS start`）；新增文件需在 `CMakeLists.txt` 的 `RCRAWS` 中登记。上电应先后看到 `rc.sysinit start`、`rcS start`，再出现 `nsh>`。

**ROMFS、`RCSRCS`/`RCRAWS`、builtin、踩坑排查** 见 [nuttx_boot_flow.md](nuttx_boot_flow.md)（建议从目录 **第 3 节「隐性资源注册」** 读起；Shell/rcS 见第 4 节）。

**板级 `test_app`**：默认在 `sf32lb52_devkit_lcd_bringup()` 末尾异步 `task_spawn`（`launch=board`，保留 `nsh_main`）；添加步骤、rcS/代码启动等见 [test_app_guide.md](test_app_guide.md) **第 4 节**。

---

## 6. 构建输出目录

**作用：** 与官方 sifli 编译产物隔离。build.sh **原生命名**为 `cmake_out/<板目录名>_<配置名>/`，**不在** `vela_my_vendor_tools.py` 里配置输出路径。

做法：把板目录从 `sf32lb52_devkit_lcd` **改名为** `my_vendor`，原生输出即为：

```
cmake_out/
├── sf32lb52_devkit_lcd_nsh/    ← 官方 sifli build.sh
└── my_vendor_nsh/              ← my_vendor build.sh
    └── nuttx.bin
```

| 构建 | 板目录 | 输出目录 |
|------|--------|----------|
| 官方 sifli | `vendor/sifli/.../sf32lb52_devkit_lcd` | `cmake_out/sf32lb52_devkit_lcd_nsh/` |
| my_vendor | `vendor/my_vendor/.../my_vendor` | `cmake_out/my_vendor_nsh/` |

板路径：`vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/`

**CLion**（Build directory 与官方用法相同，只是路径不同）：

```
-B cmake_out/my_vendor_nsh
-S nuttx
-DBOARD_CONFIG=../vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/
-DCUSTOM_MODULE_PATH=<openvela>/build/cmake
-DEXTRA_FLAGS=-Wno-cpp -Wno-deprecated-declarations
-GNinja
```

改板时若目录改为 `my_board`，输出自动变为 `cmake_out/my_board_nsh/`，仍无需改脚本。

---

## 7. 改板（适配自己的硬件）

**作用：** 当 PCB 引脚、外设与 DevKit 不同时，修改板级 BSP 源码。  
**不是**改构建输出目录；输出由**板目录名**决定（见第 6 节）。

### 7.1 何时需要改板

- LCD / 触摸 I2C 引脚不同
- UART 日志口、按键 GPIO 不同
- Flash / PSRAM 容量或地址不同
- 外设启用组合不同（无 SPI、换 I2C 口等）

对照参考：`vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/`（只读）  
在你仓库里改：`vendor/my_vendor/boards/sf32lb52/my_vendor/`（或克隆出的新板目录）

### 7.2 克隆新板目录

```bash
cd vendor/my_vendor/boards/sf32lb52
cp -r my_vendor my_board
```

### 7.3 修改 defconfig

编辑 `my_board/configs/nsh/defconfig`：

```kconfig
CONFIG_ARCH_BOARD_CUSTOM_DIR="../vendor/my_vendor/boards/sf32lb52/my_board"
CONFIG_ARCH_CHIP_CUSTOM_DIR="../vendor/my_vendor/chips/sf32lb52"
```

外设开关也在此文件，例如 UART / I2C / LCD / 触摸：

```kconfig
CONFIG_BSP_USING_UART1=y
CONFIG_BSP_USING_I2C1=y
CONFIG_INPUT_FT6146=y
CONFIG_TOUCH_IRQ_PIN=31
CONFIG_LCD_USING_CO5300=y
```

可用 build.sh menuconfig 图形修改，再 savedefconfig 写回：

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_board/configs/nsh/ --cmake menuconfig
./build.sh vendor/my_vendor/boards/sf32lb52/my_board/configs/nsh/ --cmake savedefconfig
```

**⚠ 直接编辑 defconfig 在增量构建里是不生效的。** CMake 只在 defconfig 的
**路径**变化时重新初始化配置（`nuttx/CMakeLists.txt` 里比较的是
`NUTTX_DEFCONFIG` 与缓存的 `NUTTX_DEFCONFIG_SAVED`，两者都只是路径，
源码里还留着 `TODO: do also for changes in board/config`），所以**同路径改内容是静默忽略的**：
构建成功、`.config` 里却还是旧值。

改完必须强制重新配置，并且**grep 生成的 `.config` 确认值真的落地**：

```bash
rm -f cmake_out/my_vendor_nsh/.config cmake_out/my_vendor_nsh/.config.orig
touch nuttx/CMakeLists.txt          # 让 ninja/cmake 重跑 configure
python3 vela_my_vendor_tools.py build
grep -n '<CONFIG_NAME>' cmake_out/my_vendor_nsh/.config
```

`clean` 没用（它刻意保留 `.config`）；`fullclean` / `distclean` 也行但要全量重编。
另外 `nuttx/.config` 不能存在，否则 `nuttx/CMakeLists.txt` 会直接报错要求先
`make distclean`。

> **顺带一个坑**：`touch nuttx/CMakeLists.txt` 会把它盖成**当时**的时间戳。
> 如果之后系统时钟发生回退，它就会变成"未来"文件，导致 ninja **每次构建**都重跑
> cmake configure —— 而那次重配置可能因为构建目录里的生成文件陈旧而失败
> （报 `Cannot find source file: …/myvendor_zblue/hci_core.c` 之类），
> 极端情况下整个 `cmake_out/<cfg>/` 会被清掉、只能全量重编。
> 恢复办法就是再 `touch` 同一个文件把时间戳拉回当下。

### 7.3.1 两条已经踩过的配置经验

**① 更省事的重配置入口：树内自带的 `resetconfig` 目标。** 它就是"把 defconfig 拷成
`.config` 再跑 `olddefconfig`"（`nuttx/cmake/menuconfig.cmake` 里定义）：

```bash
cmake --build cmake_out/my_vendor_nsh --target resetconfig
# 或： ninja -C cmake_out/my_vendor_nsh resetconfig
```

代价是会按 defconfig **重刷** `.config`（用 menuconfig 手工加的项会丢），所以适合
"defconfig 是唯一真源"的用法。无论走哪条路，都要记住：`include/nuttx/config.h` 与
`apps/builtin/builtin_list.h` 都是 **configure 期**产物 —— 只改 `.config`（哪怕改对了）
它们也不会更新，必须有一步真正的 configure 重跑（`touch` 任一 `CMakeLists.txt`
或 `resetconfig`）。验证方式：`ninja -n` 应打印 `Re-running CMake…`。

**② LVGL 要求"可用栈"≥ 32768（开了 freetype/thorvg 时）。** 不够就会在开机打
`[LVGL][Error] check_stack_size: Stack size is too small`（`lv_nuttx_entry.c`）。
关键是**配置值 ≠ 可用值**：`lv_nuttx_init()` 用 `pthread_get_stacksize_np()` 取
`adj_stack_size`，而 NuttX 启动任务时会用 `up_stack_frame()` 从栈顶切走两块 ——
argv 块（`sched/task/task_setup.c`）与 TLS 块（`sched/tls/tls_initinfo.c`）。
本板实测 `bicycle` 少 **96 B**（argv 16 + TLS 76→80），所以：

| 配置值 | 可用值 | 结果 |
| --- | --- | --- |
| 32768 | 32672 | 差 96 B，开机 ERROR |
| **34816**（现行） | 34720 | 通过，余量 ~1.9 KB |

UI 线程只有一个创建点：`bsp/bringup.c` 的静态栈 `g_bicycle_stack[]`（`task_spawn` +
`posix_spawnattr_setstackaddr`），所以 `CONFIG_MYVENDOR_BICYCLE_STACKSIZE` 一处配置
同时决定它和 `builtin_list.h` 里 `bicycle` 的 STACKSIZE；这块栈在 **SRAM** `.bss`
（不是 PSRAM 池），调大要算 SRAM 余量（`nuttx.map` 的 `_ebss` 距 `sram` 区尾）。
现场复核：`nsh> ps` 的 STACK 列 = 34720、开机日志里 `check_stack_size` 那行同值。

### 7.4 修改 BSP 源文件

| 你的硬件 / SDK 内容 | 修改文件 | 说明 |
|--------------------|----------|------|
| 引脚复用 | `bsp/bsp_pinmux.c` | LCD QSPI、I2C、UART、PSRAM、SPI Flash |
| LCD / 触摸 GPIO | `bsp/bsp_lcd_tp.c` | reset、power enable、VADD |
| PSRAM / Flash 初始化 | `bsp/bsp_init.c` | QSPI 模式、容量 |
| 电源时序 | `bsp/bsp_power.c` | 上电 / 下电 |
| 设备 bring-up | `bsp/sifli_ap.c` | I2C、触摸、LCD 异步初始化 |
| 按键 | `include/board.h`、`bsp/sf32lb52_buttons.c` | 按键 GPIO |
| 内存布局 | `scripts/ld.script` | Flash / SRAM / PSRAM 地址与大小 |
| 启动脚本 / /etc | `bsp/etc/` | 见第 5 节 |

**DevKit 默认引脚（CO5300 + FT6146，供 diff）：**

| 功能 | 引脚 |
|------|------|
| LCD Reset | PA00 |
| LCD Power En | PA10 |
| CO5300 VADD | PA37 |
| Backlight PWM | PA01 |
| LCDC1 QSPI | PA02–PA08 |
| Touch Reset | PA09 |
| Touch IRQ | PA31 |
| Touch I2C | PA30 / PA33 |
| 按键 KEY2 | PA11 |

面板 / 触摸驱动在 `boards/sf32lb52/drivers/`（`lcd/co5300.c`、`input/ft6146.c`），换 IC 时改 drivers 并更新 defconfig。

### 7.5 更新构建脚本

改 `vela_my_vendor_tools.py` 中的 `BOARD_CONFIG`（若用封装脚本），或直接用 build.sh 路径：

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_board/configs/nsh/ --cmake -j16
```

### 7.6 编译验证

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_board/configs/nsh/ --cmake -j16
```

diff 官方 DevKit：

```bash
diff -ru vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/ \
         vendor/my_vendor/boards/sf32lb52/my_board/src/
```

改完 push 到 Gitee：`vendor/my_vendor` 仓库提交板级改动。  
`vendor/sifli/` 保持官方原样，仅作参考。
