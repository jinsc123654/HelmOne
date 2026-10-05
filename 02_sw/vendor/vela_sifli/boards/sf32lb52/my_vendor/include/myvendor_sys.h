/**
 * @file myvendor_sys.h
 * @brief 整机只读快照：转发各 owner，不生产、不融合、不写 persist。
 *
 * 层次：
 *
 *   UI / NSH `ctl` / GATT set       UI / NSH `sys` / GATT read
 *           │ 写策略、发命令               │ 读快照
 *           ▼                              ▼
 *   myvendor_devctl                   myvendor_sys   ← 本文件
 *           │                              │ 只转发
 *           ▼                              ▼
 *   PWM / USB / BLE policy            eta9184 / companion / usbdev / mtp /
 *                                     myvendor_board_sensor（I2C + VBATS 乒乓）
 *
 * 产品 UI 只走 ctl（写）和 sys（读）。NSH `test *` 是外设 debug 探针，
 * 保留原控制逻辑，不经 ctl/sys。
 *
 * 圈速、会话里程、导航剩余、GNSS 源选择仍在 `bicycle_runtime`（UI 会话）。
 * 板载 GNSS 由 `myvendor_gnss` 读 MAX-M10S-00B-01 NMEA 并缓存；手机 GNSS 走 companion。
 * IMU / 气压 / 罗盘 / VBATS 由板卡传感器线程 `board_sensor` 后台采样；
 * sys/UI/BLE 只拷贝快照，不再 open I2C 或 `/dev/adc0`。ADC 为双槽乒乓，
 * 避免多路 RESET_FIFO 饿死等待者。坡度零点在 `persist.ui.grade_off_mdeg`。
 * 电量百分比走 VBATS（锂电 OCV 曲线 + IIR + 回差）；充电→充满时该线程
 * 调用 ctl 写 `persist.battery.full_mv`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_SYS_H
#define MYVENDOR_SYS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYVENDOR_SYS_BAT_MV_EMPTY 3000  /**< 0%：软件 3.00 V（电芯 2.80 V；与 bat_soc 一致）。 */

/** 电源综合状态（映射 ETA9184，调用方不必包含驱动头）。 */
#define MYVENDOR_SYS_PWR_IDLE         0u  /**< 未充电、未放电。 */
#define MYVENDOR_SYS_PWR_CHARGING     1u  /**< 充电中。 */
#define MYVENDOR_SYS_PWR_DISCHARGING  2u  /**< 放电中。 */
#define MYVENDOR_SYS_PWR_LOW          3u  /**< 低电量（&lt;3%）。 */

#define MYVENDOR_SYS_USB_PLUG_OFF     0u  /**< 未配置 MTP。 */
#define MYVENDOR_SYS_USB_PLUG_ENUM    1u  /**< 已枚举，传输未开始。 */
#define MYVENDOR_SYS_USB_PLUG_ACTIVE  2u  /**< 协议 poll 中。 */

/**
 * @brief 电量与充放电。
 */
typedef struct myvendor_sys_power_s
{
  int     pct;     /**< VBATS 电量；-1 尚未采到，否则 0～100。 */
  int     mv;      /**< VBATS 电池毫伏；<0 尚未采到。 */
  int     full_mv; /**< 100% 校准毫伏（persist.battery.full_mv，缺省 4100）。 */
  bool    full_cal; /**< true：KV 里已有合法校准；false：正在用缺省 4100。 */
  uint8_t state;   /**< MYVENDOR_SYS_PWR_*（ETA9184 充放电）。 */
  bool    boost;   /**< 5V Boost（功放）是否打开。 */
} myvendor_sys_power_t;

/**
 * @brief 按星座分组的索引（`sats_*_c[]` 用）。顺序 = UI 里的行顺序。
 */
