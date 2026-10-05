/**
 * @file bicycle_demo.h
 * @brief 自行车 UI — 演示模式（展会/拍摄：一遍跑完四种"看点"）。
 *
 * 一遍 = 四步，跑完回到第一步循环，直到手动关掉：
 *
 * | # | 屏幕中间横幅 | 干什么 |
 * |---|--------------|--------|
 * | 1 | 模拟 GPX 导航   | 回放一条本机 GPX 当定位，再开**这段路的 GPX 跟线导航** |
 * | 2 | 模拟坐标点导航  | 同一条 GPX 回放（把定位挪回轨迹起点），再**规划到固定坐标点**并模拟行驶 |
 * | 3 | 全国地图解析    | 结束导航，在全国几个风景点上跳（每个 10 秒） |
 * | 4 | 主题切换        | 切到另一套主题（日光 ↔ 夜间） |
 *
 * @section 横幅
 * 四条提示挂在 **LVGL top layer**（`lv_layer_top()`）上：盖住地图页/菜单/导航横幅，
 * 且**只在演示期间存在**，关掉就删。它**不是**通知横幅（`lv_pm_notify_show`）——
 * 演示期间不该被"来电话/收通知"那类东西挤掉位置。
 * 每次露脸 `BICYCLE_DEMO_BAN_MS`（默认 2 秒）就自己收掉：横幅是"报户口"用的，
 * 不该一直压着地图。
 *
 * @section 怎么用
 * · 菜单：设置 → 系统 → **演示模式**（开关）
 * · 控制台：`bicycle_nsh demo on|off|status`
 *
 * @note 演示只碰"当前这一次会话"：回放/跳点都是合成定位，不写 GNSS；
 *       演示骑行结束时不落盘（会丢弃未保存的 GPX）；主题在停止时还原成开演示前那套。
 */

#ifndef BICYCLE_DEMO_H
#define BICYCLE_DEMO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name 四步各自的料（改这几个就够了）
 * @{
 */
/** @brief 第 1、3 步：每条导航演多久（毫秒；提前到达/结束会提前进下一步）。 */
#define BICYCLE_DEMO_NAV_MS        30000u
/** @brief ①→② 之间「导航结束」那一步停多久（毫秒）—— 用来把上一个导航彻底清掉。 */
#define BICYCLE_DEMO_NAV_END_MS    2000u
/**
 * @brief 第 3 步等"规划完毕"的上限（毫秒）。
 * @note 用户 2026-09-29："第二段最好在规划完毕后开始计算时间" —— 规划是异步的
 *       （route worker），几百毫秒到几秒不等；这期间计时**不走**（把起点一直往后挪），
 *       规划落地后才开始数那 `BICYCLE_DEMO_NAV_MS`。给个上限免得规划卡住把演示挂住。
 */
#define BICYCLE_DEMO_PLAN_WAIT_MAX_MS 15000u
/** @brief 第 3 步：每个点停多久（毫秒；用户定的 10 秒 = 10000）。 */
#define BICYCLE_DEMO_HOP_PERIOD_MS 10000u
/** @brief 第 4 步：换完主题停多久（毫秒）。 */
#define BICYCLE_DEMO_THEME_MS      15000u
/** @brief 横幅每次露脸多久（毫秒）——到点就自己收掉，别一直压着地图。 */
#define BICYCLE_DEMO_BAN_MS        2000u
/**
 * @brief 第 3 步**切换前**提前多久报下一个点的名字（毫秒）。
 * @note 用户要求"每个点切换前需要告诉是什么位置"：跳点是在 2 km 窗口里看地图，
 *       不点名观众不知道跳到哪了 —— 提前报，地图再跳。
 */
#define BICYCLE_DEMO_ANNOUNCE_MS   1500u
/** @brief 跑完第 4 步回到第 1 步继续（0 = 一遍就停）。展会演示要一直循环。 */
#define BICYCLE_DEMO_LOOP          1

/** @brief 第 1、2 步回放的那条轨迹（App 从 `import/` 导进来的骑行文件）。 */
#define BICYCLE_DEMO_GPX     "/mnt/lfs/mtp/import/20260529户外骑行.gpx"
/** @brief 第 2 步"坐标点导航"的目的地。 */
#define BICYCLE_DEMO_PLAN_LON 118.3762928
#define BICYCLE_DEMO_PLAN_LAT 32.2458731
/** @} */

/** @brief `bicycle_demo_start()` 的返回值。 */
enum {
    BICYCLE_DEMO_OK = 0,
    /** @brief 地图页还没建好（开机没走完 / 工厂模式）。 */
    BICYCLE_DEMO_ERR_NO_MAP = -1,
    /** @brief 导航起不来（轨迹打不开、路线规划没起来）。 */
    BICYCLE_DEMO_ERR_NAV = -2,
    /** @brief 正在记录一次真实骑行：演示会把它搅掉，拒绝。 */
    BICYCLE_DEMO_ERR_RIDING = -3,
};

/**
 * @brief 开演示模式。
 * @return #BICYCLE_DEMO_OK（含"本来就在跑"）；负值为上面的失败原因。
 * @note 菜单开着时会顺手把菜单收起（演示要看地图，与"骑行记录 → 导航"同一个姿势）。
 */
int bicycle_demo_start(void);
/**
 * @brief 关演示模式：停回放/跳点、收掉演示骑行（未保存的 GPX 丢弃）、
 *        删掉演示横幅、把主题与地图缩放恢复成开演示之前的样子。
 */
void bicycle_demo_stop(void);
/** @brief 已开则停、未开则开（菜单那一行用）。 */
void bicycle_demo_toggle(void);
/** @brief 是否在演示中。 */
bool bicycle_demo_active(void);

/**
 * @brief 当前这一步的横幅文案（菜单副标题 / `bicycle_nsh demo status` 用）。
 * @return 静态缓冲；未开时给的是这一行"没开"时该显示的说明文案。
 */
const char * bicycle_demo_status(void);
/**
 * @brief 失败原因文案（通知横幅）。
 */
const char * bicycle_demo_error_text(int rc);

/**
 * @brief UI 主循环每圈调用（循环顶，与 `bicycle_ui_ctl_poll()` 同处）。
 * @note 未开演示时是空操作；开着时只在某一步到点时推进一步。
 */
void bicycle_demo_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_DEMO_H */
