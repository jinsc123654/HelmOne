# GPX 传感器扩展示例

演示 **gpx_ext_sensor**（心率/踏频/风向）+ **句柄化多录制器**（3 路并行录 → 依次 decode）。

> 扩展类型与 register 在组件内 `gpx_ext_sensor.h`；例程只负责采样与 push，不单独声明扩展。

> `gpx example` 在 NSH 中**同步执行**（约 5 s×3 路录制 + decode），会占用当前终端直到结束。

## 前置条件

- 文件系统可写，例如 LittleFS 挂载在 `/mnt/lfs`
- Kconfig：`CONFIG_MYVENDOR_GPX=y`（可选 `MYVENDOR_GPX_EXAMPLE_*` 调 demo 任务栈/优先级）

## 测试步骤（推荐）

### 一键全流程

```bash
nsh> gpx example
```

按顺序自动执行并打印说明：

| 步骤 | 动作 |
|------|------|
| [1/6] | 注册传感器扩展 (`gpx_ext_sensor_register`) |
| [2/6] | 创建 3 个独立录制器（各 1 线程） |
| [3/6] | 启动 3 路录制（随机起点各不同） |
| [4/6] | 同时录制 5 s（每秒每路 push 1 点） |
| [5/6] | stop + release，打印 written/dropped |
| [6/6] | 依次 decode 三个文件 |

输出文件：

```text
/mnt/lfs/custom_demo_1.gpx
/mnt/lfs/custom_demo_2.gpx
/mnt/lfs/custom_demo_3.gpx
```

指定每路录制秒数：

```bash
gpx example demo 10
```

### 预期 decode 输出

每点一行，含标准字段 + 扩展字段，例如：

```text
gpx example: --- decode /mnt/lfs/custom_demo_1.gpx ---
  #1 lat=39.908001 lon=116.391998 ele=48.00 time=2026-06-16T… hr=120 cad=80 wind=0
  #2 …
gpx example: decoded 5 points from /mnt/lfs/custom_demo_1.gpx
```

### 验证 GPX 文件内容（可选）

```bash
head -30 /mnt/lfs/custom_demo_1.gpx
```

应看到 `<gpxtpx:hr>`、`<gpxtpx:cad>`、`<gpxtpx:speed>` 及 `<gpxpx:PowerInWatts>` 一类片段。

### 通过标准

- 三个 `.gpx` 文件均存在且非 0 字节
- 每路 decode 点数 = 录制秒数（默认 5）
- 每行含 `hr=`、`cad=`、`speed=`、`power=`
- `[5/6]` 中 `written` 与点数一致，`dropped=0`（队列未满时）

## 高级子命令（分步 / 后台）

```bash
# 后台单路录制（task_spawn，NSH 立即返回）
gpx example record /mnt/lfs/custom_demo.gpx 10

gpx example status          # 等待 running → idle

# 仅 decode（自动 register 扩展）
gpx example decode /mnt/lfs/custom_demo.gpx
```

> 不要用 `gpx decode` 查看上述文件的扩展字段（未注册 parse/print）。

## 扩展字段

例程使用组件内 **`gpx_sensor_ext_t`**（`gpx_ext_sensor.h`），每点可含：

| 字段 | GPX XML | 含义 |
|------|---------|------|
| `hr` | `<gpxtpx:hr>` | 心率 (bpm) |
| `cad` | `<gpxtpx:cad>` | 踏频 (rpm) |
| `speed_mps` | `<gpxtpx:speed>` | 速度 (**m/s**；gpxtpx **v2** 才定义 speed) |
| `power_w` | `<gpxpx:PowerInWatts>` | 功率 (W；Garmin PowerExtension v1) |

扩展里只放有 schema 定义的字段：曾经的风向（自定义 `mybike:windDir`）已撤掉 ——
Garmin 的 gpxtpx/gpxx 与 gpxdata 都没有风向元素。

注册/注销：`gpx_ext_sensor_register()` / `gpx_ext_sensor_unregister()`。

## 录制线程是怎么工作的