typedef enum
{
  MYVENDOR_SYS_GNSS_CONST_GPS = 0,   /**< $GPGSV / $GPGSA */
  MYVENDOR_SYS_GNSS_CONST_GLO,       /**< $GLGSV / $GLGSA */
  MYVENDOR_SYS_GNSS_CONST_GAL,       /**< $GAGSV / $GAGSA */
  MYVENDOR_SYS_GNSS_CONST_BDS,       /**< $BDGSV / $BDGSA */
  MYVENDOR_SYS_GNSS_CONST_QZSS,      /**< $GQGSV / $GQGSA */
  MYVENDOR_SYS_GNSS_CONST_N
} myvendor_sys_gnss_const_e;

/**
 * @brief 读线程健康（GNSS 恢复状态上屏用，见 myvendor_sys_gnss_t::rd_state）。
 */
typedef enum myvendor_sys_gnss_health_e
{
  MYVENDOR_SYS_GNSS_HL_OK      = 0, /**< 心跳正常。 */
  MYVENDOR_SYS_GNSS_HL_STALL   = 1, /**< 心跳停了：读线程卡住（含卡在 poll 里）。 */
  MYVENDOR_SYS_GNSS_HL_RECOVER = 2, /**< 正在回收 / 重启读线程。 */
  MYVENDOR_SYS_GNSS_HL_FAILED  = 3, /**< 回收失败（仍在重试，端口保持 open）。 */
} myvendor_sys_gnss_health_e;

/**
 * @brief 一份 GNSS 定位（手机或板载）。
 */
