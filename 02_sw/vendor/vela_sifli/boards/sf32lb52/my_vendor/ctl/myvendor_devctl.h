/**
 * @file myvendor_devctl.h
 * @brief 系统控制面（写）：背光、手机 BLE 广播、传感器 Central、USB 控制器。
 *
 * 产品写入口：UI 与 NSH `ctl` 都走本文件。读走 `myvendor_sys.h` / NSH `sys`。
 * NSH `test *` 是外设探针，不经本层。不要走 `myvendor_bicycle_ctl`（那是 UI
 * 消息队列）。策略落在 `/mnt/kv/db/persist.*`（含 `persist.boot.*`；2SFBL
 * 路径为 `/db/persist.boot.*`），执行器仍在 PWM / companion_bridge /
 * USB 控制器（`ctl mtp`）。菜单搜表/点选连接仍只投递 companion 队列；连上过的
 * HR/CSC/CPS 地址记在 `/mnt/kv/bicycle_sensors.tsv`，供开机回连。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_DEVCTL_H
#define MYVENDOR_DEVCTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYVENDOR_DEVCTL_BL_KEY      "persist.display.brightness"  /**< 背光百分比。 */
#define MYVENDOR_DEVCTL_RADIO_KEY   "persist.bt.radio"            /**< 手机广播 0/1。 */
#define MYVENDOR_DEVCTL_SENSOR_KEY  "persist.bt.sensor"           /**< 传感器 Central 0/1。 */
#define MYVENDOR_DEVCTL_MTP_KEY     "persist.usb.mtp"             /**< USB 控制器 0/1。 */
#define MYVENDOR_DEVCTL_NOTIF_KEY   "persist.ui.notif"            /**< 手机通知总开关 0/1。 */
#define MYVENDOR_DEVCTL_CALLS_KEY   "persist.ui.notif_calls"      /**< 仅来电 0/1。 */
#define MYVENDOR_DEVCTL_SOUND_KEY   "persist.ui.sound"            /**< 蜂鸣器提示音 0/1。 */
#define MYVENDOR_DEVCTL_AUTOPAUSE_KEY "persist.ui.autopause"      /**< 骑行自动暂停 0/1。 */
#define MYVENDOR_DEVCTL_LAST_RIDE_M_KEY "persist.ride.last_m"     /**< 上次保存骑行里程，米。 */
#define MYVENDOR_DEVCTL_LAST_LON_E7_KEY "persist.ride.last_lon_e7" /**< 上次骑行终点经度 ×1e7。 */
#define MYVENDOR_DEVCTL_LAST_LAT_E7_KEY "persist.ride.last_lat_e7" /**< 上次骑行终点纬度 ×1e7。 */
#define MYVENDOR_DEVCTL_EPH_AUTO_KEY "persist.gnss.eph_auto"      /**< 开机注入/定位后倒库 0/1。 */
#define MYVENDOR_DEVCTL_GNSS_RMC_KEY "persist.gnss.solver_rmc"    /**< 1=完全信 RMC，0=自定义滤波。 */
#define MYVENDOR_DEVCTL_GRADE_KEY   "persist.ui.grade_off_mdeg"   /**< 坡度零点，毫度。 */
#define MYVENDOR_DEVCTL_TZ_KEY     "persist.ui.tz_min"            /**< 时区，相对 UTC 的分钟。 */
#define MYVENDOR_DEVCTL_BAT_FULL_KEY "persist.battery.full_mv"     /**< 满电校准：100% 对应毫伏。 */
#define MYVENDOR_DEVCTL_PWR_KEY     "persist.boot.pwr"            /**< 1=2SFBL 不判 PWR 键，直接跳转。 */
/** 启动槽位：`fw` = SD 上 `/mnt/kv/fw/` 目录里的 `*.bin`（产品路径，bootloader 按版本取最新）；
 *  `main` = **本机被 `flash` 写进去的 main 分区**；`factory` = 工厂槽。
 *
 *  2SFBL 的选择顺序是 `persist.boot.target` → `/fw` → `main` → `factory`，
 *  所以这里写了什么、**下一次开机**就走什么 —— 改完不复位不生效。
 *  （现场踩过：`flash` 明明成功、`build_date` 却不变，就是因为它还在走 `fw`。）
 */
