# Vela 低层改动：scan-stop 在"适配器拆除窗口"里的空指针（板子 panic 根因）

> 本文件属于「上游必改件」记录（见 [README.md](README.md)）：改动**不在 vendor 编译单元里**，
> 改的是 zblue 自己的主机栈文件，`repo sync` 之后必须重新应用。
>
> 对应补丁：[patches/zblue-scan-stop-null-ctx.patch](patches/zblue-scan-stop-null-ctx.patch)
>
> **构建时的落地机制（2026-09-24 起）**：`vela_override/zblue/patch_scan_stop_null.py`。
> CMake 在 configure 阶段跑它，把改动写进构建副本
> `cmake_out/my_vendor_nsh/myvendor_zblue/myvendor_scan.c`，并按路径后缀顶掉上游那份
> —— 于是**上游 git 保持不动**（`git status` 干净，不再有就地改的 `M`）。
> 本目录的 `.patch` 是同一改动的另一份表达（`repo sync` 后手工应用用），两份必须一致：
> 改了脚本就重新导出 `python3 zcode/gen_zblue_pitch_patch.py scan`（只打印，不落盘），
> 再对构建副本做一次 `git apply` + `diff` 验证。
>
> 为什么是脚本而不是继续就地改：打过 pitch 补丁的树再走一次构建脚本会锚点不中，
> 脚本会**拒绝猜**（锚点匹配数 ≠ 1 直接报错），CMake 只打 WARNING 后继续 ⇒ 静默退回
> 旧版本，属于"build 成功 ≠ 生效"那一类坑。两份表达同源才能避免。

## 改哪个组件、基于哪个版本（**换版本前一定先对这张表**）

| 项 | 值 |
|---|---|
| 仓库 | `external/zblue/zblue`（独立 repo 工程 `zblue`） |
| 写入时的 HEAD | `6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3`（`6f79fb2a0f8`，2026-04-22 `chore: sync .github…`） |
| 文件 | `subsys/bluetooth/host/scan.c` |
| 该文件在 HEAD 的 blob | `8d6ff66b5ff51cb75908cb2e6a06aa4398401a0c` |
| 补丁写于 | 2026-09-19（只堵 softreset + stop_mc 两处） |
| **补齐于** | **2026-09-24**（同一根因的第 3、4 处：`bt_le_scan_user_add` / `bt_le_scan_user_remove` 入口；并把前两处收进构建脚本，上游树回到 pristine） |

**怎么判断是否需要重新适配**（版本漂移时最省事的三步）：

```bash
git -C external/zblue/zblue rev-parse HEAD
git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/scan.c   # 应等于上表 blob
git -C external/zblue/zblue apply --check \
    ../../../vendor/my_vendor/docs/pitch/patches/zblue-scan-stop-null-ctx.patch
```

- HEAD 变了但 blob 没变 ⇒ 文件没被动过，**照着下面"改后"直接改这两处**即可；
- blob 也变了 ⇒ 先 `git log -p -1 <blob>..HEAD -- subsys/bluetooth/host/scan.c` 看上游怎么改的，
  再按本文件的意图重做（判空仍应加在 `bt_scan_softreset()` 入口）。

## 现场（实机 crash note，2026-09-18 20:35）

`/mnt/kv/…` 里的 `n007_20260918_203502.txt`：

```
kind=assert   file=../../nuttx/arch/arm/src/arm_m/arm_memfault.c  line=136
pid=48 name=ble_companion
cfsr=00000082   mmfar=00000004        ← DACCVIOL：读/写地址 0x4
pc=10106aa6     lr=101071cd
```

用当次镜像的 ELF（`/home/jinsc/SDK/vela/elf/my_vendor+nsh+20260918-172158+892bab5d.elf`）解出来：

```
pc → bt_scan_softreset      scan.c:111
lr → bt_le_scan_stop_mc     scan.c:1841
```

`mmfar=0x4` 正是 `struct bt_dev_scan_ctx` 里 `scan_dev_found_cb` 的偏移 ⇒
**`hdev->scan_ctx == NULL`，代码在往空指针 +4 写**。

