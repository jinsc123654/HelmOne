# My Vendor 编译与烧录指南

使用 `vendor/my_vendor/` 编译 SF32LB52 固件。  
板卡制作见 [board_guide.md](board_guide.md)；**工具命令全文**见 [tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)。

---

## 1. 前置条件

- OpenVela 工程已同步（含 `vendor/my_vendor`；`vendor/sifli` 作参考可选）
- ARM 交叉工具链、CMake、Ninja、sftool

### 1.1 把这份工程交给别人：能不能编出一样的固件（2026-09-19 实测审计）

**拷整个文件夹 ⇒ 可以**（未提交的东西也在里面）；**走 git ⇒ 会缺三块**，按下表补齐即可。

| 缺什么 | 现状 | 怎么补 |
|---|---|---|
| ① 上游 5 处改动（5 个文件 / 4 个仓库） | 不在 `vendor/my_vendor` 里（在 `apps` / `apps/graphics/lvgl/lvgl` / `zblue` / `frameworks_bluetooth` 各自的 git 里，**sync 会冲掉**）。2026-09-19 用 `repo status` 全树核对：**这 5 处之外没有别的改动** | 打 `docs/pitch/patches/` 里的五份补丁（清单与命令见 [patches/README.md](pitch/patches/README.md)）。五份都已实测：在各自仓库 HEAD 上空打一遍，得到的文件与本机**逐字节相同**。注意其中 `apps-lvgl-kconfig.patch` 要在**嵌套仓库** `apps/graphics/lvgl/lvgl` 里打 |
| ② `vendor/my_vendor` 本身不在 vela 的 manifest 里 | 2026-09-19 起已**提交并推送**到 `https://gitee.com/jinsc123654/vela_sfili.git`（`master`，与 origin 同步） | `git clone https://gitee.com/jinsc123654/vela_sfili.git vendor/my_vendor`（或直接拷文件夹） |
| ③ `boot_loader/bin/*.bin` | **未入库**（`ftab.bin` / `bootloader.bin` / `fs_root*` / `fat_root.bin`），由 SiFli SDK 生成；只影响**烧录**，不影响 `build` | 装 SiFli SDK 后 `build_board.py build-boot`（+ `pack-sd-img`），或把这几个 bin 一并给出 |

主机侧依赖（不阻塞，文档已写）：`python3` + `pyserial`（monitor）、`sftool 0.1.16`（`~/.sifli/tools/sftool/`）、
工具链在仓库自带 `prebuilts/` 下。烧录用 `sftool -m sd`（介质见 `boot_loader/storage.conf`）。

### 1.2 抽换件（`vela_override/`）：随仓库走，但要看头部对版本

`vela_override/` 下的 19 个文件（16 个 `.c` + 3 个 `patch_*.py`）**在本次仓库里**，
构建时按**文件名**顶掉上游同名 `.c`（或由 CMake 调脚本地改写 zblue 的构建副本），
**上游 git 不动** ⇒ `repo sync` 收不走、也不需要 `docs/pitch`。

接手的人要判断"换了上游版本后还能不能这么替"，看每个文件的**头部**（2026-09-19 起都有）：

```
 *   替的是上游 : nuttx / sched/wdog/wd_start.c
 *   写入时 HEAD: 2ce740a0ac1052c5f51083a334ef3093f59ff780
 *   上游 blob  : f9d8340186a95e7f1d8602020c8da463ec247fe2
 *   为什么抽换 : PSRAM waitdog 视为合法；断链才剪环，避免 wd_insert HardFault
 *   版本漂移自查:
 *     git -C nuttx rev-parse HEAD:sched/wdog/wd_start.c   # 与上面的 blob 比对
 *     git -C nuttx diff -- sched/wdog/wd_start.c          # 上游若已前进，先看这里再决定还要不要抽换
```

- `patch_*.py` 是**构建期补丁脚本**（不是同名替换），头部列出它改的每个上游文件与 blob（个别脚本改两个文件）；
- 抽换件是否真的编进去了，两条命令（详见 [docs/pitch/README.md](pitch/README.md) 与 `vela_override/CMakeLists.txt` 顶部）：

