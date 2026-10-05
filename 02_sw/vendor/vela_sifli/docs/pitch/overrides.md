# 抽换层索引（vela_override）

> 本文件是 [README.md](README.md) 的补充：那份记"上游必改件"（**会被 `repo sync` 冲掉**的改动），
> 这份记**抽换层**（不会被冲掉，但要知道"替的是谁、哪个版本"）。
>
> 权威来源是**各抽换件的文件头**（`vela_override/` 下每个 `.c` / `.py` 顶部那段"抽换件说明"）——
> 本文是从那里生成的快照；改了抽换件请重新生成，别只手改本表。

## 机制（为什么这些不需要在本目录留补丁）

`vela_override/CMakeLists.txt` 在构建时把目标里的**上游同名 `.c` 去掉**，改编本目录的同名文件；
`patch_*.py` 则是构建期**就地改写** zblue 的构建副本。所以上游 `nuttx` / `apps` /
`frameworks_bluetooth` / `zblue` 的 tree 保持干净 —— `git status` 里看不到它们，
`repo sync` 也收不走，补丁自然不必留在这里。**代价**：上游一旦改了这些文件，抽换件要么跟着改、
要么该退役，所以每个抽换件都记了写入时的上游 HEAD 与**该文件 blob**。

**快照日期**：2026-09-19

## 索引（20 条，19 个文件）

| 抽换件 | 类型 | 替的上游仓库 | 上游文件 | 文件 blob（12位） | 为什么抽换 |
|---|---|---|---|---|---|
| `bluetooth/sal_gatt_client_interface.c` | 同名替换 | `frameworks/connectivity/bluetooth` | `service/stacks/zephyr/sal_gatt_client_interface.c` | `a5bc28c7ed71` | keep_db / 溢出不拆链 |
| `bluetooth/sal_gatt_server_interface.c` | 同名替换 | `frameworks/connectivity/bluetooth` | `service/stacks/zephyr/sal_gatt_server_interface.c` | `4cbc9d365a1a` | CCC 写回调按写入字节/peer cfg 上报（不用 conn_index） |
| `bluetooth/sal_le_advertise_interface.c` | 同名替换 | `frameworks/connectivity/bluetooth` | `service/stacks/zephyr/sal_le_advertise_interface.c` | `9d4ba4033133` | SF32 走 legacy bt_le_adv_start（无 Ext Adv）；与 scan 共用跨角色 ticket 队列，等待有界 2 s |
| `bluetooth/sal_le_scan_interface.c` | 同名替换 | `frameworks/connectivity/bluetooth` | `service/stacks/zephyr/sal_le_scan_interface.c` | `24d8275d5d8b` | -EALREADY 重试、options=0；与 adv 共用跨角色 ticket 队列，等待有界 2 s |
| `bluetooth/scan_manager.c` | 同名替换 | `frameworks/connectivity/bluetooth` | `service/src/scan_manager.c` | `bf4ef311849d` | LOCAL 下不编 bt_socket.h |
| `freetype/ftsystem.c` | 同名替换 | `external/freetype/freetype` | `src/base/ftsystem.c` | `9beb7e245d2f` | FT heap 走 PSRAM |
| `fs/fs_fat32util.c` | 同名替换 | `nuttx` | `fs/fat/fs_fat32util.c` | `c03df449c07e` | FatSz16==0 当 FAT32（大簇 1GiB 卷） |
| `fs/fs_procfs_mount.c` | 同名替换 | `nuttx` | `fs/mount/fs_procfs_mount.c` | `86004c0e59e5` | df -h 用 uint64，避免 >=4GiB wrap |
| `mtd/ftl.c` | 同名替换 | `nuttx` | `drivers/mtd/ftl.c` | `89130f2c1894` | O_DIRECT 写成功返回扇区数（不是 leftover 0） |
| `sched/sem_post.c` | 同名替换 | `nuttx` | `sched/semaphore/sem_post.c` | `491012b9b144` | mutex holder≠tid 记 schedmon，不 DEBUGASSERT |
| `sched/sem_waitirq.c` | 同名替换 | `nuttx` | `sched/semaphore/sem_waitirq.c` | `70552158c26c` | waitobj==NULL 记 schedmon，不 DEBUGASSERT |
| `sched/wd_cancel.c` | 同名替换 | `nuttx` | `sched/wdog/wd_cancel.c` | `5056f617b123` | 未入链不 list_delete |
| `sched/wd_start.c` | 同名替换 | `nuttx` | `sched/wdog/wd_start.c` | `f9d8340186a9` | PSRAM waitdog 视为合法；断链才剪环，避免 wd_insert HardFault |
| `usbdev/mtp.c` | 同名替换 | `nuttx` | `drivers/usbdev/mtp.c` | `b97b5ec3e9cb` | iProduct / iSerialNumber 可写（MAC 后缀） |
| `zblue/init.c` | 同名替换 | `external/zblue/zblue` | `port/kernel/init.c` | `f17a4edb88fa` | z_sys_init 幂等（SAL BREDR+LE 都可能调用） |
| `zblue/long_wq.c` | 同名替换 | `external/zblue/zblue` | `subsys/bluetooth/host/long_wq.c` | `b7b6e3dc03b1` | 禁止第二条 BT LW WQ 共用 BSS 栈 |
| `zblue/system_work_q.c` | 同名替换 | `external/zblue/zblue` | `kernel/system_work_q.c` | `5cc26fd82e14` | 禁止第二条 sysworkq 共用 BSS 栈 |
| `zblue/patch_gatt_ccc_pool.py` | 构建期补丁脚本 | `external/zblue/zblue` | `subsys/bluetooth/host/gatt.c` | `8e31d1cfa353` | 断开释放 CCC 槽 + 池满时回收已断开的 peer；CCC 池金丝雀 + 30 s 校验；CCC 写入/复位的原始字节取证；地址对不上时不把还在用的 CCC 打成 0 |
| `zblue/patch_hci_core.py` | 构建期补丁脚本 | `external/zblue/zblue` | `subsys/bluetooth/host/hci_core.c` | `ab23d63b2da4` | 超时返回 -ETIMEDOUT；skip_sync 轮询退出不可中断等待 |
| `zblue/patch_net_buf_timeout.py` | 构建期补丁脚本 | `external/zblue/zblue` | `subsys/bluetooth/host/buf.c` | `b7ed19303be2` | 缓冲区 K_FOREVER 改成有上限的轮询（否则卡死不可杀） |
| `↑同上` | ↑ | `external/zblue/zblue` | `subsys/bluetooth/host/hci_core.c` | `ab23d63b2da4` | 同上（控制流侧） |