同一时刻的串口日志：

```
[2344.710] ble_companion: adapter off done, enable after 8000 ms      ← 栈已被拆
[coredump] assert … arm_memfault.c:136 irq=1 writing kv
```

以及更长一段里已有的征兆：

```
[2302.5] wdog: clipped corrupt g_wdactivelist (1) …（waitdog 节点被写坏，两个任务被摘）
[2357.0] myvendor net_buf: pool 0x20001c78 count=4 empty 2000 ms, giving up (n=0)
[2280-2365] sal adv ticket n=1825 serving=1810   ← 票据序号在涨、serving 冻住：
                                                    SAL 里压着一批 GAP 工作项没跑完
```

## 机制

`hdev->scan_ctx` 的**唯一赋值点**是 `bt_scan_reset()`（`scan.c:121`），它只在两处被调用：
`hci_reset_complete()`（`hci_core.c:2523`，**HCI_RESET 完成之后**）与 `bt_finalize_init()`
（`hci_core.c:4333`）。而设备结构在初始化路径上会被 `memset(hdev, 0, sizeof(*hdev))`
（`hci_core.c:187`）清零，`scan_ctx` 随之变 NULL。

于是**"适配器已关、HCI_RESET 还没回来"这个窗口**里，任何一次 scan-stop 都会踩空。
上游对 **start** 有保护（`bt_le_scan_start_mc`：`if (!atomic_test_bit(hdev->flags, BT_DEV_READY))
return -EAGAIN;`），**stop 这条漏了**。

这个窗口是真实存在的：板级 `adapter cycle`（恢复阶梯第一级）就是"disable → 等 8 s → enable"，
而 SAL 里排队的工作项会在 disable 之后才被 worker 执行 —— 上面的 `serving=1810` 就是证据，
**从上层堵不干净**，所以修在栈里。

## 改动前（上游 HEAD 原文）

注意：`bt_le_scan_stop_mc()` 里紧随 `bt_scan_softreset()` 之后的
`hdev->scan_ctx->scan_dev_found_cb = NULL;` 是**同一窗口的第二颗雷**
（同一条路径，所以那次先崩在前面那句）。

`subsys/bluetooth/host/scan.c`（blob `8d6ff66b5ff`）：

```c
void bt_scan_softreset(struct bt_dev *hdev)
{
	hdev->scan_ctx->scan_dev_found_cb = NULL;
#if defined(CONFIG_BT_EXT_ADV)
	reset_reassembling_advertiser(hdev);
#endif
}
```

```c
int bt_le_scan_stop_mc(uint8_t dev_id)
{
	struct bt_dev *hdev = bt_dev_get(dev_id);
	if (!hdev) {
		return -ENODEV;
	}

	bt_scan_softreset(hdev);
	hdev->scan_ctx->scan_dev_found_cb = NULL;

	if (IS_ENABLED(CONFIG_BT_EXT_ADV) &&
	    atomic_test_and_clear_bit(hdev->flags, BT_DEV_SCAN_LIMITED)) {
		atomic_clear_bit(hdev->flags, BT_DEV_RPA_VALID);

#if defined(CONFIG_BT_SMP)
		bt_id_pending_keys_update(hdev);
#endif
	}

	return bt_le_scan_user_remove(hdev, BT_LE_SCAN_USER_EXPLICIT_SCAN);
}
```

## 改动后（完整函数，可直接照抄）