typedef struct myvendor_sys_gnss_s
{
  bool     valid;             /**< 有经纬度的有效定位。 */
  bool     alive;             /**< 近期有 NMEA 或手机帧（含搜星）。 */
  int32_t  lat_e7;            /**< 纬度 ×1e7。 */
  int32_t  lon_e7;            /**< 经度 ×1e7。 */
  int32_t  alt_mm;            /**< 海拔，毫米。 */
  uint16_t speed_centi_kmh;   /**< 0.01 km/h。 */
  uint16_t course_deg;        /**< 0～359。 */
  uint8_t  satellites;        /**< 解算用卫星。 */
  uint8_t  sats_heard;        /**< SNR>0 的跟踪星；搜星进度看这个。 */
  uint8_t  sats_in_view;      /**< GSV 可见（含历书、SNR=0）。 */
  uint8_t  fix_quality;       /**< 0 无、1 2D、2 3D。 */
  uint8_t  hdop_x10;         /**< HDOP ×10；0 表示未知。 */
  /**
   * @brief UBX-NAV-PVT 带来的量（NMEA 没有这几个）。见 docs/gnss_speed_filter.md 步骤 2。
   *
   * @details
   * `pvt_valid` 为假时下面四项无意义（模组没开 PVT，或最近 2.5 s 没有帧）。
   *
   * `speed_centi_kmh` 保持原义 = NMEA RMC 那份**未滤波**的多普勒；这里另给一份
   * 模块**滤波过**的 `gSpeed` 和它自己的精度估计 `sAcc`。服务层只报事实，
   * 信谁由消费方（码表显示）决定，见 bicycle_runtime.c 的多普勒选源与 sAcc 门。
   */
  uint16_t speed_pvt_centi_kmh; /**< PVT gSpeed（0.01 km/h）。 */
  uint16_t speed_acc_mm_s;      /**< PVT sAcc：速度精度 1σ，mm/s。 */
  uint32_t pos_acc_mm;          /**< PVT hAcc：水平位置精度 1σ，mm。 */
  uint8_t  pvt_fix_type;        /**< PVT fixType：0 无 / 2 2D / 3 3D。 */
  bool     pvt_gnss_ok;         /**< PVT flags.gnssFixOK：模组自认定位可用。 */
  bool     pvt_valid;           /**< 上面四项来自最近一帧 PVT 且够新。 */
  /**
   * @brief 按星座分组的卫星数（索引见 #myvendor_sys_gnss_const_e）。
   *
   * @details
   * 由 NMEA 的**发话者**分出来（`$GPGSV`/`$GLGSV`/… 一种星座一句）：
   * 在视 = GSV 的 total_sats，锁定 = GSA 里参与解算的星数、heard = GSV 中 SNR>0。
   * 供 UI 的"系统资源 → 卫星"页面用，也是确认"哪几个星座真在跑"的唯一证据
   * （本固件不回 UBX-CFG-VALGET，星座开关读不回来）。
   */
  uint8_t  sats_in_view_c[MYVENDOR_SYS_GNSS_CONST_N];
  uint8_t  sats_locked_c[MYVENDOR_SYS_GNSS_CONST_N];
  uint8_t  sats_heard_c[MYVENDOR_SYS_GNSS_CONST_N];
  /**
   * @brief 模组身份（UBX-MON-VER）：告诉 UI"这一路是模组不支持，还是当前没数据"。
   *
   * @details
   * `mod_const_mask` 的位按 #myvendor_sys_gnss_const_e 排；**0 = 还没读到**
   * （读之前不要据此下结论）。`mod_gnss` 是 MON-VER 扩展串里那条星座列表原文
   * （形如 `GPS;GLO;GAL;BDS`），`mod_fw` 是固件串（如 `ROM SPG 5.10 …`）。
   */
  uint8_t  mod_const_mask;
  char     mod_gnss[24];
  char     mod_fw[24];
  uint32_t utc_sec;           /**< Unix 时间；无定位时也可有。 */
  uint8_t  rx_hz;             /**< 近 1s NMEA 接收率。 */
  /**
   * @brief 读线程健康与"为什么卡住"（给 UI 的"卫星"页，2026-09-18 加）。
   *
   * @details 骑行时没有串口，这套诊断必须能上屏 —— 否则"模组不定位"永远只能
   *          事后翻日志。字段由 **消费侧**（myvendor_gnss_get）在返回快照时
   *          现填，不是读线程 publish 的：读线程卡死时 publish 根本不会再跑，
   *          那样恰好把要看的信息冻在卡死前。
   *
   *          - `rd_state`：#myvendor_sys_gnss_health_e；
   *          - `rd_hb_age_ms`：读线程心跳年龄（正常 <1 s，>12 s 视为卡死）；
   *          - `rd_in_poll_ms`：非 0 = 有一次 `poll()` 进去没出来 —— 卡在
   *            驱动里那两把无超时、信号免疫的互斥量上（见
   *            docs/gnss_ble_diag_log.md §2.2）；
   *          - `rd_restarts`：diag 触发过的重启次数；
   *          - `rd_phase`：卡在哪一段（字符串字面量，任何线程读都安全）；
   *          - `uart_*`：驱动器侧证据 —— `uart_tx_hold`/`uart_rx_hold` 是
   *            USART2 的 xmit/recv 锁持有者 pid（-1 = 空闲、-2 = 未知），
   *            `uart_open_cnt`/`uart_close_cnt` 是端口的打开次数与"最后一次
   *            close"次数（close 会把那两把锁强重置，是唯一能把它们弄坏的路径）。
   */
  uint8_t     rd_state;
  uint8_t     rd_restarts;
  uint16_t    uart_open_cnt;
  uint16_t    uart_close_cnt;
  uint32_t    rd_hb_age_ms;
  uint32_t    rd_in_poll_ms;
  const char *rd_phase;
  int16_t     uart_tx_hold;
  int16_t     uart_rx_hold;
  /**
   * @brief 本份数据的采样时刻（CLOCK_MONOTONIC 毫秒）；0 表示未知。
   *
   * @details
   * 消费方用 myvendor_mono_elapsed_ms(now, stamp_ms) 自算年龄。存在的意义：
   * `satellites` / `sats_heard` / `sats_in_view` 都是"只增不减"的粘滞字段
   * （GSV 窗口里 `if (>0)` 才更新），而且 myvendor_gnss_get() 在 idle / 灌
   * 星历 / 倒库 / 星历宽限窗口内会**故意**返回旧的 g_fix 并把 alive 置真。
   * 没有这个时间戳，消费方无法区分"刚采到"和"恢复期里的旧值"。
   */
  uint32_t stamp_ms;
} myvendor_sys_gnss_t;