#define MYVENDOR_DEVCTL_BOOT_TARGET_KEY "persist.boot.target"
/** 槽位名长度上限（含结尾 NUL）；`fw`/`main`/`factory` 都远小于它。 */
#define MYVENDOR_DEVCTL_BOOT_TARGET_MAX 16
#define MYVENDOR_DEVCTL_LOG_KEY     "persist.log.mask"            /**< syslog 屏蔽位，见 ctl log。 */
/** framework 的 BT_LOG* 档位，见 `ctl btlog`。
 *
 *  这个 key 是**框架自己定的**：`service/utils/log_server.c` 的
 *  `PERSIST_BT_FRAMEWORK_LOG_LEVEL`，它用 `property_monitor` 监听这个 key
 *  来热加载等级。所以这里必须写字面量同一个串 —— 改成别的名字，
 *  `ctl btlog` 就变成"写了个没人读的值"。 */
#define MYVENDOR_CTL_BTLOG_KEY      "persist.bluetooth.log.level"
/** 「充电时也按负载动态调频」模式（`ctl dvfs charge on|off`），见 `sf32lb_dvfs.h`。
 *  默认 0 = 关（原策略：充电把 HCLK 钉在 240）。 */
#define MYVENDOR_DEVCTL_DVFS_CHARGE_DYN_KEY "persist.dvfs.charge_dyn"
/** diag 周期计数器报表的模块开关掩码（`ctl cnt`），位序见 `myvendor_diag.c`
 *  的 `g_cnt_mods[]`。**KV 里没有这条 key ⇒ 全开**（所有位 1）。 */
#define MYVENDOR_DEVCTL_DIAG_CNT_KEY "persist.diag.cnt_mask"

/** @brief 读/写"周期计数器报表"开关掩码（`ctl cnt`）；改动落 KV。 */
uint32_t myvendor_devctl_diag_cnt_mask_get(void);
int myvendor_devctl_diag_cnt_mask_set(uint32_t mask);

#define MYVENDOR_DEVCTL_BL_DEFAULT  16u  /**< KV 缺失：PWM 16 = UI 20%。 */
#define MYVENDOR_DEVCTL_TZ_DEFAULT 480  /**< 默认 UTC+8。 */
#define MYVENDOR_DEVCTL_TZ_MIN     (-12 * 60)
#define MYVENDOR_DEVCTL_TZ_MAX     (14 * 60)
#define MYVENDOR_DEVCTL_BAT_FULL_DEFAULT     4100  /**< 未校准：本机 CV 约 4.10 V。 */
#define MYVENDOR_DEVCTL_BAT_FULL_MARGIN_MV   20    /**< 观测 4.10 V → 100% 用 4.08 V。 */
#define MYVENDOR_DEVCTL_BAT_FULL_DEADBAND_MV 50    /**< |新校准−已存|≥50 mV 才重写。 */
#define MYVENDOR_DEVCTL_BAT_FULL_OBS_MIN     4050  /**< 充满观测过低则拒绝（防未充满当满电）。 */
#define MYVENDOR_DEVCTL_BAT_FULL_OBS_MAX     4650  /**< CH7×k≈3.3 时脚 4.2 V 约 4.4 V。 */
#define MYVENDOR_DEVCTL_BAT_FULL_OK_MIN      4030  /**< 已存 KV 合法下限（观测 − margin）。 */
#define MYVENDOR_DEVCTL_BAT_FULL_OK_MAX      4630

#define MYVENDOR_DEVCTL_FAVORITES_PATH "/mnt/kv/bicycle_favorites.tsv"
#define MYVENDOR_DEVCTL_FAVORITE_MAX 32u
#define MYVENDOR_DEVCTL_FAVORITE_NAME_MAX 48u

#define MYVENDOR_DEVCTL_SENSORS_PATH "/mnt/kv/bicycle_sensors.tsv"
#define MYVENDOR_DEVCTL_SENSOR_REC_MAX 16u
#define MYVENDOR_DEVCTL_SENSOR_ADDR_LEN 6u
#define MYVENDOR_DEVCTL_SENSOR_NAME_MAX 24u