```c
void bt_scan_softreset(struct bt_dev *hdev)
{
	/* 板级改动（my_vendor，见 docs/pitch/zblue-scan-stop-null-ctx.md）：
	 * `hdev->scan_ctx` 只在 HCI_RESET 完成（hci_reset_complete → bt_scan_reset）
	 * 或 bt_finalize_init 里被赋值，设备结构在初始化路径上会被 memset 清零。
	 * 于是"适配器已拆掉、复位还没回来"这个窗口里再来一次 scan-stop，就是
	 * hdev->scan_ctx == NULL 解引用 —— 实机 2026-09-18 20:35 的 panic 正是它
	 * （MemManage cfsr=0x82 mmfar=0x4，pc=bt_scan_softreset scan.c:111，
	 *  lr=bt_le_scan_stop_mc scan.c:1841，task=ble_companion）。
	 *
	 * 调用方可能来自 SAL 里排队的工作项（适配器 cycle 拆栈时队列还没清空），
	 * 从上层堵不干净，所以在这里直接变 no-op。 */
	if (hdev == NULL || hdev->scan_ctx == NULL) {
		LOG_WRN("scan softreset skipped, scan ctx not ready");
		return;
	}

	hdev->scan_ctx->scan_dev_found_cb = NULL;
#if defined(CONFIG_BT_EXT_ADV)
	reset_reassembling_advertiser(hdev);
#endif
}
```

```c
int bt_le_scan_stop_mc(uint8_t dev_id)
{
	struct bt_dev *hdev = bt_dev_get(dev_id);
	if (!hdev) {
		return -ENODEV;
	}

	bt_scan_softreset(hdev);

	/* 同一窗口的第二处解引用（scan.c:1856）：softreset 已经判过 NULL，
	 * 那次没 panic 只是因为同一路径，但这里同样不能裸写。 */
	if (hdev->scan_ctx != NULL) {
		hdev->scan_ctx->scan_dev_found_cb = NULL;
	}

	if (IS_ENABLED(CONFIG_BT_EXT_ADV) &&
	    atomic_test_and_clear_bit(hdev->flags, BT_DEV_SCAN_LIMITED)) {
		atomic_clear_bit(hdev->flags, BT_DEV_RPA_VALID);

#if defined(CONFIG_BT_SMP)
		bt_id_pending_keys_update(hdev);
#endif
	}

	return bt_le_scan_user_remove(hdev, BT_LE_SCAN_USER_EXPLICIT_SCAN);
}
```

- 语义：**没初始化好的扫描上下文本来就没有"回调要清"这回事**，跳过即正确。
- `LOG_WRN` 在本配置下**可能被编译掉**（不影响判空逻辑）：确认改动是否进了镜像要看反汇编，
  别看日志里有没有那句话。

## 2026-09-24 补齐：同一个窗口的第 3、4 处

2026-09-18 那次只堵了**本次故障路径上的两处**（softreset 与紧随其后的 stop_mc 尾部）。
但"`scan_ctx == NULL`"这个前提是对**所有从上层进得来的入口**成立的，不只是 stop：

| 入口 | 裸解引用在哪 | 谁会走到 |
|---|---|---|
| `bt_le_scan_user_remove()`（新增判空） | 函数体 `atomic_clear_bit(hdev->scan_ctx->scan_state.scan_flags, …)`；以及它调用的 `scan_update()` 第一行 `k_mutex_lock(&hdev->scan_ctx->…scan_update_mutex, K_NO_WAIT)`（`scan.c:467`） | 8 个调用点，其中 `hci_core.c:1106/1609/1707` 是连接完成/取消的收尾 —— 正是"链路事件撞上适配器拆栈"的组合 |
| `bt_le_scan_user_add()`（新增判空） | `scan_check_if_state_allowed()` 第一行 `atomic_test_bit(hdev->scan_ctx->scan_state.scan_flags, flag)`（`scan.c:517`） | conn.c 四条连接路径 + `bt_le_scan_start_mc` |

返回值口径**不是随手挑的**：

- `user_add` 判空返回 **-ENODEV**：这里返回 0 反而危险 —— 上层会认为"flag 已经置上、
  扫描已经起来了"，从而不再重试，等于把 bug 藏起来。
- `user_remove` 判空返回 **0**：scan_ctx 不存在时"别扫了"这个目标状态本来就成立；
  返回非零会被上层当成一次可重试的失败（本仓的历史教训就是拿错误码驱动重试会变成风暴）。
  受影响的三个 best-effort 调用点（`hci_core.c:1106/1609/1707`）都是 `if (err) LOG_WRN(...)`，
  多一条 WRN 就够。
