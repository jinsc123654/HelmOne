# GPX 组件

独立于 bicycle GUI 的 **GPX 编码 / 解码 / 后台记录线程**（纯 C，POSIX 文件 I/O）。

**设计文档**：[doc/design.md](doc/design.md) · [doc/gpx_format.md](doc/gpx_format.md)

## 模块

| 文件 | 作用 |
|------|------|
| `gpx_types.h` | 轨迹点、时间、metadata |
| `gpx_port.c/h` | 平台抽象：堆、队列、文件 I/O、线程/同步 |
| `gpx_writer.c` | GPX 编码 + 批量格式化 |
| `gpx_reader.c` | GPX 流式解码（`gpx_reader_next`） |
| `gpx_decode.c` | 句柄式解码（open/read/close；stride/limit/batch） |
| `gpx_ext.c` | 扩展注册 / 格式化 / 解析 |
| `gpx_ext_sensor.c` | 参考：心率、踏频、速度、功率、风向 |
| `example/` | **自定义扩展示例**（NSH `gpx example …`） |
| `gpx_record.c` | 记录线程（启动时配置 N 与队列深度） |
| `gpx_main.c` | NSH 联调 |

## 创建时配置（`gpx_record_create`）

```c
gpx_record_t *rec = NULL;
gpx_record_cfg_t cfg = {
  .batch_size = 32,      /* N：每批写盘点数，必填 > 0 */
  .queue_depth = 0,      /* data 队列容量；0 = ceil(N * 1.2) */
};

gpx_record_create(&cfg, &rec);
gpx_record_start(rec, "/mnt/lfs/Track/ride.gpx", &meta);
```

| 字段 | 说明 |
|------|------|
| `batch_size` | 每满 N 点格式化为一块 XML 并 `write()` 一次 |
| `queue_depth` | 环形队列容量；`0` 时自动 `gpx_port_queue_depth_for_batch(N)` |

`cfg == NULL` 或 `batch_size == 0` 时使用 Kconfig 默认 `MYVENDOR_GPX_DEFAULT_BATCH_SIZE`。

缓冲在 **`gpx_record_create`** 时按 cfg 分配，**`gpx_record_release`** 释放。

详见 [doc/record_handle.md](doc/record_handle.md)。

## NSH 测试

**前置**：LittleFS 已挂载（如 `/mnt/lfs`），且已编译并安装 `gpx` 应用（`CONFIG_MYVENDOR_GPX=y`）。

### 1. 自定义扩展（推荐，一条命令）

```bash
gpx example
# 或指定每路录制秒数（默认 5 s × 3 路）
gpx example demo 10
```

终端会同步打印 `[1/6]` … `[6/6]` 步骤；结束后检查：

```bash
ls -l /mnt/lfs/custom_demo_*.gpx
# decode 输出中每行应含 speed= batt= tag=
```

生成文件示例 decode 行：

```text
  #1 lat=39.908001 lon=116.391998 ele=48.00 time=… speed=18.0 batt=100 tag=flat
```

扩展 XML 在文件内为 `<mybike:payload><![CDATA[{"v":1,"s":…,"b":…,"t":"…"}]]></mybike:payload>`。  
细节见 [example/README.md](example/README.md)。

### 2. 基础命令（无扩展，仅 lat/lon/ele/time）

```bash
gpx record /mnt/lfs/Track/demo.gpx 10
gpx decode /mnt/lfs/Track/demo.gpx
gpx decode /mnt/lfs/Track/demo.gpx 10 10 1   # limit=10 stride=10 batch=1
```

`gpx decode` 可选参数：`[limit] [stride] [batch]`（句柄 `gpx_decode`，无线程）。

> 对 `gpx example` 生成的文件，不要用 `gpx decode` 看扩展字段；应使用 **`gpx example decode <file>`** 或 **`gpx example`** 一键流程。

### 3. 高级子命令（可选）

```bash
gpx example record /mnt/lfs/custom_demo.gpx 10   # 后台 task，NSH 立即返回
gpx example status                               # running → idle
gpx example decode /mnt/lfs/custom_demo.gpx
```

## Kconfig

| 选项 | 说明 |
|------|------|
| `MYVENDOR_GPX_DEFAULT_BATCH_SIZE` | `cfg` 省略时的默认 N |
| `MYVENDOR_GPX_RECORD_STACKSIZE` | 记录线程栈 |
| `MYVENDOR_GPX_STACKSIZE` | `gpx` 主任务栈 |

## 自定义扩展（`<extensions>`）

注册 **format**（写 GPX）、**parse**（读 GPX）、**print**（解码显示），见 `gpx_ext.h`：

```c
#include "gpx_ext.h"

static int my_format(const void *data, char *buf, size_t size) { /* … */ }
static int my_parse(const char *xml, size_t len, void *data) { /* … */ }
static int my_print(const void *data, char *buf, size_t size) { /* … */ }

gpx_ext_desc_t desc = {
  .name = "my_ext",
  .data_size = sizeof(my_ext_t),
  .fmt_max = 128,           /* 单点 XML 上限，参与 buffer 计算 */
  .print_max = 64,          /* 单点 print 文本上限；0 用默认 64 */
  .format = my_format,
  .parse = my_parse,
  .print = my_print,        /* 解码时 gpx_reader_format_trkpt() 会调用 */
};
int slot = gpx_ext_register(&desc);

/* 录制 */
my_ext_t payload = { /* … */ };
point.ext_data[slot] = &payload;
point.ext_mask = (1u << slot);
gpx_record_push(&point);

/* 解码：register → open 句柄 → read → close */
my_ext_t buf;
gpx_decode_t *dec = NULL;
gpx_decode_cfg_t dcfg = { .batch_max = 32, .stride = 1, .limit = 0 };

point.ext_data[slot] = &buf;
gpx_decode_open(path, &dcfg, &dec);
gpx_decode_read(dec, &point, 1, &n);
gpx_reader_format_trkpt(&point, line, sizeof(line));
gpx_decode_close(&dec);
```

示例：`gpx_ext_sensor.c`（hr/cad/speed/power/wind）；`gpx example decode` 会 `gpx_ext_sensor_register()` 并打印扩展字段。通用 `gpx decode` 仅显示 lat/lon/ele/time，除非在解码前自行 `gpx_ext_register()`。

## API（句柄 + data/cmd 队列）

每个录制器 **独立线程**；`create` 后通过 **data 队列** push 点、**cmd 队列** 控制生命周期。可同时 `create` 多个句柄（多文件并行录）。

```c
gpx_record_t *rec = NULL;
gpx_record_cfg_t cfg = { .batch_size = 32, .queue_depth = 0 };

gpx_record_create(&cfg, &rec);
gpx_record_start(rec, "/mnt/lfs/Track/ride.gpx", &meta);
gpx_record_push(rec, &point);
gpx_record_pause(rec);   /* 可选：暂停刷盘，data 仍可入队 */
gpx_record_resume(rec);
gpx_record_stop(rec);      /* 刷盘并 close，线程仍存活 */
gpx_record_release(&rec); /* stop + join 线程 + 释放句柄 */
gpx_record_get_runtime(rec, &runtime);
```

| cmd API | 作用 |
|---------|------|
| `start` | 打开 GPX，开始录制 |
| `pause` / `resume` | 暂停 / 恢复刷写 |
| `stop` | 结束本次会话，线程 idle |
| `release` | 线程自行刷盘、释放资源并退出 |
