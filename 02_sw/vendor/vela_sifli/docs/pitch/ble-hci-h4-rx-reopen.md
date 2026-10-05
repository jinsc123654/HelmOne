# Vela 低层改动：HCI H4 读错误有界重开（不再永久丢 RX）

> 本文件属于「上游必改件」记录（见 [README.md](README.md)）：这条改动**不在 vendor 编译单元里**，
> 改的是 Vela 自己的框架文件，`repo sync` 之后必须重新应用。

## 改哪个文件

```
apps/frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c
```

本板实际编译的 h4 实现就是上面这个 Vela 文件，加上 `nuttx/drivers/serial/uart_bth4.c` 与
`chips/sf32lb52/sf32lb52_bth4.c`（`compile_commands.json` 可查）。

## 改哪个组件、基于哪个版本（**换版本前一定先对这张表**）

| 项 | 值 |
|---|---|
| 仓库 | `frameworks/connectivity/bluetooth`（独立 repo 工程 `frameworks_bluetooth`；`apps/frameworks` 只是指向 `../frameworks/` 的符号链接） |
| 写入时的 HEAD | `43945bc1d13f3494a79e3e1d79ca8312c4c0f6a0`（`43945bc1`，2026-04-22 `chore: sync .github…`） |
| 文件 | `service/stacks/zephyr/hci_h4.c` |
| 该文件在 HEAD 的 blob | `580de33d9a70555704cc6c9d83ade59aa9fceee1` |
| 补丁写于 | 2026-09-18（2026-09-19 补齐整段函数对照） |

```bash
git -C frameworks/connectivity/bluetooth rev-parse HEAD
git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/stacks/zephyr/hci_h4.c  # 应等于上表 blob
git -C frameworks/connectivity/bluetooth apply --check \
    ../../vendor/my_vendor/docs/pitch/patches/vela-hci-h4-rx-reopen.patch
```

## 原因

`bt_sal_hci_transport_recv()`（service-loop 里由 poll 回调驱动的接收函数）在读失败时：

```c
len = read(h4->fd, frame + frame_size, sizeof(frame) - frame_size);
if (len < 0) {
    BT_LOGE("Reading hci failed, errno %d", errno);
    hci_remove_recv(NULL);      /* ← 把 service-loop poll 摘掉 */
    close(h4->fd);
    h4->fd = -1;
    return;                     /* ← 回调路径整体死掉，再没人调它 */
}
```

后果：**一次瞬时读错误（设备被复位过、FIFO 抖动）就永久失去 RX**。控制器之后再说什么都没人听，
上层只能靠把整栈复位来救（adapter cycle → LCPU 复位 → 任务重建）—— 代价 12 s 起，而且会把手机和
心率计一起踢掉。现场表现是"链路莫名静默"，日志里看不出源头（`link stale` / `cmdsilent` 只是结果）。

另外：`EAGAIN` 也走同一条路 —— 一次"暂时没数据"就把 RX 永久打死。

## 改动前（上游 HEAD 原文，blob `580de33d9a7`）

```c
static void bt_sal_hci_transport_recv(void)
{
    struct h4_data* h4 = bt_dev->data;
    static uint8_t frame[1026];
    struct net_buf* buf;
    size_t buf_tailroom;
    size_t buf_add_len;
    ssize_t len;
    const uint8_t* frame_start = frame;
    static ssize_t frame_size = 0;

    len = read(h4->fd, frame + frame_size, sizeof(frame) - frame_size);
    if (len < 0) {
        BT_LOGE("Reading hci failed, errno %d", errno);
        hci_remove_recv(NULL);          /* ← 摘掉 poll：RX 永久死亡 */
        close(h4->fd);
        h4->fd = -1;
        return;                         /* ← EAGAIN 也走这条路 */
    }

    frame_size += len;
    /* … 下面是正常的组包/分发，未改 … */
}
```

（文件里没有 `h4_rx_reopen`、也没有 `#include <syslog.h>`、没有 `hci_poll_recv` 的前置声明。）

