# 录制器句柄（`gpx_record_t`）

本文说明 **`gpx_record` 句柄如何创建、使用与销毁**。API 声明见 `gpx_record.h`；例程用法见 [example/README.md](../example/README.md)。

---

## 1. 句柄是什么

`gpx_record_t` 是 **不透明指针**（`typedef struct gpx_record gpx_record_t`）。每个句柄在内部独占：

| 资源 | 说明 |
|------|------|
| **1 个工作线程** | `gpx_record_thread`，经 `gpx_port_thread_create()` 创建 |
| **cmd 队列** | 投递 start / pause / resume / stop / release |
| **data 队列** | 缓存待写盘的 `gpx_point_t` |
| **fmt_buf** | 批写 XML 缓冲区（`batch_size × gpx_writer_trkpt_fmt_max()`） |
| **gpx_writer_t** | 当前 GPX 文件 fd 与 open 状态 |
| **mutex + sem** | 队列保护与线程唤醒 |

调用方 **不直接访问** 上述成员，仅通过 `gpx_record_*` API 与句柄交互。

**多句柄**：`gpx_record_create()` 可多次调用，得到多个独立句柄（多个线程、多个文件并行录）。例程 `gpx example demo` 即 create ×3。

---

## 2. 生命周期状态

```text
                    gpx_record_create()
                            │
                            ▼
                    ┌───────────────┐
                    │  idle         │  线程已启动，未 open 文件
                    │  recording=0  │  push → -EAGAIN
                    └───────┬───────┘
                            │ gpx_record_start()
                            ▼
                    ┌───────────────┐
         ┌─────────│  recording    │◄────────┐
         │         │  recording=1  │         │
         │         └───────┬───────┘         │
         │    pause/resume │                 │ resume
         │         ┌───────▼───────┐         │
         │         │  paused       │─────────┘
         │         │  仍 push 入队  │
         │         └───────┬───────┘
         │                 │ gpx_record_stop()
         │                 ▼
         │         ┌───────────────┐
         └────────►│  idle         │  文件已 close，线程仍存活
                   │  recording=0  │  可再次 start 新文件
                   └───────┬───────┘
                           │ gpx_record_release()
                           ▼
                    ┌───────────────┐
                    │  released     │  join 线程，free 句柄
                    │  *rec = NULL  │
                    └───────────────┘
```

---

## 3. 创建句柄（create）

### 3.1 配置

在 **create** 时传入 `gpx_record_cfg_t`（不是 start）：

```c
typedef struct gpx_record_cfg {
  unsigned batch_size;   /* N：每满 N 点批写一次；须 > 0 */
  unsigned queue_depth;  /* data 环形容量；0 = ceil(N × 1.2) */
} gpx_record_cfg_t;
```

| 字段 | 说明 |
|------|------|
| `batch_size` | 批写粒度；`cfg == NULL` 或 0 时用 Kconfig `MYVENDOR_GPX_DEFAULT_BATCH_SIZE` |
| `queue_depth` | 须 ≥ `batch_size` 且 ≥ 2；0 时自动 `gpx_port_queue_depth_for_batch(N)` |

### 3.2 调用

```c
gpx_record_t *rec = NULL;
gpx_record_cfg_t cfg = {
  .batch_size = 32,
  .queue_depth = 0,   /* 自动 ≈ 38 */
};

int ret = gpx_record_create(&cfg, &rec);
if (ret != 0) { /* -EINVAL / -ENOMEM / 线程创建失败 */ }
```

**create 内部顺序**：

1. 解析 cfg，分配 `struct gpx_record`
2. 分配 data/cmd 队列 storage + `fmt_buf`
3. 初始化 mutex、sem(0)
4. 启动工作线程 → 线程阻塞在 `sem_wait`

此时 **`gpx_record_is_alive(rec) == true`**，**`gpx_record_is_recording(rec) == false`**。

---

## 4. 开始一次录制会话（start）

```c
gpx_meta_t meta = {
  .creator = "Helm One",
  .meta_name = "ride",
  .meta_desc = "",
  .track_name = "Morning loop",
  .track_desc = "",
};

ret = gpx_record_start(rec, "/mnt/lfs/Track/ride.gpx", &meta);
```

| 返回值 | 含义 |
|--------|------|
| `0` | 线程已 open 文件，`recording = true` |
| `-EBUSY` | 已在录制中 |
| `-ETIMEDOUT` | cmd 处理超时（约 1 s） |
| 其他负值 | cmd 队列满等 |

**机制**：API 将 START 消息 **push 到 cmd 队列** 并唤醒线程；线程执行 `gpx_writer_open()`，将 meta 字符串 **拷贝到句柄内部 buffer**（调用方 meta 指针此后可释放）。

**start 之后**方可 `gpx_record_push()`。

---

## 5. 写入轨迹点（push）

```c
gpx_point_t point = { /* lat/lon/ele/time … */ };

/* 若有扩展：先 gpx_ext_register，再绑定 ext_data[slot] / ext_mask */
point.ext_data[slot] = &my_ext;
point.ext_mask = (1u << slot);

ret = gpx_record_push(rec, &point);
```

