# GPX 文件组成说明

**标准**：GPX 1.1（[Topografix GPX](https://www.topografix.com/gpx.asp)）  
**本组件实现**：`gpx_writer` 写入 / `gpx_reader` 读取  
**更新**：2026-06-16

---

## 1. GPX 是什么

GPX（GPS Exchange Format）是基于 **XML 的纯文本** 格式，用来交换 GPS 轨迹、路线、航点等。  
骑行记录场景下，核心内容是 **轨迹点序列**（`<trkpt>`），可被 Strava、Garmin Connect、高德等工具导入。

特点：

- 人类可读，可用文本编辑器打开
- 体积随点数线性增长（本组件约 **130～160 字节/点**，见 §3）
- 坐标系通常为 **WGS84**（与 GNSS 输出一致）

---

## 2. GPX 各版本有什么区别

常见「版本」指 Topografix 发布的 **GPX 1.0 / 1.1** 主标准；另有厂商 **扩展字段**，容易与主版本号混淆。

### 2.1 版本一览

| 版本 | 年代 | 状态 | 说明 |
|------|------|------|------|
| **GPX 0.x** | 2002 前 | 已淘汰 | 早期草案，现几乎见不到 |
| **GPX 1.0** | 2002 | 仍可读 | 第一个广泛使用的正式版 |
| **GPX 1.1** | 2007 | **当前主流** | 本组件、`TRK_EXAMPLE.gpx` 均为此版 |
| **厂商扩展** | — | 并存 | 如 Garmin `gpxtpx:`、心率/踏频；**不是** GPX 2.0 |

本组件 **`gpx_writer` 固定输出 GPX 1.1**（`version="1.1"` + 1.1 命名空间）。

### 2.2 GPX 1.0 与 1.1 主要差异

| 对比项 | GPX 1.0 | GPX 1.1 |
|--------|---------|---------|
| Schema URL | `.../GPX/1/0` | `.../GPX/1/1` |
| `<metadata>` | 较简单 | 增加 **author、copyright、link、email、keywords** 等 |
| 扩展机制 | 有限 | 更规范的 **extension** 嵌套 |
| 元素约束 | 较松 | XSD 更完整，互操作性更好 |
| 轨迹/路线/航点 | 均支持 | 均支持，语义与 1.0 大体兼容 |
| 实际兼容性 | 老设备/软件 | Strava、Garmin、高德等 **默认按 1.1 处理** |

对 **骑行轨迹记录**（只有 `trk/trkseg/trkpt` + 可选 metadata）而言：

- 1.0 与 1.1 的 **`<trkpt lat lon ele time>` 写法相同**，导入工具通常 **都能读**。
- 差异主要在 **元数据 richness** 和 **扩展字段**；本组件不写 1.1 新增的高级 metadata，因此与精简 1.0 文件在体积上接近。

### 2.3 厂商扩展（易混淆，非主版本）

部分 App/码表会在 `<trkpt>` 或 `<metadata>` 下挂 **私有 XML 命名空间**，例如：

- 心率 `<gpxtpx:hr>145</gpxtpx:hr>`
- 踏频、速度、功率、温度等

特点：

- 仍包在 GPX 1.1 文件里，根元素仍是 `version="1.1"`。
- **体积更大**；只有对应平台完全支持。

**本组件写入的具体字段**（`gpx_ext_sensor`，骑行录制用）：

| 字段 | 元素 | 单位 | 定义来源 |
|------|------|------|----------|
| 心率 | `<gpxtpx:hr>` | bpm | Garmin TrackPointExtension **v2** |
| 踏频 | `<gpxtpx:cad>` | rpm | 同上 |
| 速度 | `<gpxtpx:speed>` | **m/s** | 同上；**speed 只在 v2 里定义**（v1 无），多数工具自己 ×3.6 显示 km/h |
| 功率 | `<gpxpx:PowerInWatts>` | W | Garmin **PowerExtension v1**：该 schema 只定义这一个元素（`xsd:unsignedShort`），直接挂在 `<extensions>` 下、无外层包裹 |

**只写有 schema 定义的字段**。风向这类没有定义的**不写**：Garmin 的 gpxtpx/gpxx 与
gpxdata 都没有风向元素，只有一份提案（logiqx）里的 `twd`/`tws`，不是 schema，所以本
扩展不再带 `mybike:windDir`。同理不写裸 `<power>` —— 那是事实约定、无 XSD，功率用
上面那个有定义的 `gpxpx:PowerInWatts`。

四个字段全开时单点扩展实测 371 字节，`GPX_EXT_SENSOR_FMT_MAX` 按 512 预留；
`has_*` 为 false 的字段不写，所以没有心率带时文件里就没有 `gpxtpx:hr`。

### 2.4 本组件读写的版本策略

| 能力 | 策略 |
|------|------|
| 写入 | 仅 GPX **1.1**，单 `trk` / 单 `trkseg` |
| 读取 | 按 XML 扫描 `<trkpt>`，**1.0 / 1.1 均可**（只要标签结构相同） |
| 扩展 | 写入/读取经 **可注册扩展点**（§3.3 / `gpx_ext.h`）；`gpx decode` 只显示已注册扩展的字段 |
| 不支持 | GPX 0.x、FIT 二进制、KML |

---

## 3. 文件体积估算（采样率与时长）

### 3.1 估算公式

```text
文件大小 ≈ 文件头尾固定开销 + 轨迹点数 × 每点 XML 字节数

轨迹点数 = 采样频率 (Hz) × 骑行时长 (秒)
```

**本组件**（`gpx_writer_format_trkpt`，含 `lat/lon` + `<ele>` + `<time>`，带换行缩进）：

| 每点大小 | 条件 |
|----------|------|
| **约 130～160 B** | 典型（6 位经纬度、2 位海拔、UTC 时间） |
| **约 80～100 B** | 仅 `lat/lon`，无 `ele`/`time` |
| **≤ 256 B** | 单点格式化上限 `GPX_TRKPT_FMT_MAX` |

文件头尾（`<gpx>` + `metadata` + `<trk>` 开闭标签）一般 **0.4～1 KB**，相对长骑行可忽略。

以下按 **典型 145 B/点 + 0.5 KB 开销** 估算（**1 Hz，即每秒 1 个点**）。

### 3.2 1 Hz 采样 — 1 / 30 / 60 分钟

| 骑行时长 | 点数 | 估算体积 | 说明 |
|----------|------|----------|------|
| **1 分钟** | 60 | **≈ 9～10 KB** | 短途试跑、联调 `gpx record … 60` |
| **30 分钟** | 1 800 | **≈ 260～280 KB** | 通勤级骑行 |
| **60 分钟** | 3 600 | **≈ 520～550 KB** | 1 小时休闲骑 |

心算：**1 Hz 下约 145 KB / 10 分钟**（仅轨迹体，含头尾误差 ±几 KB）。

### 3.3 其它采样率（60 分钟对照）

| 采样率 | 60 分钟点数 | 估算体积（145 B/点） |
|--------|-------------|----------------------|
| 0.2 Hz（5 s 一点） | 720 | **≈ 105 KB** |
| **1 Hz** | 3 600 | **≈ 520 KB** |
| 5 Hz | 18 000 | **≈ 2.5 MB** |
| 10 Hz | 36 000 | **≈ 5.0 MB** |

码表 / GNSS 常见 **1 Hz**；导航或运动模式可能 **5 Hz**，体积成倍增加。

### 3.4 与存储、同步的关系

| 场景 | 参考 |
|------|------|
| LittleFS / SD 单文件 | 60 分钟 @ 1 Hz ≈ **0.5 MB**，压力不大 |
| BLE 传整文件 | 0.5 MB @ 15 KB/s ≈ **35 s**；大文件走 USB MTP |
| 批写 N 点 | 只减少 **写盘次数**，**不减少** 最终文件体积 |
| 省体积手段 | 降采样、缩短记录时长、App 侧记录、二进制 FIT（非 GPX） |

---

## 4. 文档树形结构

本组件生成的骑行 GPX 采用 **单 track / 单 segment** 结构：

```text
gpx                          ← 根元素（整文件容器）
├── metadata                 ← 文件级元信息（可选）
│   ├── name
│   └── desc
└── trk                      ← 一条轨迹（Track）
    ├── name                 ← 轨迹名称（可选）
    ├── desc                 ← 轨迹描述（可选）
    └── trkseg               ← 轨迹段（Segment）
        ├── trkpt            ← 轨迹点 1
        ├── trkpt            ← 轨迹点 2
        └── ...
```

GPX 1.1 还支持本组件 **暂未写入** 的节点：

| 节点 | 用途 | 本组件 v0.1 |
|------|------|-------------|
| `wpt` | 航点（POI） | 不生成 |
| `rte` / `rtept` | 路线 | 不生成 |
| 多个 `trk` | 多段活动 | 仅 1 个 `trk` |
| 多个 `trkseg` | 暂停/分段 | 仅 1 个 `trkseg` |

---

## 5. 完整示例（与本组件输出一致）

```xml
<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="Helm One"
 xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
 xmlns="http://www.topografix.com/GPX/1/1"
 xsi:schemaLocation="http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd"
>
<metadata>
<name><![CDATA[Helm One ride]]></name>
<desc><![CDATA[...]]></desc>
</metadata>
<trk>
<name><![CDATA[/mnt/lfs/Track/TRK_20260616_120000.gpx]]></name>
<trkseg>
  <trkpt lat="39.907415" lon="116.391332">
    <ele>45.20</ele>
    <time>2026-06-16T08:00:01Z</time>
  </trkpt>
  <trkpt lat="39.907515" lon="116.391432">
    <ele>45.50</ele>
    <time>2026-06-16T08:00:02Z</time>
  </trkpt>
</trkseg>
</trk>
</gpx>
```

> 注：本组件 `gpx_writer_open()` 不写 XML 声明行 `<?xml ...?>`，但内容符合 GPX 1.1；多数导入工具可正常识别。

---

## 6. 各层元素说明

### 6.1 根元素 `<gpx>`

| 属性 | 含义 | 本组件 |
|------|------|--------|
| `version` | GPX 版本 | 固定 `"1.1"` |
| `creator` | 生成程序名 | `gpx_meta.creator`，默认 `CONFIG_MYVENDOR_PRODUCT_NAME`（Helm One） |
| `xmlns` 等 | XML 命名空间 | 固定 Topografix 1.1 |

对应代码：`gpx_writer_open()` 写入 `GPX_HEAD`。

### 6.2 `<metadata>`

描述 **整份文件** 的摘要，不参与轨迹几何计算。

| 子元素 | 含义 | 映射 |
|--------|------|------|
| `<name>` | 活动/文件标题 | `gpx_meta.meta_name` |
| `<desc>` | 说明、URL 等 | `gpx_meta.meta_desc` |

长文本使用 `<![CDATA[...]]>` 包裹，避免 XML 特殊字符转义。

### 6.3 `<trk>` 轨迹

一条连续或逻辑上属于同一活动的轨迹。

| 子元素 | 含义 | 映射 |
|--------|------|------|
| `<name>` | 轨迹名 | `gpx_meta.track_name` |
| `<desc>` | 轨迹描述 | `gpx_meta.track_desc` |
| `<trkseg>` | 轨迹段容器 | 固定一个 |

### 6.4 `<trkseg>` 轨迹段

- 一次 `<trk>` 可含多个 `<trkseg>`（例如中间暂停后分段）。
- 本组件 **整段骑行用一个 `trkseg`**，停录时在 `gpx_writer_close()` 写闭合标签。

### 6.5 `<trkpt>` 轨迹点（核心）

每个点表示某一时刻的位置（可选海拔、时间）。

```xml
<trkpt lat="39.907415" lon="116.391332">
  <ele>45.20</ele>
  <time>2026-06-16T08:00:01Z</time>
</trkpt>
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `lat` 属性 | 浮点 | 是 | 纬度，WGS84，度 |
| `lon` 属性 | 浮点 | 是 | 经度，WGS84，度 |
| `<ele>` | 浮点 | 否 | 海拔，**米** |
| `<time>` | ISO 8601 UTC | 否 | 点时间，结尾 `Z` |

**本组件 C 结构映射**（`gpx_types.h`）：

```c
typedef struct gpx_point {
  float latitude;    /* → lat 属性 */
  float longitude;   /* → lon 属性 */
  float altitude;    /* → <ele>，需 has_altitude */
  gpx_time_t time;   /* → <time>，需 has_time */
  bool has_altitude;
  bool has_time;
} gpx_point_t;
```

格式化精度（`gpx_writer_format_trkpt`）：

- 经纬度：6 位小数
- 海拔：2 位小数
- 时间：`YYYY-MM-DDTHH:MM:SSZ`

---

## 7. 文件写入顺序（与记录批写对应）

`gpx_record` 写盘时，文件内容按时间分为 **头 / 体 / 尾** 三段：

```text
┌─────────────────────────────────────────┐
│ 头（gpx_writer_open，仅一次）            │
│   <gpx> … <metadata> … <trk> … <trkseg> │
├─────────────────────────────────────────┤
│ 体（每批 N 点，重复 K 次）               │
│   <trkpt>…</trkpt> × N                  │
│   <trkpt>…</trkpt> × N                  │
│   …                                     │
│   尾批：<trkpt>…</trkpt> × (余数)        │
├─────────────────────────────────────────┤
│ 尾（gpx_writer_close，仅一次）           │
│   </trkseg></trk></gpx>                 │
└─────────────────────────────────────────┘
```

这与内存中的处理对应：

1. 队列存 **结构体** `gpx_point_t`
2. 每满 **N 点** → 格式化为 XML 字符串 → **一次 `write()`**
3. 停录 → flush 剩余点 → 写 footer

---

## 8. 读取时解析范围（`gpx_reader`）

解码器 **流式** 扫描文件，每次返回一个 `<trkpt>`：

1. 查找 `<trkpt`
2. 解析 opening tag 中的 `lat=`、`lon=`
3. 读取子元素 `<ele>`、`<time>`（若存在）
4. 遇到 `</trkpt>` 返回一个 `gpx_point_t`

**不解析**：`metadata`、`trk/name`、多 track、route、waypoint。  
若文件含多个 `<trkseg>`，会顺序读出所有 `<trkpt>`。

---

## 9. 与 X-TRACK 示例文件对比

仓库内示例：`mkfs/Track/TRK_EXAMPLE.gpx`（X-TRACK 录制）

| 项目 | X-TRACK 示例 | 本组件 `gpx_writer` |
|------|--------------|---------------------|
| GPX 版本 | 1.1 | 1.1 |
| creator | `Arduino GPX Lib` | 可配置，默认 `CONFIG_MYVENDOR_PRODUCT_NAME`（Helm One） |
| metadata | 有 name/desc | 有（可选） |
| 结构 | trk → trkseg → trkpt | 相同 |
| trkpt 字段 | ele + time | ele + time（可选） |
| 换行/缩进 | 较紧凑 | 每点独立行，带缩进 |

两者导入第三方工具通常 **兼容**。

---

## 10. 命名与存储建议

本组件不强制路径，常见约定：

```text
/mnt/lfs/Track/
  2026_06/
    TRK_20260616_120000.gpx
```

- 扩展名：`.gpx`
- 时间戳建议 **UTC**（与 `<time>` 一致）
- 单文件 = 单次骑行活动

---

## 11. 常见问题

**Q：没有 `<time>` 能否导入？**  
A：可以显示轨迹线，但速度/时长分析会受限。建议 GNSS 有效时始终带时间。

**Q：海拔用 GNSS 还是气压计？**  
A：由生产者决定，写入 `<ele>` 即可；本组件不融合。

**Q：文件很大怎么办？**  
A：增大批写 N 只减少 syscall，不减少文件体积；长期骑行可考虑分文件或 App 侧记录。

**Q：能否追加到已有 GPX？**  
A：v0.1 仅 `O_TRUNC` 新建；追加需新 API（未实现）。

---

## 12. 参考

- [GPX 1.1 Schema](http://www.topografix.com/GPX/1/1/gpx.xsd)
- 组件实现：`gpx_writer.c`、`gpx_reader.c`
- 设计总览：[design.md](design.md)