typedef struct
{
  char name[MYVENDOR_DEVCTL_FAVORITE_NAME_MAX];
  double latitude;
  double longitude;
} myvendor_devctl_favorite_t;

/**
 * @brief 曾连接过的 HR/CSC/CPS。autorc 每种（HR/CSC/CPS）至多一条。
 */
typedef struct
{
  uint8_t kind;       /**< 0 HR / 1 CSC / 2 CPS。 */
  uint8_t addr_type;  /**< LE 地址类型（public / random）。 */
  uint8_t addr[MYVENDOR_DEVCTL_SENSOR_ADDR_LEN];
  char    name[MYVENDOR_DEVCTL_SENSOR_NAME_MAX];
  bool    autorc;     /**< 开机自动回连。 */
} myvendor_devctl_sensor_rec_t;

/**
 * @brief 从 persist.* 载入 RAM（缺文件用默认值），并 PrefPct 背光。
 *
 * 工厂固件不读 persist.usb.mtp / persist.bt.radio，直接开 MTP 与手机 BLE。
 * PrefPct 记住 persist 亮度；2SFBL 衔接触屏时不要关背光。
 */
void myvendor_devctl_load(void);

/**
 * @brief 立刻改 PWM 和 RAM 策略，不写 persist。
 *
 * 连点调光用这个，避免每次点击打 KV。PWM 由 UI 线性过渡；
 * 5 秒无操作后再 @ref myvendor_devctl_bl_commit。
 */
int myvendor_devctl_bl_apply(uint8_t pct);

/**
 * @brief 把当前 RAM 亮度写入 persist（无变化则跳过）。
 */
void myvendor_devctl_bl_commit(void);

/**
 * @brief 设置背光百分比并立刻写入 persist。
 * @param pct 0～100；0 为关，超过硬件上限会被夹紧。
 * @return 0 成功，负值为 PWM 错误码。
 */
int myvendor_devctl_bl_set(uint8_t pct);

/**
 * @brief 读取策略侧背光百分比（未必等于当前 PWM）。
 * @return 0～100。
 */
uint8_t myvendor_devctl_bl_get(void);

/**
 * @brief 硬件允许的最大背光百分比（限流夹紧）。
 * @return 1～100。
 */
uint8_t myvendor_devctl_bl_max(void);

/**
 * @brief 开关手机 Companion 广播（GATT 0xFF10）。
 * @param on true 允许广播，false 停 adv（不关适配器）。
 * @return 0 成功，-ENOTSUP 本镜像未编 BLE companion。
 */
int myvendor_devctl_radio_set(bool on);

/**
 * @brief 手机广播策略是否为开。
 */
bool myvendor_devctl_radio_get(void);

/**
 * @brief 开关 HR/CSC/CPS Central（扫描 + 自动连接）。
 * @param on true 启动传感器，false 停止。
 * @return 0 成功，-ENOTSUP 本镜像未编 BLE companion。
 */
int myvendor_devctl_sensor_set(bool on);

/**
 * @brief 传感器 Central 策略是否为开。
 */
bool myvendor_devctl_sensor_get(void);

#define MYVENDOR_DEVCTL_SENSOR_SCAN_MS_DEFAULT  30000u  /**< 菜单搜表默认窗口。 */
#define MYVENDOR_DEVCTL_SENSOR_KIND_HR          0u
#define MYVENDOR_DEVCTL_SENSOR_KIND_CSC         1u
#define MYVENDOR_DEVCTL_SENSOR_KIND_CPS         2u
#define MYVENDOR_DEVCTL_SENSOR_KIND_N           3u  /**< 槽位数；与 myvendor_sys / ble_sensor 同序。 */

/**
 * @brief 限时观察者搜表（不自动 GATT 连接）。
 * @param ms 时长；0 表示 @ref MYVENDOR_DEVCTL_SENSOR_SCAN_MS_DEFAULT。
 * @return 0 成功，-ENOTSUP 本镜像未编 BLE companion。
 */
int myvendor_devctl_sensor_scan(uint32_t ms);

/**
 * @brief 只停观察者；已连上的 GATT 保持。
 */
int myvendor_devctl_sensor_scan_stop(void);