/**
 * @brief HR / CSC / CPS（各字段独立过期）。
 */
typedef struct myvendor_sys_sensor_s
{
  bool     hr_valid;
  bool     cadence_valid;
  bool     power_valid;
  uint16_t hr_bpm;
  uint16_t cadence_rpm;
  uint16_t power_w;
} myvendor_sys_sensor_t;

/**
 * @brief USB 控制器与 MTP worker（worker 不是 ctl mtp 开关）。
 */
typedef struct myvendor_sys_usb_s
{
  bool    controller_on;  /**< PHY + USBC 时钟。 */
  bool    worker_running; /**< mtp_simple 任务。 */
  uint8_t plug;           /**< MYVENDOR_SYS_USB_PLUG_*。 */
} myvendor_sys_usb_t;

/**
 * @brief 三轴向量（加速度 m/s²，陀螺 rad/s，磁场 µT）。
 */
typedef struct myvendor_sys_vec3_s
{
  bool  valid;
  float x;
  float y;
  float z;
} myvendor_sys_vec3_t;

/**
 * @brief BMP388 气压。
 */
typedef struct myvendor_sys_baro_s
{
  bool  valid;
  float hpa;     /**< 百帕。 */
  float temp_c;  /**< 芯片温度。 */
} myvendor_sys_baro_t;

/**
 * @brief MMC5983 磁力计（未做倾角补偿，不是航向融合）。
 */
typedef struct myvendor_sys_mag_s
{
  bool  valid;
  float x;       /**< µT。 */
  float y;
  float z;
  float temp_c;
} myvendor_sys_mag_t;

/**
 * @brief 一次整机只读快照。
 */
typedef struct myvendor_sys_snapshot_s
{
  myvendor_sys_power_t  power;
  myvendor_sys_gnss_t   phone;     /**< 手机 0xFF14，5 s 过期。 */
  myvendor_sys_gnss_t   onboard;   /**< 板载 MAX-M10S-00B-01；过期则 alive=false。 */
  myvendor_sys_sensor_t sensor;
  myvendor_sys_usb_t    usb;
  myvendor_sys_vec3_t   accel;     /**< BMI270，m/s²。 */
  myvendor_sys_vec3_t   gyro;      /**< BMI270，rad/s。 */
  myvendor_sys_baro_t   baro;      /**< BMP388。 */
  myvendor_sys_mag_t    mag;       /**< MMC5983。 */
  bool     companion_alive; /**< ble_companion 主循环。 */
  bool     policy_radio;    /**< persist.bt.radio。 */
  bool     policy_sensor;   /**< persist.bt.sensor。 */
  bool     policy_mtp;      /**< persist.usb.mtp。 */
  bool     policy_notif;    /**< persist.ui.notif。 */
  bool     policy_calls;    /**< persist.ui.notif_calls。 */
  bool     policy_sound;    /**< persist.ui.sound。 */
  bool     policy_eph_auto; /**< persist.gnss.eph_auto。 */
  uint8_t  bl_pct;          /**< 策略背光。 */
  uint8_t  bl_applied;      /**< 当前 PWM。 */
  uint8_t  bl_max;          /**< 硬件夹紧上限。 */
} myvendor_sys_snapshot_t;

/**
 * @brief 从各 owner 组装一份快照（不写 persist）。
 *
 * IMU/气压/罗盘/电量来自板卡传感器线程缓存，本调用不触发 I2C 或 ADC。
 *
 * @param out 输出；NULL 则忽略。
 */
void myvendor_sys_snapshot(myvendor_sys_snapshot_t *out);

/**
 * @brief 向 stdout 打印快照（NSH `sys`）。
 */
void myvendor_sys_dump(void);