- `bt_scan_softreset` 是 `void`：直接 `return`，语义是"没有回调要清"。
- `bt_le_scan_stop_mc` 尾部：只判空再写，不改控制流。

两处新增判空都带各自唯一的 `LOG_WRN` 串（`scan state add skipped` / `scan state remove
skipped`），方便 grep 计数这条窗口被踩到的次数；窗口本身很短，不构成刷屏源。

## 取舍与不做的部分

- 只堵**入口**：四个位置合起来正好是"从上层能进 scan 模块的四条门"
  （softreset / stop_mc / user_add / user_remove），而不是把 `scan.c` 里 30 余处
  `hdev->scan_ctx->` 逐个加判空 —— 逐个加是噪音，真正的约束是"不要在窗口里调进来"。
  同族的 `bt_le_scan_start_mc` 上游已有 `BT_DEV_READY` 门，不必重复。
- 更治本的方向（未做）：让 adapter cycle **在 disable 之前等 SAL 队列排空**，
  或让 SAL 在拆栈时取消排队的工作项。前者在 Vela 侧；后者可做在板级 SAL 抽换件
  （`vela_override/bluetooth/sal_le_{scan,advertise}_interface.c`，属于本仓库，不进 pitch）。

## 验证

**① 两份表达是否等价**（改完脚本/补丁就该跑一次，0 成本的强判据）：

```bash
cd /home/jinsc/SDK/vela/openvela
git -C external/zblue/zblue apply \
    vendor/my_vendor/docs/pitch/patches/zblue-scan-stop-null-ctx.patch
diff external/zblue/zblue/subsys/bluetooth/host/scan.c \
     cmake_out/my_vendor_nsh/myvendor_zblue/myvendor_scan.c \
  && echo "补丁 == 构建副本"
git -C external/zblue/zblue apply -R \
    vendor/my_vendor/docs/pitch/patches/zblue-scan-stop-null-ctx.patch
```

**② 判空是否真在目标码里**（推荐，因为 `LOG_WRN` 在本配置下可能被编译掉 ——
实测对象文件里 grep 不到那几个格式串，所以只能看反汇编）：

```bash
# 注意：改过之后对象名带 myvendor_ 前缀，别再用 `-name scan.c.o`
O=$(find cmake_out/my_vendor_nsh -name myvendor_scan.c.o | head -1)
# 注意：prebuilts/gcc/linux-aarch64 那份 objdump 在本机（x86_64）跑不了，用系统的
arm-none-eabi-objdump -d "$O" |
  awk '/<bt_le_scan_user_remove>:/{f=1} f{print} f&&/^$/{exit}' | head -8
```

期望**两个 `cbz` 挡在函数体前面**（2026-09-24 实测）：

```
00000000 <bt_le_scan_user_remove>:
   0:	cbz	r0, 3e                ← hdev == NULL
   2:	mov	r2, r0
   4:	ldr.w	r3, [r0, #700]	@ 0x2bc   ← scan_ctx
   8:	cbz	r3, 42                ← scan_ctx == NULL
   a:	push	{r7, lr}                ← 真函数体从这里开始
```

`bt_scan_softreset` 同理（`cbz` × 2 挡在 `str [r2, #4]` 之前，就是原来崩的那句）。

**③ 归档成员**：`ar t .../libzblue.a | grep scan` 只应看到 `myvendor_scan.c.o`，
不能再有上游的 `scan.c.o`（两份都在会以先入者为准，白改）。

实机：进入一次 `adapter cycle`（码表断电再上电，或 `ctl radio off` → `on` 触发恢复阶梯），
不该再出现 `arm_memfault.c … mmfar=00000004` 的 panic。

## 回退

上游树本来就是 pristine（改动只在构建副本里），所以回退 = 改
`vela_override/CMakeLists.txt`：注释掉 `myvendor_patch_zblue_scan_null()` 调用
（或删掉 `vela_override/zblue/patch_scan_stop_null.py`），重新 configure + 构建即可。
就地改那版的回退方式（`git checkout -- subsys/bluetooth/host/scan.c`）已不适用。