/**
 * @brief 开配对窗口（[secs] 秒，0 = 默认 30 s）。
 *
 * 窗口内设备**可被发现、可被连接**（配对就靠这段）；窗口外没绑定记录时**完全不广播**
 * （射频让给传感器扫描）。设备 UI「蓝牙设备 → 手机蓝牙」页和 `ctl pair open` 都走这里。
 */
int myvendor_devctl_pair_open(uint32_t secs);

/**
 * @brief 解除与手机的绑定（默认全部）。
 */
int myvendor_devctl_pair_unbind(void);

/**
 * @brief 解除第 idx 台已绑定手机（0-based）。
 */
int myvendor_devctl_pair_unbind_at(unsigned idx);

/**
 * @brief 读"已绑定手机"记录数（最多 3）。
 */
unsigned myvendor_devctl_pair_phone_count(void);

/**
 * @brief 读第 idx 台手机地址（0-based，形如 `78D8404BF520`）。
 * @return 1 = 有；0 = 无此槽。
 */
int myvendor_devctl_pair_phone_at(unsigned idx, char *addr, size_t len);

/**
 * @brief 读第一台已绑定手机（兼容旧 UI）。
 * @return 1 = 已配对；0 = 没记录。
 */
int myvendor_devctl_pair_phone(char *addr, size_t len);

/** @brief 配对窗口剩余毫秒（0 = 没开窗）。设备 UI 的倒计时用。 */
uint32_t myvendor_devctl_pair_window_left_ms(void);

/**
 * @brief 按扫描表 1-based 下标连接（按地址，不按名）。
 * @param idx_1based 最近一次扫描表行号。
 */
int myvendor_devctl_sensor_connect(uint8_t idx_1based);

/**
 * @brief 同上，但指定要绑哪个类型槽位（菜单按当前页面的类型传）。
 * @param idx_1based 最近一次扫描表行号。
 * @param kind 0 HR / 1 CSC / 2 CPS；@ref COMPANION_SENSOR_KIND_AUTO
 *             （0xff）表示由服务侧取 ADV 里第一个匹配的类型。
 */
int myvendor_devctl_sensor_connect_kind(uint8_t idx_1based, uint8_t kind);

/**
 * @brief 按 **MAC** 连接一台扫描到的设备（**设备端 UI 点击首选这条**）。
 *
 * @details 与 `_connect_kind()` 的差别只在寻址方式：后者传扫描表下标，
 *          而那张表是活的 —— 扫描回调并发改写它，且**每次用户起扫整表清空**。
 *          于是"界面显示第 N 项"到"底层按 idx=N 查表"之间只要表变过，
 *          `g_found[idx-1]` 就指到别的设备上了。MAC 在显示那一刻就已确定。
 *          服务侧复用回连记录那条路径（`CONNECT_ADDR` → `ble_sensor_connect_addr`）。
 *
 * @param addr      6 字节 LE 地址（不能为 NULL）。
 * @param addr_type 地址类型（随扫描结果给出）。
 * @param kind      绑到哪个类型槽位（0 HR / 1 CSC / 2 CPS）。
 * @param name      设备名，可为 NULL。
 * @return 0 已投递；-EINVAL 参数非法；-ENOTSUP 未编 companion。
 */
int myvendor_devctl_sensor_connect_addr(const uint8_t *addr, uint8_t addr_type,
                                        uint8_t kind, const char *name);

/**
 * @brief 断开一个类型槽位（HR=0 / CSC=1 / CPS=2）。
 */
int myvendor_devctl_sensor_disconnect_kind(uint8_t kind);

/**
 * @brief 开关 USB 控制器（PHY + USBC 时钟 + MTP gadget）。
 *
 * 不停 BLE 文件传输，也不拆 `mtp_simple` worker。off 后主机看不到设备。
 *
 * @param on true 上电并可枚举，false 下电省电。
 * @return 0 成功，负值为 USB 错误码，-ENOTSUP 未编 USB 设备控制器。
 */
int myvendor_devctl_mtp_set(bool on);

/**
 * @brief USB 控制器策略是否为开。
 */
bool myvendor_devctl_mtp_get(void);