```bash
ninja 2>&1 | grep "override compiled"                       # 本次构建真的编译了哪些抽换件
strings -a cmake_out/<cfg>/nuttx.bin | grep -c '^vela_override/'   # 哪些进了镜像（现 19；裸 bin/strip 后都在）
```

第二条数的是每个抽换件自己带的 `.myvendor_marker` 段常量（只占 flash，不占 SRAM；段由板级
`scripts/ld.script` 的 `KEEP` 保住，否则会被 `--gc-sections` 丢掉）。细节见
[docs/pitch/README.md](pitch/README.md) 的"镜像侧标记的坑与做法"。

⚠️ 一句提醒：这些 `__pycache__/*.pyc` 目前被 git 跟踪着，跑脚本就会变脏（建议 `git rm --cached` + `.gitignore`）。

**逐位相同做不到**：OVNX 头里的 `build_date` 默认取 wrap 时的当前时间
（`scripts/ovnx_version.py` 支持传入固定值），所以同一份代码两次烧录的镜像不一样；
**功能一致**，比对时看 `payload_len` + 源码一致而不是整文件 md5。

**自查"别人拿到后缺什么"**：

```bash
# ① 全树还有哪些子仓被动过（补丁应该正好覆盖这些）
repo status -j16 | grep -v '\.mimosa' | grep -B1 '^ [-mad]' | grep '^project'
# ② vendor 有没有没提交的东西（新机 clone 后应为空）
git -C vendor/my_vendor status --porcelain
# ③ 烧录产物在不在
ls boot_loader/bin/*.bin
```

`repo status` 里还会看到两处**与固件无关**的条目，都不用管：

- 官方 `apps/graphics/lvgl/lvgl/`：本板 `CONFIG_MYVENDOR_LVGL_STACK=y`，真正的 LVGL 在 `vendor/my_vendor/apps/graphics/lvgl/`，
  官方那棵树**不参与编译**。它曾被 LVGL 9.5 覆盖成脏树，2026-09-19 已恢复为 checkout 原样（`repo status` 现在不报它）。
- `apps/testing/drivers/nist-sts/`：manifest 里它是独立 project，同一路径父仓库 `apps` 也跟踪了 7 个文件，
  两个仓库互相把对方的文件报成 untracked。**没有多余文件**（83 个文件全部被某一方跟踪），不是脏树。

详见 [patches/README.md](pitch/patches/README.md)。

---

## 2. 快速编译

```bash
# openvela 根目录
python3 vendor/my_vendor/build_board.py build
```

或 build.sh：

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ --cmake -j$(nproc)
```

固件：`cmake_out/my_vendor_nsh/nuttx.bin`（烧录用 `nuttx.flash.bin`）。

---

## 3. 配置与保存

```bash
python3 vendor/my_vendor/build_board.py menuconfig
python3 vendor/my_vendor/build_board.py savedefconfig
```

---

## 4. 烧录与监视

```bash
# 固件 only（不含 NAND 文件系统）
python3 vendor/my_vendor/build_board.py build flash monitor

# 全量（boot + factory + main + bicycle 资源 fs_root.bin）
python3 vendor/my_vendor/build_board.py build-all flash-all monitor
```

`flash` / `flash-fs` / `flash-all` 区别见 [vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md) 第 4 节。

SD/eMMC 启动的卡布局、`/mnt/kv` 与 `/mnt/lfs`、pack/burn 见 [sd_partition.md](sd_partition.md)。  
二级 boot msh、停留调试、USB-CDC 烧录见 [boot_2sfbl.md](boot_2sfbl.md)。  
工厂固件（msh `factory`、MTP 不在 2SFBL）见 [factory_firmware.md](factory_firmware.md)。  
产品固件放在 `/mnt/kv/fw` 的 A/B 槽见 [fs_firmware.md](fs_firmware.md)。  
新 `repo sync` 的干净树上，BLE 双角色必改点见 [required_patches.md](required_patches.md)。

---

## 5. 对照官方 sifli（只读参考）

```bash
python3 vendor/sifli/build_board.py build flash monitor
```

---

## 6. 清除重编

```bash
python3 vendor/my_vendor/build_board.py fullclean
python3 vendor/my_vendor/build_board.py build
```
