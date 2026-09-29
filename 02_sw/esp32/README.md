# ESP32-S3 传感器模拟器（Helm One 台架仪表）

三块 ESP32-S3，各自冒充 Helm One 车机能绑定的三种 BLE 传感器，用来在没有真传感器
（或真传感器不好用时）验证车机从扫描、绑定、订阅到显示的整条链路。

| 工程 | 冒充的传感器 | GATT 服务 | 地址（静态随机，固定） | 广播名 | 车机侧绑定命令 |
|------|--------------|-----------|------------------------|--------|----------------|
| `csc_sim/` | 踏频计 | `0x1816` Cycling Speed and Cadence | `C0:DE:CA:DE:00:01` | `Helm-CSC-0001` | `ctl sensor bind 1CSC C0:DE:CA:DE:00:01 1` |
| `cps_sim/` | 功率计 | `0x1818` Cycling Power | `C0:DE:CA:DE:00:02` | `Helm-CPS-0002` | `ctl sensor bind 2CPS C0:DE:CA:DE:00:02 1` |
| `hrs_sim/` | 心率带 | `0x180D` Heart Rate | `C0:DE:CA:DE:00:03` | `Helm-HR-0003` | `ctl sensor bind 0HR C0:DE:CA:DE:00:03 1` |

三个工程互相独立（各自 `idf.py build`），但都遵守同一条与车机的契约，见下。

## 编译 / 烧录

```bash
cd csc_sim            # 或 cps_sim / hrs_sim
source ~/ESP_IDF/5.4.3/esp-idf/export.sh
idf.py set-target esp32s3        # 第一次
idf.py -p /dev/ttyACMx -b 921600 flash
idf.py -p /dev/ttyACMx monitor   # 串口控制台（注意：打开串口会复位板子）
```

⚠️ `/dev/ttyACMx` 的编号随插拔变化，烧之前先认板子：
`udevadm info -q property -n /dev/ttyACM1 | grep SERIAL_SHORT`（`5658059041` 是 vela 车机）。

⚠️ **一种传感器同时只开一块板。** 三个模拟器的地址是写死的常量，两块同类的板一起上电
会互相抢连接（主机侧就表现为反复 `le-connection-abort-by-local`、名字忽明忽暗）。
要并行跑两个同类，改各自 `main.c` 里的 `s_own_addr`。

## 四个台架行为（三个工程一致）

### 1. 状态灯：IO48 上的 WS2812

**颜色 = 传感器种类；动效 = 链路状态**；峰值亮度压得很低（就摆在车机屏幕旁边）：

| 工程 | 颜色（峰值） | 空闲（在广播、没人连） | 已连接 | 没在广播 |
|------|--------------|------------------------|--------|----------|
| `csc_sim` | 绿 rgb(0, 22, 0) | **闪烁** 1 Hz（亮 35% / 灭） | **呼吸** 3.2 s（15% ⇄ 100%） | 灭 |
| `cps_sim` | 黄 rgb(30, 16, 0) | 同上 | 同上 | 灭 |
| `hrs_sim` | 红 rgb(28, 0, 0) | 同上 | 同上 | 灭 |

即：绿=踏频、黄=功率、红=心率；没人连时是硬闪（一眼看出在等人），连上后是慢呼吸
（一眼看出在通信）。动画由一颗 40 fps 的小任务**按帧缩放颜色**实现——可寻址灯珠没有
PWM，只能靠改值做渐变。想调色 / 调快慢改各自 `led.c` 顶部那几行（`LED_RGB`、
`LED_BLINK_MS`、`LED_IDLE_LEVEL`、`LED_BREATH_MS`、`LED_BREATH_MIN`）；
黄是 R 略高于 G，这两个数值下 R=G 会看成绿色。

### 2. 数据与电量都是三角波

| 量 | 范围 | 周期 |
|----|------|------|
| 踏频 | 40 .. 120 rpm（`ride <top>` 可改上限） | 60 s 上 + 60 s 下 |
| 功率 | 100 .. 300 W（`ride <top>`） | 60 s + 60 s |
| 心率 | 70 .. 160 bpm（`ride <top>`） | 60 s + 60 s |
| 电量 | 20 % .. 100 % | 60 s + 60 s |

功率计报告的踏频 = 功率 ÷ 2.3，所以两者一起扫，读数自洽。电量的变化搭在测量通知的
顺风车上发（不额外起定时器）；`battery <n>` 钉住一个值，`battery auto` 交还给三角波。