/**
 * @brief 导航期间临时关闭 MTP；不修改持久化开关。
 *
 * hold=true 时即使执行 `ctl mtp on` 也保持 USB 关闭；hold=false 时按当前
 * 持久化策略恢复。
 */
int myvendor_devctl_mtp_nav_hold(bool hold);

/**
 * @brief 开关手机通知（0xFF17 横幅 + 最近通知）。
 * @param on false 则丢弃到达的通知。
 */
int myvendor_devctl_notif_set(bool on);

/**
 * @brief 是否接受手机通知。
 */
bool myvendor_devctl_notif_get(void);

/**
 * @brief 仅收录来电（需通知总开关为开）。
 */
int myvendor_devctl_notif_calls_only_set(bool on);

/**
 * @brief 是否仅来电。
 */
bool myvendor_devctl_notif_calls_only_get(void);

/**
 * @brief 开关蜂鸣器提示音（按键、骑行、导航等 UI 声）。
 *
 * 关则丢弃 UI 投递；NSH `test sound` 诊断 PWM 不受本开关影响。
 * @param on true 允许出声，false 静音。
 */
int myvendor_devctl_sound_set(bool on);

/**
 * @brief 蜂鸣器策略是否为开。默认开。
 */
bool myvendor_devctl_sound_get(void);

/**
 * @brief 开关骑行自动暂停（低于速度阈值停表，恢复后继续）。
 * 默认开。
 */
int myvendor_devctl_autopause_set(bool on);

/**
 * @brief 自动暂停是否为开。
 */
bool myvendor_devctl_autopause_get(void);

/**
 * @brief 记下上次成功保存的骑行里程（米），写入 KV。开机待机页用。
 */
void myvendor_devctl_last_ride_m_set(int32_t m);

/**
 * @brief 上次保存骑行里程（米）。KV 缺失为 0。
 */
int32_t myvendor_devctl_last_ride_m_get(void);

/**
 * @brief 记下上次保存骑行的终点（WGS84 ×1e7）。非法坐标忽略。
 */
void myvendor_devctl_last_pos_set(int32_t lon_e7, int32_t lat_e7);

/**
 * @brief 读取上次骑行终点。KV 缺失或非法为 false。
 */
bool myvendor_devctl_last_pos_get(int32_t *lon_e7, int32_t *lat_e7);

/**
 * @brief 开关 GNSS 星历自动维护（开机灌最新 mga_*，定位后倒库；关机前再 dump）。
 *
 * 关掉后开机不注入、定位后不 dump、关机也不 dump。
 * App 手动写入 eph 目录下的 .ubx 仍会热加载；`ctl gnss dump_in` / `dump_out` 不受影响。
 * 默认开。
 */
int myvendor_devctl_eph_auto_set(bool on);

/**
 * @brief 星历自动维护是否为开。
 */
bool myvendor_devctl_eph_auto_get(void);

/**
 * @brief GNSS 解算：true 完全信 RMC，false 用自定义滤波。
 */
int myvendor_devctl_gnss_rmc_set(bool on);

/**
 * @brief 是否完全信 RMC。
 */
bool myvendor_devctl_gnss_rmc_get(void);

/**
 * @brief 坡度校准偏置（IMU 俯仰零点，毫度）。
 */
int myvendor_devctl_grade_offset_set(int32_t mdeg);

/**
 * @brief 读取坡度零点（毫度）。
 */
int32_t myvendor_devctl_grade_offset_get(void);

/**
 * @brief 时区偏置（相对 UTC 的分钟）。默认 +480（UTC+8）。
 */
int myvendor_devctl_tz_min_set(int16_t min);

/**
 * @brief 读取时区偏置（分钟）。
 */
int16_t myvendor_devctl_tz_min_get(void);

/**
 * @brief 只读 RAM 里的时区，不碰 KV。崩溃路径用；未 load 则为默认 UTC+8。
 */
int16_t myvendor_devctl_tz_min_peek(void);

/**
 * @brief 把 UTC unix 秒换成本地时、分。
 * @return 成功则为 true。
 */
bool myvendor_devctl_tz_hms(long utc_sec, int *hour, int *min);