/**
 * @brief 电量百分比（QFN 脚 20 VBATS / GPADC CH7，3.00 V～满电校准 OCV 曲线）。
 *
 * 只返回板卡传感器线程已发布槽。满电点来自充电→充满时的 VBATS。
 * @return -1 未知，否则 0～100。
 */
int myvendor_sys_battery_percent(void);

/**
 * @brief 电池电压（已发布槽的 VBATS 毫伏）。
 * @return >=0 毫伏，负值为尚未采到。
 */
int myvendor_sys_battery_mv(void);

/**
 * @brief 同 `myvendor_sys_battery_percent()`（历史别名，不采样）。
 */
int myvendor_sys_battery_percent_cached(void);

/**
 * @brief 电源综合状态。
 * @return MYVENDOR_SYS_PWR_*。
 */
uint8_t myvendor_sys_power_state(void);

/**
 * @brief 拷贝未过期的 HR/CSC/CPS。
 * @param out 输出；NULL 则忽略。
 */
void myvendor_sys_sensor_get(myvendor_sys_sensor_t *out);

/**
 * @brief 同上，但**不阻塞**（UI 线程用）。
 * @return 取到快照返回 true；bridge 锁忙（BLE 侧在写）返回 false 且不改 @p out。
 */
bool myvendor_sys_sensor_get_try(myvendor_sys_sensor_t *out);

#define MYVENDOR_SYS_SENSOR_KIND_HR   0u
#define MYVENDOR_SYS_SENSOR_KIND_CSC  1u
#define MYVENDOR_SYS_SENSOR_KIND_CPS  2u
#define MYVENDOR_SYS_SENSOR_KIND_N    3u
#define MYVENDOR_SYS_SENSOR_NAME_MAX  24u
#define MYVENDOR_SYS_SENSOR_FOUND_MAX 16u
#define MYVENDOR_SYS_SENSOR_ADDR_LEN  6u

#define MYVENDOR_SYS_SENSOR_LINK_IDLE       0u
#define MYVENDOR_SYS_SENSOR_LINK_CONNECTING 1u
#define MYVENDOR_SYS_SENSOR_LINK_READY      2u

/**
 * @brief 扫描表一行。
 */
typedef struct myvendor_sys_sensor_found_s
{
  uint8_t table_idx; /**< 1-based，CONNECT 用。 */
  uint8_t kind_mask; /**< bit0 HR / bit1 CSC / bit2 CPS。 */
  int8_t  rssi;
  bool    linked;
  uint8_t addr_type;
  uint8_t addr[MYVENDOR_SYS_SENSOR_ADDR_LEN];
  char    name[MYVENDOR_SYS_SENSOR_NAME_MAX];
} myvendor_sys_sensor_found_t;

/**
 * @brief 一个类型槽位的连接态。
 */
typedef struct myvendor_sys_sensor_slot_s
{
  uint8_t link; /**< MYVENDOR_SYS_SENSOR_LINK_*。 */
  int8_t  bat_pct; /**< 0..100；未知为 -1。 */
  uint8_t addr_type;
  uint8_t addr[MYVENDOR_SYS_SENSOR_ADDR_LEN];
  char    name[MYVENDOR_SYS_SENSOR_NAME_MAX];
} myvendor_sys_sensor_slot_t;

/**
 * @brief 扫描表 + 槽位连接快照。
 */
typedef struct myvendor_sys_sensor_ui_s
{
  bool    scanning;
  uint8_t found_n;
  myvendor_sys_sensor_found_t found[MYVENDOR_SYS_SENSOR_FOUND_MAX];
  myvendor_sys_sensor_slot_t  slot[MYVENDOR_SYS_SENSOR_KIND_N];
} myvendor_sys_sensor_ui_t;

/**
 * @brief 拷贝扫描表与槽位连接态。
 */
void myvendor_sys_sensor_ui_get(myvendor_sys_sensor_ui_t *out);