例程底层统一使用 **`gpx_record`**（`gpx_record.c`）。每个 `gpx_record_create()` 对应 **1 个录制工作线程**、**2 个环形队列**：

| 队列 | 作用 | 谁写入 | 谁消费 |
|------|------|--------|--------|
| **cmd 队列** | start / pause / resume / stop / release | 调用方（NSH、GNSS 任务等） | 录制线程 |
| **data 队列** | 待写盘的 `gpx_point_t` | 调用方 `gpx_record_push()` | 录制线程批写 |

线程与调用方通过 **`gpx_port_mutex`** 保护队列，通过 **`gpx_port_sem`** 唤醒（push 满 N 点或投递 cmd 时 `sem_post`）。

```text
  调用方（gpx example / 业务任务）
        │
        │  gpx_record_start / push / pause / stop / release
        ▼
  ┌─────────────┐     cmd 队列      ┌──────────────────┐
  │  API 层     │ ───────────────► │  gpx_record 线程  │
  │ gpx_record  │     data 队列     │  process cmd     │
  └─────────────┘ ◄─────────────── │  batch format    │
        ▲                          │  gpx_writer 写盘 │
        │  join (release)          └────────┬─────────┘
        └───────────────────────────────────┘
                                           ▼
                                    GPX 文件 (LittleFS)
```

### 1. 启动线程（create）

`gpx_record_create()` 做三件事：

1. 按 `batch_size` / `queue_depth` 分配 data 队列、cmd 队列、批写缓冲区 `fmt_buf`
2. 初始化 mutex + sem(0)
3. **`gpx_port_thread_create()`** 拉起 `gpx_record_thread`，线程进入 `sem_wait` 阻塞

此时线程 **idle**：尚未 open 文件，**不能 push**（push 会返回 `-EAGAIN`）。

一键 demo 对应 `[2/6]`：

```c
gpx_record_cfg_t cfg = { .batch_size = 10, .queue_depth = 0 };
gpx_record_create(&cfg, &rec);   /* 每路各 create 一次 → 3 线程 */
```

后台子命令 `gpx example record` 则在 **`task_spawn` 拉起的 `gpx_example_record_task`** 里同样调用 `gpx_record_create()`（见 `gpx_example_record.c`）。

### 2. 开始录制（start）

`gpx_record_start(rec, path, &meta)` **不直接 open 文件**，而是：

1. 构造 `GPX_RECORD_CMD_START` 消息（路径 + meta 字符串拷贝进 cmd）
2. **push 到 cmd 队列**，`sem_post` 唤醒线程
3. 轮询等待 `rec->recording == true`（最多约 1 s）

录制线程被唤醒后：

1. 从 cmd 队列取出 START
2. `gpx_writer_open()` 写 GPX 头 + `<trkseg>`
3. 置 `recording = true`，此后 **可以 push**

一键 demo 对应 `[3/6]`：3 个 recorder 各 `start` 一个 `.gpx` 文件。

### 3. 写入数据（push）

`gpx_record_push(rec, &point)` 在 **任意上下文**（NSH、GNSS 回调、demo 循环）调用：

1. 检查 `recording`；未 start 则 `-EAGAIN`
2. **`gpx_ext_point_clone_ext()`** 深拷贝扩展 payload（避免栈上结构体失效）
3. 点入 **data 队列**；满则 `-ENOSPC`，统计 `points_dropped`
4. 若队列中点数 **≥ batch_size N**，`sem_post` 唤醒线程刷盘

录制线程侧（`gpx_record_flush_if_ready_locked`）：

1. 从 data 队列 **peek N 点**
2. `gpx_writer_format_trkpt()` 格式化为 XML 写入 `fmt_buf`
3. **`gpx_writer_write_bytes()`** 一次 `write()` 落盘
4. `advance` 消费 N 点，更新 `points_written` / `batches_written`

一键 demo 对应 `[4/6]`：外层 `for (t=0; t<seconds; t++)` 内对 3 路各 `push` 一点，再 `sleep(1)`。