/**
 * @brief 写下 syslog 屏蔽位并存 KV，下次开机自动恢复。
 *
 * @details
 * 立即调 `nx_setlogmask()` 全系统生效（遍历所有已存在任务，新任务在
 * sched/tls/task_initinfo.c 里继承），再写 `persist.log.mask`。
 * **写 KV 失败不回滚内存里的值** —— 本次开机照常按新 mask 走，只是下次
 * 开机回到旧值。
 *
 * @param mask 8 位屏蔽位（bit0=EMERG … bit7=DEBUG），如 `LOG_UPTO(LOG_INFO)`。
 * @param old  非空则回填设置前的屏蔽位（`nx_setlogmask()` 的返回值，权威）。
 * @return 0 成功；负值为 persist 写入失败的原因（值仍已生效）。
 */
int myvendor_devctl_log_mask_set(uint8_t mask, uint8_t *old);

/**
 * @brief 当前 syslog 屏蔽位（RAM 快照，不读盘）。
 *
 * @details 这是**开机载入值或本进程最后一次 set**，不是 `nx_setlogmask()`
 * 的实时回读 —— NuttX 没有 `nx_getlogmask()`。全仓只有 `ctl log` 一处写它，
 * 所以两者一致。
 */
uint8_t myvendor_devctl_log_mask_get(void);

/**
 * @brief 设置「充电时也按负载动态调频」模式，并落 KVDB。
 *
 * @details 先改运行期（`sf32lb_dvfs_charge_dyn_set()`）再落盘 —— 与 log mask
 *          同一个理由：写盘失败也要保留本次生效的行为，反过来会让调用方看到
 *          "设了却没生效"。
 * @param on true = 开。
 * @return 0 成功；负值为落盘失败（**运行期已经生效**）。
 */
int myvendor_devctl_dvfs_charge_dyn_set(bool on);

/** @brief 读取「充电时也按负载动态调频」模式当前值。 */
bool myvendor_devctl_dvfs_charge_dyn_get(void);

/**
 * @brief 读取全部常用点；与 NSH `ctl fav` 使用同一文件。
 * @param out 输出数组。
 * @param cap 输出容量。
 * @param count 实际条数。
 * @return 0 成功，负 errno 失败；文件不存在视为空列表。
 */
int myvendor_devctl_favorites_load(myvendor_devctl_favorite_t *out,
                                   size_t cap, size_t *count);

/**
 * @brief 读取任意路径上的常用点/途经点 TSV（`名字\\t纬度\\t经度`）。
 * @return 0 成功，负 errno；文件不存在视为空列表。
 */
int myvendor_devctl_waypoints_load(const char *path,
                                   myvendor_devctl_favorite_t *out,
                                   size_t cap, size_t *count);

/**
 * @brief 拷贝已连接过的传感器记录。
 * @return 0 成功；文件不存在视为空列表。
 */
int myvendor_devctl_sensor_recs_get(myvendor_devctl_sensor_rec_t *out,
                                    size_t cap, size_t *count);

/**
 * @brief 记录版本（增删改后递增），菜单用来刷新列表。
 */
uint32_t myvendor_devctl_sensor_rec_gen(void);

/**
 * @brief 打印已保存的传感器记录 + 落盘文件状态（`sys` / `sys hr` 用）。
 *
 * 「连过但开机不回连」时先看这张表：文件行说明记录有没有落盘（`missing`
 * 就是没保存成功），每条记录带 `autorc` 位（1 = 开机回连的目标）。
 */
void myvendor_devctl_sensor_rec_dump(void);

/**
 * @brief 回连对账：把 `autorc` 记录和**本次开机**的槽位目标并排打出来。
 *
 * @details
 * 回连**只**该由 `autorc` 记录驱动（武装在 `companion_sensor_autorc_all()`
 * → `ble_sensor_connect_addr()`）。所以「删了还回连」和「配对过却不回连」
 * 这两个方向相反的问题，判据在同一张表上：
 *
 *   - 记录有 ★、槽位地址却是空 → **武装没上**，开机不会回连；
 *   - 记录没有、槽位却有地址 → **有东西绕过记录在连**（删了也回来的形态）。
 *
 * 槽位侧取自 `myvendor_sys_sensor_ui_get()` 的快照（与 `sys slot` 同一份），
 * 所以这两段输出天然对齐。
 */
