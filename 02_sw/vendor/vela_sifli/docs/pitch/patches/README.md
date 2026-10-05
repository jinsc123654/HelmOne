# 补丁（相对各子仓根）

这五份补丁覆盖**`vendor/my_vendor` 之外的全部改动**（2026-09-19 用 `repo status` 全树核对：
除它们之外，`nuttx` / `packages` / 其它子仓都是干净的）。五份都已实测：
**在各自仓库的 HEAD 上空打一遍，得到的文件与本机现状逐字节相同**。

> ⚠️ **2026-09-24 起，前两份还多了一条路**：`scan.c` / `id.c` 的改动同时由板级
> `vela_override/zblue/patch_*.py` 在**构建时**写进构建副本
> （`cmake_out/<build>/myvendor_zblue/myvendor_scan.c` / `myvendor_id.c`），
> 上游 git 因此保持不动（`git -C external/zblue/zblue status` 干净）。
> 本目录的 `.patch` 是同一改动的另一份表达，**两份必须一致**：apply 本补丁后
> `diff` 那份构建副本应为空（命令见 [zblue-scan-stop-null-ctx.md](../zblue-scan-stop-null-ctx.md) 的「验证」）。
> 不一致的后果不是报错而是**静默退回旧版本** —— 构建脚本锚点不中时只打 CMake WARNING。
> 改了脚本要重新导出：`python3 zcode/gen_zblue_pitch_patch.py scan|id`（只打印，不落盘）。

| 文件 | 在哪个仓库执行 | 改的文件 | 说明文档 |
|------|----------------|----------|----------|
| [zblue-id-scan-eacces.patch](zblue-id-scan-eacces.patch) | `external/zblue/zblue` | `subsys/bluetooth/host/id.c` | [ble.md](../ble.md)（构建时脚本 `patch_id_scan_addr.py`） |
| [zblue-scan-stop-null-ctx.patch](zblue-scan-stop-null-ctx.patch) | `external/zblue/zblue` | `subsys/bluetooth/host/scan.c` | [zblue-scan-stop-null-ctx.md](../zblue-scan-stop-null-ctx.md)（panic 修复；**4 处**判空，2026-09-24 补齐，构建时脚本 `patch_scan_stop_null.py`） |
| [vela-hci-h4-rx-reopen.patch](vela-hci-h4-rx-reopen.patch) | `frameworks/connectivity/bluetooth` | `service/stacks/zephyr/hci_h4.c` | [ble-hci-h4-rx-reopen.md](../ble-hci-h4-rx-reopen.md) |
| [apps-lvgl-myvendor-stack.patch](apps-lvgl-myvendor-stack.patch) | `apps` | `graphics/lvgl/CMakeLists.txt`、`graphics/lvgl/Makefile` | [lvgl.md](../lvgl.md) |
| [apps-lvgl-kconfig.patch](apps-lvgl-kconfig.patch) | **`apps/graphics/lvgl/lvgl`**（它自己是个 project，不是 `apps`！） | `Kconfig` | [lvgl.md](../lvgl.md) |
| [nuttx-kconfig-quote-blank.patch](nuttx-kconfig-quote-blank.patch) | `nuttx` | `cmake/nuttx_kconfig.cmake` | [nuttx-kconfig-quote-blank.md](../nuttx-kconfig-quote-blank.md)（CMake ≥3.23 不接受 `.config` 空行展开成零参数；**没有抽换件可用**，只能打补丁） |

> ⚠️ 最后一份的仓库是 **嵌套的 LVGL project**（manifest 里的 `apps_graphics_lvgl`），
> 在 `apps` 仓库里执行会报"路径不在 HEAD 中"。它必须打，否则 config 生成出来的 `CONFIG_LV_*`
> 少一批（如 `LV_SUNDAY_STR`），编到 `widgets/calendar/lv_calendar.c` 直接报
> `error: 'CONFIG_LV_SUNDAY_STR' undeclared`（2026-09-19 实测）。

**一键打（幂等，已打过的会跳过）与打完的自查命令，见 [../README.md](../README.md) 的
「一键打上全部补丁」一节** —— 这里不重复第二份命令清单，省得两处不一致。
只想逐份核对基版本时，用上面表格里的仓库逐个 `git apply --check` 即可。

打完 Kconfig 那份之后，如果构建目录里已经有旧的 `.config`，要让它重新推导一次
（例如删掉 `cmake_out/<cfg>/.config`），否则旧配置里缺的符号不会自己回来。

冲突时不要 `--3way` 乱合：先按每份说明文档里的"上游仓库 @ HEAD + 文件 blob"确认基版本，
对不上就按文档给的**整段改前/改后函数**手改。

## 基版本对不上时怎么判断

每份补丁的说明文档都带一张 id 表（仓库 HEAD + 文件 blob），可以现场比对：

```bash
git -C external/zblue/zblue rev-parse HEAD
git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/scan.c    # 对比文档里的 blob
git -C external/zblue/zblue diff -- subsys/bluetooth/host/scan.c           # 本地已经改了哪些
```

## 打完之后怎么自查

```bash
git -C <repo> diff -- <file> | diff - vendor/my_vendor/docs/pitch/patches/<patch>   # 与补丁应完全一致
git -C <repo> apply --check -R vendor/my_vendor/docs/pitch/patches/<patch>          # 反向也干净 ⇒ 补丁与现状逐字相符
```

## 不要动的东西

- 官方 `apps/graphics/lvgl/lvgl/` 工作区：**`.c/.h` 保持上游原样**（本板 `CONFIG_MYVENDOR_LVGL_STACK=y`，
  真正参与编译的 LVGL 在 `vendor/my_vendor/apps/graphics/lvgl/`，编译图中的对象都在
  `boards/exclude_board/myvendor_lvgl/` 下；见 [lvgl.md](../lvgl.md)）。
  2026-09-19 已把这棵树从实验残留（1768 改 / 799 删 / 2379 未跟踪）恢复为 checkout 原样，
  **但只有 `Kconfig` 必须留着 9.5 那版** —— config 生成走 `apps/graphics/lvgl/Kconfig`
  的 `osource ".../lvgl/Kconfig"`，上游旧版缺 `LV_SUNDAY_STR` 之类的符号，编 `lv_calendar.c` 会失败
  （见上表最后一行与 [apps-lvgl-kconfig.patch](apps-lvgl-kconfig.patch)）。
  也就是说：这棵树现在**只有 `Kconfig` 一处是有意保留的改动**，其余与上游一致。
- `apps/testing/drivers/nist-sts/`：它在 manifest 里是**独立 project**（`apps_testing_drivers_nist-sts`），
  同一个路径下父仓库 `apps` 也跟踪了 7 个文件（`CMakeLists.txt`/`Kconfig`/`Make.defs`/`Makefile`/两个 patch/`.gitignore`），
  于是两个仓库互相把对方的文件报成 untracked。**目录里没有多余文件**（83 个文件全部被某一方跟踪，
  2026-09-19 核对），是 openvela 的重叠检出，不是脏树，也不需要补丁。本板不编它（无产物）。
- 另有两份"整文件替换件"的**全文**放在 [replace/](../replace/) 下，供对不上时手抄。