| 返回值 | 含义 |
|--------|------|
| `0` | 入队成功 |
| `-EAGAIN` | 未 start |
| `-ENOSPC` | data 队列满，`points_dropped++` |
| 其他 | 扩展 clone 失败等 |

**机制**：

1. 对扩展 payload **深拷贝**（`gpx_ext_point_clone_ext`），避免调用方栈变量失效
2. 点写入 **data 队列**
3. 队列点数 ≥ N 时唤醒线程，线程 **peek N 点 → 格式化 → 一次 write()**

`push` 可在 **任意任务上下文** 调用（GNSS 回调、NSH、worker 线程），内部已加锁。

---

## 6. 暂停与恢复（pause / resume）

```c
gpx_record_pause(rec);   /* 暂停刷盘，data 仍可 push */
/* … */
gpx_record_resume(rec);  /* 恢复批写，消化积压点 */
```

暂停期间 **不 flush**，但 **push 仍入队**；适合「采集不停、写盘暂缓」场景。例程一键 demo 未使用 pause。

查询状态：

```c
gpx_record_runtime_t rt;
gpx_record_get_runtime(rec, &rt);
/* rt.recording, rt.paused, rt.batch_size, rt.queue_depth */
```

---

## 7. 结束会话（stop）

```c
gpx_record_stop(rec);
```

1. 投递 STOP 命令
2. 线程刷写 **尾批**（不足 N 的剩余点）
3. `gpx_writer_close()` 写 footer 并 close fd
4. `recording = false`

**stop 不销毁线程**，句柄可再次 `start` 录下一个文件。

统计：

```c
gpx_record_stats_t stats;
gpx_record_get_stats(rec, &stats);
/* stats.points_written, points_dropped, batches_written */
```

---

## 8. 销毁句柄（release）

```c
gpx_record_release(&rec);   /* 调用后 rec == NULL */
```

1. 投递 RELEASE（若仍在录则等价 stop + 退出）
2. **`gpx_port_thread_join()`** 等待工作线程结束
3. 释放队列、fmt_buf、mutex、sem 与句柄本身

**release 之后不得再使用该指针**。

---

## 9. 完整用法示例

### 9.1 单次骑行

```c
gpx_record_t *rec = NULL;
gpx_record_cfg_t cfg = { .batch_size = 32, .queue_depth = 0 };

gpx_record_create(&cfg, &rec);
gpx_record_start(rec, "/mnt/lfs/Track/ride.gpx", &meta);

while (采集进行中) {
  gpx_record_push(rec, &point);
}

gpx_record_stop(rec);
gpx_record_release(&rec);
```

### 9.2 多文件并行（与 `gpx example demo` 相同模式）

```c
gpx_record_t *rec[3] = { NULL };
for (i = 0; i < 3; i++)
  gpx_record_create(&cfg, &rec[i]);

for (i = 0; i < 3; i++)
  gpx_record_start(rec[i], paths[i], &meta);

/* 同一循环内对 rec[0..2] 分别 push */

for (i = 0; i < 3; i++) {
  gpx_record_stop(rec[i]);
  gpx_record_release(&rec[i]);
}
```

### 9.3 与 NSH 例程的对应

| 例程步骤 | API |
|----------|-----|
| `gpx example` [2/6] | `gpx_record_create` ×3 |
| [3/6] | `gpx_record_start` ×3 |
| [4/6] | `gpx_record_push` |
| [5/6] | `gpx_record_stop` → `gpx_record_release` |

`gpx example record` 在独立 NuttX 任务中执行 **9.1 单路** 流程（见 `gpx_example_record.c`）。

---

## 10. 设计约束与注意点

1. **一个句柄同一时刻只 open 一个 GPX 文件**；换文件需 `stop` 再 `start`（或新 create 新句柄）。
2. **push 非阻塞**；队列满即丢点并计数，不阻塞 GNSS 回调。
3. **meta / 路径字符串**：start 时拷贝进句柄；push 的扩展数据也会 clone。
4. **线程栈**：Kconfig `MYVENDOR_GPX_RECORD_STACKSIZE`（默认 8192）。
5. **掉电**：未 flush 的 data 队列内点仍在 RAM，见 [design.md §5.5](design.md#55-掉电与数据安全)。

---

## 11. API 速查

| API | 作用 |
|-----|------|
| `gpx_record_create` | 分配资源 + 启动 idle 线程 |
| `gpx_record_start` | open GPX，开始会话 |
| `gpx_record_push` | 入队轨迹点 |
| `gpx_record_pause` / `resume` | 暂停 / 恢复刷盘 |
| `gpx_record_stop` | 尾批 + close，线程存活 |
| `gpx_record_release` | join + free 句柄 |
| `gpx_record_is_recording` | 是否在 start…stop 之间 |
| `gpx_record_is_alive` | 线程是否未 release |
| `gpx_record_get_stats` | written / dropped / batches |
| `gpx_record_get_runtime` | batch、queue、recording、paused |