void myvendor_devctl_sensor_link_dump(void);

/**
 * @brief GATT READY 后记下设备，并指定为该类型的开机回连（同类型仅一条）。
 */
int myvendor_devctl_sensor_remember(uint8_t kind, const uint8_t *addr,
                                    uint8_t addr_type, const char *name);

/**
 * @brief 选定或取消开机回连。on=true 时只清掉同类型其它记录的 autorc。
 */
int myvendor_devctl_sensor_auto_set(uint8_t idx, bool on);

/**
 * @brief 取出一条开机回连记录（任意类型，先找到的一条）。
 * @return 0 成功，-ENOENT 未选择。
 */
int myvendor_devctl_sensor_auto_get(myvendor_devctl_sensor_rec_t *out);

/**
 * @brief 删除一条记录；若是开机回连目标则同时取消回连。
 */
int myvendor_devctl_sensor_delete(uint8_t idx);

/**
 * @brief 读取已校准的满电毫伏（缺文件或非法为 4100）。
 *
 * 产品 100% 映射用这个值，不是固定 4.20 V。
 */
int myvendor_devctl_bat_full_mv_get(void);

/**
 * @brief 是否已有合法的 persist.battery.full_mv。
 */
bool myvendor_devctl_bat_full_have(void);

/**
 * @brief 充满 IO 时用当前 VBATS 毫伏更新满电校准。
 *
 * 100% 点 = 观测 − 20 mV（4.43 → 4.41）。观测须在约 4.05～4.65 V：使用中
 * 掉到 3.98 V 不是满电，不会走到这里。已有 KV 时 |新−旧| ≥ 50 mV 才重写，
 * 可升可降（温度、老化、ADC），不单向锁死。
 *
 * @param mv 当前 VBATS 毫伏；非法范围则忽略。
 */
void myvendor_devctl_bat_full_note(int mv);

/**
 * @brief 软件关机：先安全倒 GNSS 导航库，放开 MTP/LFS hold，停 IWDT，拉低 PA29。
 *
 * 确认 PA29 已为低后等待 100 ms，再 HAL_PMU_Reboot。轨已掉电则不会执行到复位；
 * 插电仍活着时复位回到 2SFBL（充电动画）。
 */
void myvendor_devctl_poweroff(void);

/**
 * @brief 只拉电、不停做收尾。关机动画把星历/GPX/MTP 做完后再调。
 */
void myvendor_devctl_power_cut(void);

/**
 * @brief 调试开机：写入 persist.boot.pwr。
 *
 * 2SFBL 见到非 0 则不判 PWR 键、不进充电等待页，拉住 PA29 后直接 autoboot。
 * 给反复烧录用；量产不要开。`ctl pwr off` 写回 0。
 */
int myvendor_devctl_pwr_set(bool on);

/** @brief persist.boot.pwr 是否为开（缺文件视为关）。 */
bool myvendor_devctl_pwr_get(void);

/**
 * @brief 读启动槽位（`persist.boot.target`）。
 *
 * @param[out] out 目标缓冲，至少 @ref MYVENDOR_DEVCTL_BOOT_TARGET_MAX 字节。
 * @param[in]  n   缓冲长度。
 * @return true = 读到；false = 没设过（交给 2SFBL 走默认顺序 `/fw` → `main`
 *         → `factory`）或参数非法。
 *
 * @note 只读 KV 文件，任意线程可调。返回的槽位名**不保证是已知值** ——
 *       文件可以被 `ctl boot` 之外的途径改，所以显示时原样打出来即可。
 */
bool myvendor_devctl_boot_target_get(char *out, size_t n);

/**
 * @brief 写启动槽位（只在**下一次开机**生效，本函数不复位）。
 *
 * @param[in] target 槽位名：`fw` / `main` / `factory`（大小写不敏感）。
 * @return 0 成功；负值为 errno（写盘失败）。
 *
 * @note 会先校验是不是已知槽位 —— 写错名字的后果是 2SFBL 认不出、退回
 *       默认顺序，用户会以为"改了没生效"，所以宁可在这里拒掉。
 */
int myvendor_devctl_boot_target_set(const char *target);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_DEVCTL_H */