## 改动后（完整函数，可直接照抄）

新增函数（放在 `bt_sal_hci_transport_recv` 之前，`hci_packet_complete` 之后）：

```c
/**
 * @brief 有界重开 HCI 传输：重开字符设备 + 重新注册 service-loop poll。
 *
 * @details 为什么需要它：`bt_sal_hci_transport_recv()` 一旦读失败就
 *          `hci_remove_recv()` + `close()` + `return`，**poll 被摘掉、回调路径
 *          整体死掉** —— 之后控制器说什么都没人听，只能靠上层把整栈复位
 *          （adapter cycle → LCPU 复位 → 任务重建），代价 12 s 起、还要踢掉所有
 *          链路。而多数读错误是瞬时的（设备被复位过、FIFO 抖动），重开一个字符
 *          设备 + 重注册 poll 就够。@note 本函数跑在 service loop 里（poll 回调
 *          里调来），所以在同一个上下文里摘/注册 poll 是安全的；重试之间只做
 *          **短** usleep（最多 5×200 ms），不把 service loop 挂住太久。
 *
 * @return 0 = 重开成功（fd 与 poll 都已恢复）；负值 = 放弃（调用方再摘回调）。
 */
static int h4_rx_reopen(struct h4_data* h4)
{
    char dev_name[32];
    unsigned tries = 0;
    int fd;

    if (bt_dev == NULL || bt_dev->name == NULL) {
        return -EINVAL;
    }

    if (snprintf(dev_name, sizeof(dev_name), "%s", bt_dev->name) < 0) {
        return -EINVAL;
    }

    while (tries < 5u) {
        fd = open(dev_name, O_RDWR | O_BINARY | O_CLOEXEC);
        if (fd >= 0) {
            h4->fd = fd;

            if (hci_handle != NULL) {
                service_loop_remove_poll(hci_handle);
                hci_handle = NULL;
            }

            hci_handle = service_loop_poll_fd(fd, POLL_READABLE, hci_poll_recv, NULL);
            if (hci_handle == NULL) {
                close(fd);
                h4->fd = -1;
                syslog(LOG_WARNING, "H4: rx reopen poll add failed fd:%d\n", fd);
                return -1;
            }

            syslog(LOG_WARNING, "H4: rx reopen #%u ok (%s) fd:%d\n",
                   tries + 1u, dev_name, fd);
            return 0;
        }

        tries++;
        if (tries < 5u) {
            usleep(200 * 1000);
        }
    }

    syslog(LOG_WARNING, "H4: rx reopen gave up after %u tries, errno %d\n",
           tries, errno);
    return -1;
}
```

`bt_sal_hci_transport_recv()` —— 只有读错误分支变了，其余原样：