### 3. 随机复位（默认 10~20 分钟）

开机后在 **600~1200 s 之间随机**取一个时刻复位，每次复位后重新随机 —— 用来反复练车机
"传感器消失又回来"的恢复路径，又不至于和别的固定周期撞上。控制台：

| 命令 | 作用 |
|------|------|
| `reset` | 看下次复位还有多久、区间是多少 |
| `reset off` / `reset on` | 关掉 / 恢复默认随机区间 |
| `reset 600` | 固定 600 s |
| `reset 600 1200` | 随机区间 600~1200 s |

### 4. 名字带地址后 4 位

广播名 = `Helm-CSC-0001` 这种形式，后缀是自身地址（`C0:DE:CA:DE:00:01`）的最后四个
十六进制字符。台架上一眼就知道是哪块板，不用去翻地址。

## 与车机的契约（三者相同）

1. **地址固定不变**：静态随机地址（最高位两比特 `11`）。车机按 MAC 绑定并把记录写进
   `/mnt/kv/bicycle_sensors.tsv`，地址一变记录就废了（当初放弃手机 App 模拟器就是这个原因）。
2. **广播里必须有本传感器的 16 bit 服务 UUID**，可连接、非定向、legacy。
3. **连上以后停止广播**：车机把"已连接却在广播"的设备当僵尸，会主动拆链。
4. **测量用 notify 且不断流**：车机 8 s 无测量就判 idle 并做 ATT 探活；三个模拟器在连接
   期间都保证有流量（踏频每转一圈 + 静止时 2 s 心跳，功率/心率固定 1 Hz）。
5. **附带电池服务（0x180F）**：车机探活优先读电池句柄，有了它空闲期不会掉链。

## 车机侧怎么用

```bash
ctl sensor bind 1CSC C0:DE:CA:DE:00:01 1    # 绑定（kind: 0HR / 1CSC / 2CPS）
test sensor read                            # 打印表格：HR/CSC/CPS 各一行
```

正常输出形如：

```
test sensor: HR=82 bpm CSC=80 rpm
ble_sensor: CSC 80 rpm loc=NA bat=100%
ble_sensor:   CSC ready rssi=-45 name=Helm-CSC-0001 addr=C0:DE:CA:DE:00:01 want=1 feat=0xa ntf=163700 adv=0
```

## 台架上踩过的坑（都是车机侧，不是模拟器）

1. **绑定了一台"当前看不见"的传感器之后，车机的手动扫描会自锁。**
   `ble_sensor_connect_busy()`（`ble_sensor.c:4228`）把 `want_link && state != READY`
   算成"正在连接"，而 `ble_sensor_scan_start()`（`:4264`）在该状态下拒绝起扫；偏偏
   `test sensor scan` 自己会先 `ble_sensor_start()` 把记录加载成 `want_link`，于是这条
   命令每次都打 `sensor scan skipped, connecting` 然后什么都不做。
   **恢复办法：重启车机。** 开机那一下会排队一个 60 s 全量扫描 burst，配对窗口一结束
   立刻生效 —— 实测窗口结束 2 s 内就扫到并连上了。
2. **第一次连接可能在建链 282 ms 后被 `0x3e` 拆掉，第二次就正常。** 模拟器和真踏频计
   （`78:D8:40:4B:F5:20`）都出现过同一签名，所以不是对端的问题。
3. **车机丢弃的数值**：功率 > 3000 W、踏频 ≥ 300 rpm、心率 0 或 > 250 —— 模拟器里都钳过。
4. **打开模拟器串口会复位它**（USB 桥的 EN 脉冲），所以三个模拟器开机就进入产出数据的
   状态，免得控制台把命令吃掉之后传感器一片安静。

## 主机侧对照工具

`tools/sensor_central.py`：用 BlueZ D-Bus 当一台独立的中心设备，做和车机一样的事
（扫描→连接→订阅→按车机同样的字段解析通知）。

```bash
/usr/bin/python3 tools/sensor_central.py --kind csc -s 15
/usr/bin/python3 tools/sensor_central.py --kind cps -s 15
/usr/bin/python3 tools/sensor_central.py --kind hr  -s 15
```

必须用**系统 python**（ESP-IDF 的 py 环境里没有 `dbus`）。本机 BlueZ 偶尔抽风：`Device1.Connect()`
会以 `le-connection-abort-by-local` 失败，用 `bluetoothctl connect <addr>` 先连上、再跑这个
工具反而稳（工具会复用已连上的链路）。