## 校验这张表还准不准（每次 `repo sync` 之后跑一遍）

思路：读出每个抽换件文件头里记的 (仓库, 路径, blob)，与 `git rev-parse HEAD:<路径>` 对比。

```bash
cd /home/jinsc/SDK/vela/openvela && python3 - <<'EOF'
import pathlib, re, subprocess
VO = pathlib.Path('vendor/my_vendor/boards/sf32lb52/my_vendor/vela_override')
pat = re.compile(r'(?:替的是上游|改的是)\s*:\s*([\w./-]+?)\s*/\s*([\w./-]+\.(?:c|h))'
                 r'(?:\n[^\n]*){0,2}?\n[^\n]*?blob\s*:\s*([0-9a-f]{12,40})')
for f in sorted(list(VO.rglob('*.c')) + list(VO.rglob('*.py'))):
    for repo, path, blob in pat.findall(f.read_text(encoding='utf-8')):
        real = subprocess.run(['git', '-C', repo, 'rev-parse', 'HEAD:' + path],
                              capture_output=True, text=True).stdout.strip()
        if not real.startswith(blob[:12]):
            print('  变了:', f.name, repo + ':' + path, blob[:12], '->', real[:12] or '(取不到)')
print('  没输出 = 全部一致')
EOF
```

- **一致** ⇒ 抽换件与上游同步，照旧；
- **变了** ⇒ 先看上游这次改了什么（`git -C <repo> diff <blob> HEAD -- <path>`），再决定：
  **跟改**（更新抽换件与文件头）／**退役**（上游已修同一件事，删掉抽换件并在
  `vela_override/CMakeLists.txt` 表里划掉）。

## 重新生成本表

本表由各文件头的"抽换件说明"块生成 —— 改完抽换件后按上面脚本的思路重生成这张表；
**不要只改表、不改文件头**，文件头才是给人看的一手信息。
