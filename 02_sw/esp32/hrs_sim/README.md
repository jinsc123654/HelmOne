# hrs_sim — 标准心率带（BLE Heart Rate，0x180D）

ESP32-S3 上模拟一条胸带心率计：心率按三角波来回扫，并且能演"带子从身上掉下来"。

- 地址 `C0:DE:CA:DE:00:03`（静态随机，永不变），广播名 `Helm-HR-0003`，外观 `0x0341`
- 绑定：`ctl sensor bind 0HR C0:DE:CA:DE:00:03 1`
- CLI 提示符 `hrs>`
- 状态灯（IO48）**红色**：空闲闪烁、连上呼吸、没广播时灭；三角波、随机复位、
  名字后缀这些共性见 `../README.md`

## 控制台命令

| 命令 | 作用 |
|------|------|
| `status` | 模式 / 当前心率 / 链路 / 广播 / 电量 / 复位倒计时 |
| `bpm <30..250>` | 固定心率（`bpm 0` 等于把带子从身上拿下来） |
| `ride [90..250]` | 三角波扫心率 70..<上限> bpm（默认上限 160，60 s 上 + 60 s 下） |
| `rest` | 回到静息（58 bpm） |
| `off` | 脱离身体：无接触、无有效读数 |
| `battery <0..100\|auto>` | 钉住电量，或交还给三角波（20..100%） |
| `reset [min] [max]\|on\|off` | 定时复位，默认随机 600..1200 s |
| `adv <on\|off>` | 手动开关可连接广播 |

开机默认就跑 `ride`（三角波）—— 和另外两个模拟器一致，开串口复位后立刻有数据。

## 协议细节（车机实际解析的字段）

Heart Rate Measurement（0x2A37），本模拟器发的 2 字节：

```
byte 0  flags：bit1 = 本传感器支持接触检测，bit2 = 当前检测到接触
byte 1  心率（8 bit；flags bit0 置位时才是 u16 LE）
```

- 贴身时 `flags = 0x06` + bpm。
- `off` 时 `flags = 0x02` + bpm=0：车机侧 bpm==0 直接丢弃（`parse_hr_meas()`），界面上就是
  "没有读数"，和真带子摘下来一致。
- 车机显示行：`HR <bpm> bpm loc=<n> bat=<n>% contact=yes|no|NA energy=NA`。
  本模拟器提供 Body Sensor Location（0x2A38）= 0x01（chest），**不**发送 energy expended
  （与那条小米胸带一致，车机显示 `energy=NA`）。想测车机的能量字段：`hr_defs.h` 里
  `HR_FLAG_ENERGY_PRESENT` 置位、并在 bpm 之后补两个字节（kJ，u16 LE）；要支持"重置能量"
  再补上 0x2A39 控制点。

## 验证状态

**只做了编译验证**（`idf.py build` 无警告，应用 0x93510 ≈ 577 KB / 2 MB 分区）；
三角波/灯/复位/名字这四条走的是与另两个工程相同的代码路径，但 **hrs_sim 本体还没上板跑过**。
板子到手后一条命令即可：

```bash
/usr/bin/python3 ../tools/sensor_central.py --kind hr -s 15   # 期望 bpm 从 70 往 160 爬
```

再在车机上 `ctl sensor bind 0HR C0:DE:CA:DE:00:03 1` + `test sensor read`，
期望 `HR <n> bpm loc=... bat=<n>% contact=yes energy=NA`。
