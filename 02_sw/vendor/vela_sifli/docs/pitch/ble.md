# BLE：zblue `id.c` 扫描地址 `-EACCES` 回退

**上游文件：** `external/zblue/zblue/subsys/bluetooth/host/id.c`  
**函数：** `bt_id_set_scan_own_addr()`  
**CMake 目标：** `zblue`（`libzblue.a`）  
**替换件：** [replace/external/zblue/zblue/subsys/bluetooth/host/id.c](replace/external/zblue/zblue/subsys/bluetooth/host/id.c)  
**补丁：** [patches/zblue-id-scan-eacces.patch](patches/zblue-id-scan-eacces.patch)  
**构建时机制（2026-09-24 起）：** `vela_override/zblue/patch_id_scan_addr.py`
（CMake configure 阶段跑，写进 `cmake_out/<build>/myvendor_zblue/myvendor_id.c` 并按路径后缀顶掉上游那份 ⇒
**上游 git 保持不动**）。上面那份 `.patch` 与它逐字节等价，校验与同步办法见
[patches/README.md](patches/README.md) 顶部注记。  
**对照 HEAD：** `external/zblue/zblue` @ `6f79fb2a0f83`，改前 blob `e2e8f4824dfc`

## 组件 id（换版本前先对这张表）

| 项 | 值 |
|---|---|
| 仓库 | `external/zblue/zblue`（独立 repo 工程 `zblue`） |
| 写入时 HEAD | `6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3` |
| 文件 | `subsys/bluetooth/host/id.c` |
| 该文件在 HEAD 的 blob | `e2e8f4824dfc6860517df6c5afc00580dcd2f617` |

```bash
git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/id.c          # 应等于上表 blob
git -C external/zblue/zblue apply --check ../../../vendor/my_vendor/docs/pitch/patches/zblue-id-scan-eacces.patch
```

HEAD 动了但 blob 没动 ⇒ 文件没被改过，照下面两段改；blob 也变了 ⇒ 先看上游这段怎么改的再重做。

相关：[required_patches.md](../required_patches.md) §2、[ble/ble_sensor.md](../ble/ble_sensor.md) §3.6。

---

## 原因

本板是 **一台射频、双角色**：Companion 给手机打 legacy 广播（Peripheral），同时 `ble_sensor` 做观察者扫 HR/CSC/CPS（Central）。

`bt_le_start_scan` 在 `CONFIG_BT_PRIVACY` 下会走 `bt_id_set_private_addr()`，发 HCI **LE Set Random Address**。SF32 LCPU 在下面任一状态都可能回 **Command Disallowed**（host 侧 `-EACCES` / `-13`）：

- 正在广播
- 正在扫描
- 正在 initiating
- **已经有一条 Peripheral 链路**（手机已连上）

上游只在「已经在扫 / 正在 initiating」时忽略这个错误。手机已经连着时启动 observer，命令仍失败 → `set scan own addr failed (-13)` → 传感器扫不起来、也不能重连。

`ble_sensor` 的 ADV / scan **时间片**只解决「手机断开后不要长期同时占射频」。它不能替代本补丁：手机已连接时根本没有广播窗口，但链路还在，LCPU 照样拒改随机地址。

---

## 改法

在 `bt_id_set_scan_own_addr()` 的 `CONFIG_BT_PRIVACY` 分支里，把：

```c
if (err == -EACCES && (atomic_test_bit(hdev->flags, BT_DEV_SCANNING) ||
                       atomic_test_bit(hdev->flags, BT_DEV_INITIATING))) {
    LOG_WRN("Set random addr failure ignored in scan/init state");
```

改成：**任意 `-EACCES` 都沿用当前 identity，写好 `own_addr_type`，返回 0，让扫描继续。**

```c
		err = bt_id_set_private_addr(hdev, BT_ID_DEFAULT);
		if (err == -EACCES) {
			/* LE Set Random Address is disallowed while advertising,
			 * scanning, initiating, or (on some controllers) while a
			 * peripheral link is up.  Keep the current identity and
			 * still start the observer.
			 */
			LOG_WRN("Set random addr disallowed, scan with identity");
			if (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_PUBLIC) {
				*own_addr_type = BT_HCI_OWN_ADDR_PUBLIC;
			} else {
				*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
			}

			return 0;
		} else if (err) {
			return err;
		}
```

