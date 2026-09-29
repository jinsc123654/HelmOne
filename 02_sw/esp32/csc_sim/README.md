# csc_sim — 标准踏频计（BLE CSC，0x1816）

ESP32-S3 上模拟一台**只有曲柄数据**的踏频计。车机（Helm One）把它当蓝牙踏频计绑定、
订阅并显示 cadence，就像对真传感器那样。

- 地址 `C0:DE:CA:DE:00:01`（静态随机，永不变），广播名 `Helm-CSC-0001`，外观 `0x0483`
- 绑定：`ctl sensor bind 1CSC C0:DE:CA:DE:00:01 1`
- CLI 提示符 `csc>`
- 状态灯（IO48）**绿色**：空闲闪烁、连上呼吸、没广播时灭；三角波、随机复位、
  名字后缀这些共性见 `../README.md`

## 已完成实机验证

车机侧读到（这段是真车机日志）：

```
I (16319) csc-sim: connected handle=1 peer=70:A6:CC:75:D8:7E itvl=45000 us timeout=420 ms
I (16367) csc-sim: mtu=256
---
test sensor: HR=82 bpm CSC=80 rpm
ble_sensor: CSC 80 rpm loc=NA bat=100%
ble_sensor:   CSC ready rssi=-45 name=Helm-CSC-Sim addr=C0:DE:CA:DE:00:01 want=1 feat=0xa ntf=163700 adv=0
```

后来加入三角波/灯/随机复位/名字后缀后的版本也在板上验证过：踏频 45 s 内从 39 → 105 rpm
线性上升、电量 25% → 85%、`reset 30` 到点真的复位并重新随机（1129 s → 917 s）、
`adv off/on` 时灯按 "off ⇄ green, dim" 切换。

## 控制台命令

| 命令 | 作用 |
|------|------|
| `status` | 模式 / 当前踏频 / 曲柄计数 / 链路 / 广播 / 电量 / 复位倒计时 |
| `rpm <0..250>` | 固定踏频（`rpm 0` = 停止踩踏；上限 250，车机丢弃 ≥300） |
| `ride [60..250]` | 三角波扫踏频 40..<上限> rpm（默认上限 120，60 s 上 + 60 s 下） |
| `stop` | 减速到 0 |
| `battery <0..100\|auto>` | 钉住电量，或交还给三角波（20..100%） |
| `reset [min] [max]\|on\|off` | 定时复位，默认随机 600..1200 s |
| `adv <on\|off>` | 手动开关可连接广播 |

## 协议细节（车机实际解析的字段）

CSC Measurement（0x2A5B），本模拟器发的 5 字节：

```
byte 0    flags = 0x02            # 只有曲柄数据，没有轮速数据
byte 1-2  累计曲柄圈数（u16 LE，会回绕）
byte 3-4  上次曲柄事件时间（u16 LE，单位 1/1024 s，约 64 s 回绕）
```

车机算法：`rpm = Δ圈数 × 1024 × 60 / Δ事件时间`，所以**事件时间必须按真实圈间距走**，
不能按通知节奏走。本工程积分曲柄角度，每完成一圈用插值算出生圈时刻（µs 精度），
于是车机算出的踏频是稳的、且严格跟着三角波走。

其它特征：CSC Feature `0x2A5C` = 0x0002（只支持曲柄数据）、Sensor Location `0x2A5D` = 0x0B
（chainstay）、SC Control Point `0x2A55`（不提供任何 CP 过程，一律回 `Op Code Not Supported`；
未订阅 indication 就写它 → ATT 错误 0x81，符合 profile）。
