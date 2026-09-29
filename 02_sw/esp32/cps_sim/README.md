# cps_sim — 标准功率计（BLE Cycling Power，0x1818）

ESP32-S3 上模拟一台曲柄式功率计：功率按三角波来回扫，同时按标准报累计曲柄圈数和事件时间。

- 地址 `C0:DE:CA:DE:00:02`（静态随机，永不变），广播名 `Helm-CPS-0002`，外观 `0x0484`
- 绑定：`ctl sensor bind 2CPS C0:DE:CA:DE:00:02 1`
- CLI 提示符 `pwr>`
- 状态灯（IO48）**黄色**：空闲闪烁、连上呼吸、没广播时灭；三角波、随机复位、
  名字后缀这些共性见 `../README.md`

## 控制台命令

| 命令 | 作用 |
|------|------|
| `status` | 模式 / 当前功率 / 踏频 / 链路 / 广播 / 电量 / 复位倒计时 |
| `watts <0..3000>` | 固定功率（`watts 0` = 停止踩踏；上限 3000，车机丢弃 >3000） |
| `ride [150..3000]` | 三角波扫功率 100..<上限> W（默认上限 300，60 s 上 + 60 s 下） |
| `stop` | 功率降到 0 W |
| `battery <0..100\|auto>` | 钉住电量，或交还给三角波（20..100%） |
| `reset [min] [max]\|on\|off` | 定时复位，默认随机 600..1200 s |
| `adv <on\|off>` | 手动开关可连接广播 |

## 协议细节（车机实际解析的字段）

Cycling Power Measurement（0x2A63），本模拟器发的 8 字节：

```
byte 0-1  flags = 0x0020          # u16 LE：bit5 = 带曲柄数据
byte 2-3  瞬时功率（int16 LE）      # 必选字段，永远在 flags 之后这个位置
byte 4-5  累计曲柄圈数（u16 LE）
byte 6-7  上次曲柄事件时间（u16 LE，1/1024 s）
```

车机只取 **byte 2-3 的瞬时功率**（`parse_cps_meas()` 在 `len >= 4` 后直接读 `p[2] | p[3]<<8`），
负值钳 0、>3000 W 丢弃。因为"瞬时功率"在规范里就是紧随 2 字节 flags 的必选字段，
所以无论 flags 怎么设，byte 2-3 的位置都不变。

报告的踏频取 `功率 ÷ 2.3`，所以功率扫的时候踏频一起扫（100 W → 约 43 rpm，300 W → 约 130 rpm），
曲柄圈数/事件时间是真按这个踏频积出来的。

其它特征：Cycling Power Feature `0x2A65`（u32 LE）= 0x00000008（只声明支持曲柄数据）、
Sensor Location `0x2A5D` = 0x05（left crank）、SC Control Point `0x2A55`（不提供校准/曲柄长度/
屏蔽等过程，一律回 `Op Code Not Supported`；未订阅 indication 就写它 → ATT 0x81）。

## 验证状态

已在板上验证：功率 32 s 内 112 → 219 W 线性上升、踏频 49 → 95 rpm 同步、
电量 25% → 67%、`reset` 倒计时在 600..1200 随机区间内、灯 `yellow, dim (advertising)`。
**尚未用真车机（Helm One）绑定验证** —— 车机侧的 CPS 记录还空着。车机在的时候：

```bash
ctl sensor bind 2CPS C0:DE:CA:DE:00:02 1     # 然后 test sensor read
```

期望看到 `CPS <n> W loc=NA bat=<n>%`，数值每秒变（三角波）。