相对本树大约在 `id.c` 第 1870 行附近。其它分支、其它文件不要动。

---

## 怎么打

OpenVela 根目录。

**覆盖（与对照 HEAD 一致时）：**

```bash
cp vendor/my_vendor/docs/pitch/replace/external/zblue/zblue/subsys/bluetooth/host/id.c \
   external/zblue/zblue/subsys/bluetooth/host/id.c
```

替换件是打过补丁的 **整份** `id.c`（约 2320 行）。上游若已改过同一文件的其它函数，不要覆盖，改打补丁或手工改这一处。

**打补丁：**

```bash
git -C external/zblue/zblue apply \
  ../../../vendor/my_vendor/docs/pitch/patches/zblue-id-scan-eacces.patch
```

`git apply --check` 可先看能否贴上。

---

## 不要改 zblue 的其它文件

| 文件 | 原因 |
|------|------|
| `subsys/bluetooth/host/hci_core.h` 里强行关掉 Ext Adv | 板级 `sf32lb52_bth4.c` 对 `LE_READ_LOCAL_FEATURES` 已无 Ext Adv bit；再加 `CONFIG_BT_EXT_ADV_LEGACY_SUPPORT=y` |
| 上游 `port/drivers/bluetooth/hci/h4.c` | 与本条无关：本板编译库里**没有**它（只有 Vela 的 `hci_h4.c` 与 `nuttx/drivers/serial/uart_bth4.c`），不需要抽换。读侧修复见 [ble-hci-h4-rx-reopen.md](ble-hci-h4-rx-reopen.md) |
| `subsys/bluetooth/host/scan.c` 加日志 | 调试用，不影响能否扫。**注意**：该文件另有一处板级必改（`bt_scan_softreset` 判空），见 [zblue-scan-stop-null-ctx.md](zblue-scan-stop-null-ctx.md) |

`hci_core.h` 不能靠 `-I` 覆盖：host 用 `"hci_core.h"`，先搜同目录。

---

## 改动前（上游 HEAD 原文，blob `e2e8f4824dfc`）

```c
int bt_id_set_scan_own_addr(struct bt_dev *hdev, bool active_scan, uint8_t *own_addr_type)
{
	int err;

	CHECKIF(own_addr_type == NULL) {
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_BT_PRIVACY)) {

		if (BT_FEAT_LE_PRIVACY(hdev->le.features)) {
			*own_addr_type = BT_HCI_OWN_ADDR_RPA_OR_RANDOM;
		} else {
			*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
		}

		err = bt_id_set_private_addr(hdev, BT_ID_DEFAULT);
		if (err == -EACCES && (atomic_test_bit(hdev->flags, BT_DEV_SCANNING) ||
				       atomic_test_bit(hdev->flags, BT_DEV_INITIATING))) {
			LOG_WRN("Set random addr failure ignored in scan/init state");

			return 0;
		} else if (err) {
			return err;
		}
	} else {
		/* Use NRPA unless identity has been explicitly requested
		 * (through Kconfig).
		 * Use same RPA as legacy advertiser if advertising.
		 */
		if (!IS_ENABLED(CONFIG_BT_SCAN_WITH_IDENTITY) &&
		    !is_adv_using_rand_addr(hdev)) {
			err = bt_id_set_private_addr(hdev, BT_ID_DEFAULT);
			if (err) {
				if (active_scan || !is_adv_using_rand_addr(hdev)) {
					return err;
				}

				LOG_WRN("Ignoring failure to set address for passive scan (%d)",
					err);
			}

			*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
		} else if (IS_ENABLED(CONFIG_BT_SCAN_WITH_IDENTITY)) {
			if (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_RANDOM) {
				/* If scanning with Identity Address we must set the
				 * random identity address for both active and passive
				 * scanner in order to receive adv reports that are
				 * directed towards this identity.
				 */
				err = set_random_address(hdev, &hdev->id_addr[BT_ID_DEFAULT].a);
				if (err) {
					return err;
				}

				*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
			} else if (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_PUBLIC) {
				*own_addr_type = BT_HCI_OWN_ADDR_PUBLIC;
			}
		}
	}

	return 0;
}
```