/**
 * @brief 某个传感器槽位是否**已绑定**（有保存的对端地址，连不连上都算）。
 * @param kind 0 HR / 1 CSC / 2 CPS。
 * @return true = 已绑定。UI 用它决定"功率/踏频那一栏要不要显示"：绑定后无论
 *         是否连上都要留着那一栏（值显示 --），删除绑定才收掉。
 * @note 只取一个布尔，不整份 ui 快照 —— UI 每拍都问。
 */
bool myvendor_sys_sensor_bound(uint8_t kind);

/**
 * @brief 手机 Companion GATT 是否已连接（不是传感器、也不是 companion 线程存活）。
 */
bool myvendor_sys_phone_ble_connected(void);

/**
 * @brief 拷贝未过期的手机 GNSS（含搜星帧；valid 表示已定位）。
 * @param out 输出；NULL 则忽略。
 * @return 近期有帧则为 true。
 */
bool myvendor_sys_phone_gnss_get(myvendor_sys_gnss_t *out);

/**
 * @brief 拷贝板载 GNSS 快照（含搜星；valid 表示已定位）。
 * @param out 输出；NULL 则只查询是否有近期 NMEA。
 * @return 近期有 NMEA，或处于星历/倒库/静止下电等预期静默则为 true。
 */
bool myvendor_sys_onboard_gnss_get(myvendor_sys_gnss_t *out);

/**
 * @brief 把快照里的 PVT 段追加到当前行（`gs` / `sacc` / `fix` / `ok`）。
 *
 * @details
 * 给 `sys onboard` 这类一行式打印用：`pvt_valid` 为假时**什么都不打**，
 * 所以调用方可以无条件调（手机那份快照就没有 PVT）。
 *
 * `gs` 与 `speed_centi_kmh` 同为 0.01 km/h，可以直接和它比；`sacc` 是速度的
 * 1σ 精度（m/s），用来判断"这一秒的速度能不能信"——见
 * docs/gnss_speed_filter.md 步骤 2，以及 bicycle_runtime.c 里的 sAcc 门。
 */
void myvendor_sys_gnss_print_pvt(const myvendor_sys_gnss_t *g);

/**
 * @brief 时钟链路快照：SYSCLK/HCLK/DLL 与 PSRAM、NAND 的实际频率。
 */
typedef struct myvendor_sys_clk_s
{
  uint32_t sysclk_hz;       /**< HCPU SYSCLK。 */
  uint32_t hclk_hz;         /**< HCPU HCLK。 */
  uint8_t  dll1_en;         /**< DLL1 使能位（读寄存器，不改配置）。 */
  uint32_t dll1_hz;         /**< DLL1 频率；未使能为 0。 */
  uint8_t  dll2_en;         /**< DLL2 使能位。 */
  uint32_t dll2_hz;         /**< DLL2 频率；未使能为 0。 */

  /** FLASH1 = PSRAM XIP：hz = 时钟源 / psclr。 */
  uint32_t flash1_hz;
  uint8_t  flash1_src;      /**< 0=SYS 1=DLL1 2=DLL2 3=DLL3。 */
  uint8_t  flash1_psclr;    /**< 硬件 MPI PSCLR.DIV，**实际生效**的分频。 */
  uint8_t  flash1_sw_div;   /**< 软件变量 BSP_GetFlash1DIV()，仅用于对照。 */

  /** FLASH2 = NAND。 */
  uint32_t flash2_hz;
  uint8_t  flash2_src;
  uint8_t  flash2_psclr;
  uint8_t  flash2_sw_div;

  /** psclr 与 sw_div 不一致（软件变量已过期）；打印时值得标出来。 */
  bool     mismatch;
} myvendor_sys_clk_t;