```c
static void bt_sal_hci_transport_recv(void)
{
    struct h4_data* h4 = bt_dev->data;
    static uint8_t frame[1026];
    struct net_buf* buf;
    size_t buf_tailroom;
    size_t buf_add_len;
    ssize_t len;
    const uint8_t* frame_start = frame;
    static ssize_t frame_size = 0;

    len = read(h4->fd, frame + frame_size, sizeof(frame) - frame_size);
    if (len < 0) {
        /* 瞬时错误：**什么都不做**，poll 还在，下一轮可读会再进来。
         * （原来 EAGAIN 也走下面那条路：摘回调 + 关设备 —— 一次"暂时没数据"
         *   就把 RX 永久打死。） */
        if (errno == EINTR || errno == EAGAIN) {
            return;
        }

        BT_LOGE("Reading hci failed, errno %d", errno);
        close(h4->fd);
        h4->fd = -1;
        frame_size = 0;   /* 半帧丢掉，重新同步 */

        /* **先重开，实在不行才摘回调。** 摘回调 = 放弃 RX，只有上层复位整栈
         * 才能救回来（见 h4_rx_reopen 的说明）。 */
        if (h4_rx_reopen(h4) != 0) {
            hci_remove_recv(NULL);
        }
        return;
    }

    frame_size += len;

    while (frame_size > 0) {
        const uint8_t* buf_add;
        const uint8_t packet_type = frame_start[0];
        const int32_t decoded_len = hci_packet_complete(frame_start, frame_size);

        if (decoded_len == -1) {
            BT_LOGE("HCI Packet type is invalid, length could not be decoded");
            frame_size = 0; /* Drop buffer */
            break;
        }

        if (decoded_len == 0) {
            if (frame_size == sizeof(frame)) {
                BT_LOGE("HCI Packet is too big for frame");
                frame_size = 0; /* Drop buffer */
                break;
            }
            if (frame_start != frame) {
                memmove(frame, frame_start, frame_size);
            }
            /* Read more */
            break;
        }

        buf_add = frame_start + sizeof(packet_type);
        buf_add_len = decoded_len - sizeof(packet_type);

        buf = get_rx(frame_start);

        frame_size -= decoded_len;
        frame_start += decoded_len;

        if (!buf) {
            BT_LOGD("Discard adv report due to insufficient buf");
            continue;
        }

        buf_tailroom = net_buf_tailroom(buf);
        if (buf_tailroom < buf_add_len) {
            BT_LOGE("Not enough space in buffer %zu/%zu", buf_add_len,
                buf_tailroom);
            net_buf_unref(buf);
            continue;
        }

        net_buf_add_mem(buf, buf_add, buf_add_len);

        h4_data_dump("BT RX", packet_type, buf->data, buf_add_len);
        h4->recv(bt_dev, buf, h4->hci_data);
    }
}
```

另外两行必须一起改（否则编译不过）：文件头部加 `#include <syslog.h>`，
并在 `static void hci_remove_recv(void* data);` 那一行下面加前置声明
`static void hci_poll_recv(service_poll_t* poll, int revent, void* userdata);`。

要点与取舍：

- 本函数跑在 **service loop 里**（poll 回调），所以在同一上下文里摘/注册 poll 是安全的，
  不需要跨线程调用。
- 重试之间的 `usleep` 只做 **短** 的（≤5×200 ms），避免把 service loop 挂住太久；超过就放弃，
  让上层那套重锤接手。
- 日志用 `syslog(LOG_WARNING, …)` 而不是 `BT_LOGE`：**这个文件里的 `BT_LOGE` 在本配置下是空的**
  （编译不进字符串），用它等于这条修复没有任何可见输出。

## 验证

- 正常情况下**不该**看到任何 `H4: rx reopen` 行。
- 出现 `H4: rx reopen #N ok` ⇒ 传输抖过并自愈（链路不掉、无需整栈复位）；
- 反复出现 ⇒ 传输在持续抖动，配合 `sf32lb52 bt stall … rxage/txage` 能看到时间线；
- 出现 `H4: rx reopen gave up after 5 tries` ⇒ 传输真的死了，此时才轮到
  `BLE recovery escalate` / 任务重建那一级。

## 回退

把 `h4_rx_reopen()` 与错误分支改回"`hci_remove_recv(NULL)` + `close` + `return`"即可；
或用该文件所在仓库的 git 工作区回退单个文件（`git -C frameworks/connectivity/bluetooth
checkout -- service/stacks/zephyr/hci_h4.c`）。改动**只在这一处**，
没有触碰 zblue 子模块与 NuttX 源码树。

## 源码留在哪（2026-09-18 补齐）

- 整文件覆盖件：`replace/frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c`
- 补丁：`patches/vela-hci-h4-rx-reopen.patch`（仓库根是 `frameworks/connectivity/bluetooth`）
- 应用/确认命令：见 [README.md](README.md) 的「hci_h4.c 怎么应用」一节

**路径别搞错**：`apps/frameworks` 是指向 `../frameworks/` 的符号链接，两者是同一个文件；
本文件所在的是独立仓库 `frameworks_bluetooth`（`git -C frameworks/connectivity/bluetooth`），
不是 `apps` 仓库。