## 改动后（工作区/替换件原文，逐字一致）

```c
int bt_id_set_scan_own_addr(struct bt_dev *hdev, bool active_scan, uint8_t *own_addr_type)
{
	int err;

	CHECKIF(own_addr_type == NULL) {
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_BT_PRIVACY)) {

		if (BT_FEAT_LE_PRIVACY(hdev->le.features)) {
			*own_addr_type = BT_HCI_OWN_ADDR_RPA_OR_RANDOM;
		} else {
			*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
		}

		err = bt_id_set_private_addr(hdev, BT_ID_DEFAULT);
		if (err == -EACCES) {
			/* LE Set Random Address is disallowed while advertising,
			 * scanning, initiating, or (on some controllers) while a
			 * peripheral link is up.  Keep the current identity and
			 * still start the observer.
			 */
			LOG_WRN("Set random addr disallowed, scan with identity");
			if (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_PUBLIC) {
				*own_addr_type = BT_HCI_OWN_ADDR_PUBLIC;
			} else {
				*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
			}

			return 0;
		} else if (err) {
			return err;
		}
	} else {
		/* Use NRPA unless identity has been explicitly requested
		 * (through Kconfig).
		 * Use same RPA as legacy advertiser if advertising.
		 */
		if (!IS_ENABLED(CONFIG_BT_SCAN_WITH_IDENTITY) &&
		    !is_adv_using_rand_addr(hdev)) {
			err = bt_id_set_private_addr(hdev, BT_ID_DEFAULT);
			if (err) {
				if (active_scan || !is_adv_using_rand_addr(hdev)) {
					return err;
				}

				LOG_WRN("Ignoring failure to set address for passive scan (%d)",
					err);
			}

			*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
		} else if (IS_ENABLED(CONFIG_BT_SCAN_WITH_IDENTITY)) {
			if (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_RANDOM) {
				/* If scanning with Identity Address we must set the
				 * random identity address for both active and passive
				 * scanner in order to receive adv reports that are
				 * directed towards this identity.
				 */
				err = set_random_address(hdev, &hdev->id_addr[BT_ID_DEFAULT].a);
				if (err) {
					return err;
				}

				*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
			} else if (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_PUBLIC) {
				*own_addr_type = BT_HCI_OWN_ADDR_PUBLIC;
			}
		}
	}

	return 0;
}
```

- 语义：`-EACCES`（`LE Set Random Address` 在广播/扫描/发起或某些控制器的"外设链路已连"状态下被拒）
  不再只在 `SCANNING/INITIATING` 时被忽略，而是**一律按当前 identity 继续起观察者**；
  `own_addr_type` 跟着 identity 类型给（PUBLIC / RANDOM）。
- 其余分支（`CONFIG_BT_SCAN_WITH_IDENTITY` 等）未改。

---

## 验证

```bash
git -C external/zblue/zblue diff -- subsys/bluetooth/host/id.c
```

应只有 `bt_id_set_scan_own_addr()` 这一处。

实机：

1. 手机连上 Companion。
2. `test sensor scan`（或产品扫表）能出设备表。
3. 失败日志常为 `set scan own addr failed (-13)`，说明本补丁没打上或被 `repo sync` 冲掉。

`vela_override/zblue/` 里的 `h4.c` / `init.c` 抽换解决的是 H4 读卡死和双 workqueue，**替代不了** 这一处地址回退。

---

## 长期

`vela_override/CMakeLists.txt` 已注明「预留 `id.c`」。做成与 `h4.c` 相同的同名抽换后，不必再改 `external/zblue`。在那之前，每次整树 `repo sync` 都要重新打本补丁。