/**
 * @brief 现算一份时钟链路快照。
 *
 * @details
 * 不复用 flash_get_freq()：那个用 BSP_GetFlash1DIV()/2DIV() 这个**软件变量**
 * 做分频，而该变量在 bsp_init.c 里被改过多次，HAL_MPI_PSRAM_Init 只在初始化
 * 时把值写进 PSCLR，之后软件与硬件可能不一致（启动日志里 NAND 就是
 * sw_div=1 / hw_psclr=4）。本函数直接读硬件 PSCLR，并把软件值一并报出来对照。
 *
 * 公式：内存时钟 = 时钟源频率 / MPI PSCLR.DIV。
 * PDIV1/PDIV2 不参与 —— 整个 HAL 里没有任何地方用它们算频率。
 *
 * 只读寄存器，不改任何时钟配置。
 *
 * @param out 输出；NULL 则忽略。
 */
void myvendor_sys_clk_get(myvendor_sys_clk_t *out);

/**
 * @brief 时钟源名称（"SYS"/"DLL1"/"DLL2"/"DLL3"/"?"）。
 *
 * @param src myvendor_sys_clk_t::flashN_src。
 * @return 静态字符串。
 */
const char *myvendor_sys_clk_src_name(uint8_t src);

/**
 * @brief 板载 GNSS 正在注入星历，或刚灌完在等 NMEA（主界面「星历同步」）。
 */
bool myvendor_sys_gnss_eph_busy(void);

#define MYVENDOR_SYS_NAV_MAX_PTS  32u  /**< 手机下发折线点数上限。 */

/**
 * @brief 取出挂起的导航折线（一次性，手机 GATT 0xFF19）。
 * @param lat_e7 纬度 ×1e7；容量至少 @ref MYVENDOR_SYS_NAV_MAX_PTS。
 * @param lon_e7 经度 ×1e7。
 * @param count 输出点数。
 * @return 拷贝到路线则为 true。
 */
bool myvendor_sys_nav_take(int32_t *lat_e7, int32_t *lon_e7, uint8_t *count);

#define MYVENDOR_SYS_NOTIF_TITLE_MAX  48u
#define MYVENDOR_SYS_NOTIF_BODY_MAX   96u
#define MYVENDOR_SYS_NOTIF_ICON_MAX   128u
#define MYVENDOR_SYS_INBOX_MAX        8u

#define MYVENDOR_SYS_NOTIF_TYPE_GENERIC   0u
#define MYVENDOR_SYS_NOTIF_TYPE_CALL      1u
#define MYVENDOR_SYS_NOTIF_TYPE_SMS       2u
#define MYVENDOR_SYS_NOTIF_TYPE_APP       3u
#define MYVENDOR_SYS_NOTIF_TYPE_CALENDAR  4u

/**
 * @brief 一条手机通知。
 */
typedef struct myvendor_sys_notif_s
{
  uint8_t type;  /**< MYVENDOR_SYS_NOTIF_TYPE_*。 */
  char    title[MYVENDOR_SYS_NOTIF_TITLE_MAX + 1u];
  char    body[MYVENDOR_SYS_NOTIF_BODY_MAX + 1u];
  char    icon[MYVENDOR_SYS_NOTIF_ICON_MAX];
} myvendor_sys_notif_t;

/**
 * @brief 取出下一条待弹横幅（一次性）。
 */
bool myvendor_sys_notif_take(myvendor_sys_notif_t *out);

/**
 * @brief 拷贝最近通知（新到旧）。
 * @param out 可为 NULL（只取条数）。
 * @param n 输出条数。
 * @param maxn out 容量。
 */
void myvendor_sys_inbox_get(myvendor_sys_notif_t *out, uint8_t *n, uint8_t maxn);

/**
 * @brief BMI270 加速度（m/s²）。
 */
void myvendor_sys_accel_get(myvendor_sys_vec3_t *out);

/**
 * @brief BMI270 陀螺（rad/s）。
 */
void myvendor_sys_gyro_get(myvendor_sys_vec3_t *out);

/**
 * @brief BMP388 气压。
 */
void myvendor_sys_baro_get(myvendor_sys_baro_t *out);

/**
 * @brief MMC5983 磁力计（µT）。
 */
void myvendor_sys_mag_get(myvendor_sys_mag_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_SYS_H */