> demo 默认 `batch_size = seconds`（ capped 到 Kconfig 默认 32），10 s 录制时 **10 点一批写 1 次**，故统计里常见 `batches=1`。

### 4. 暂停 / 恢复（pause / resume）

例程一键 demo **未调用** pause，但产品代码可用：

```c
gpx_record_pause(rec);    /* 暂停刷盘：data 仍可 push 入队 */
/* … 继续 push 或停止采集 … */
gpx_record_resume(rec);     /* 恢复刷盘，积压点按 N 批写 */
```

机制：cmd 队列投递 `PAUSE` / `RESUME`，线程置 `paused` 标志。**暂停期间不调用 flush**，但 **push 仍入队**；恢复后继续批写。

### 5. 结束录制（stop）

`gpx_record_stop(rec)`：

1. cmd 队列投递 `STOP`，唤醒线程
2. 线程执行 **close session**：
   - 刷写 data 队列中 **剩余不足 N 的点**（尾批）
   - `gpx_writer_close()` 写 `</trkseg></trk></gpx>` 并 close fd
   - `recording = false`
3. 调用方轮询直到 `recording == false`

**stop 后线程仍存活**，句柄可再次 `start` 录新文件。

一键 demo 对应 `[5/6]` 的第一步：`gpx_record_stop(rec[i])`。

### 6. 销毁线程（release）

`gpx_record_release(&rec)`：

1. cmd 队列投递 `RELEASE`
2. 若仍在录，先走 stop 逻辑（刷盘 + close）
3. 线程退出循环，`thread_alive = false`，`gpx_port_thread_join()`
4. 释放队列、fmt_buf、mutex、sem，**free 句柄**，`*rec = NULL`

一键 demo 在 stop 之后对每个 recorder 调用 `gpx_record_release()`。

### 两种例程入口对比

| 方式 | 入口 | 录制线程谁创建 | NSH 是否阻塞 |
|------|------|----------------|--------------|
| **一键 demo** | `gpx example` / `demo 10` | NSH 进程内直接 `gpx_record_create` ×3 | 是（录完 + decode 才返回） |
| **后台 record** | `gpx example record …` | `task_spawn` → `gpx_example_record_task` 内 create ×1 | 否（立即回到 `nsh>`） |

后台 task 内典型顺序与 demo 单路相同：

```text
gpx_ext_sensor_register()
  → gpx_record_create()
  → gpx_record_start()
  → 循环: gpx_example_fill_point() + gpx_record_push() + sleep(1)
  → gpx_record_stop()
  → gpx_record_release()
  → gpx_ext_sensor_unregister()
```

### 集成到自己代码时的最小模板

```c
gpx_record_t *rec = NULL;
gpx_record_cfg_t cfg = { .batch_size = 32, .queue_depth = 0 };

gpx_record_create(&cfg, &rec);
gpx_record_start(rec, "/mnt/lfs/Track/ride.gpx", &meta);

/* 采集循环（类型见 gpx_types.h / gpx_ext_sensor.h；产品里主要改 fill_point） */
int slot = gpx_ext_sensor_register();
gpx_point_t point;
gpx_sensor_ext_t sensor;

gpx_example_fill_point(&point, &sensor, &origin, index, slot);
gpx_record_push(rec, &point);

/* 结束 */
gpx_record_stop(rec);
gpx_record_release(&rec);
```

扩展需先 `gpx_ext_sensor_register()`；`gpx_example_fill_point()` 设置 `point` 与 `ext_data[slot]`。

## 源码

| 文件 | 说明 |
|------|------|
| `gpx_example.c` | 一键 demo + 子命令 |
| `gpx_example_record.h` | 例程 API 声明（含 `gpx_point_t` / `gpx_sensor_ext_t`） |
| `gpx_example_record.c` | `fill_point` 实现 + 后台 record task |
| `../gpx_ext_sensor.c` | 传感器扩展 format/parse/print |

Kconfig：`MYVENDOR_GPX_EXAMPLE_STACKSIZE` / `MYVENDOR_GPX_EXAMPLE_PRIORITY`
