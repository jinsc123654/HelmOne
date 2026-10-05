/**
 * @file myvendor_lcd_disp.h
 * @brief LCD 刷新区管道（在自行车 App 之外）：UI 提交 SRAM 条带指针，ui_flush 零拷贝 PUTAREA/DMA。
 *
 * 两块 LVGL 局部缓冲乒乓：worker 传上一片时，UI 可填下一片。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __MYVENDOR_LCD_DISP_H
#define __MYVENDOR_LCD_DISP_H

#include <nuttx/lcd/lcd_dev.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef MYVENDOR_LCDDEVIO_ABORT_XFER
#  define MYVENDOR_LCDDEVIO_ABORT_XFER   _LCDIOC(32)  /**< 中止进行中的 DMA。 */
#endif
#ifndef MYVENDOR_LCDDEVIO_RESTART_XFER
#  define MYVENDOR_LCDDEVIO_RESTART_XFER _LCDIOC(33)  /**< 重启 DMA 管道。 */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYVENDOR_LCD_DISP_WAIT_MS
#  define MYVENDOR_LCD_DISP_WAIT_MS 2000u  /**< flush_wait 默认超时（毫秒）。 */
#endif

/** DMA/PUTAREA 完成回调。 */
typedef void (*myvendor_lcd_disp_done_cb_t)(void *user_data);

/** DMA 管道重启后：(1) 请求 LVGL 全屏重绘。 */
typedef void (*myvendor_lcd_disp_refr_cb_t)(void *user_data);

/** DMA 管道重启后：(2) lv_display_flush_ready，放开 UI 等待。 */
typedef void (*myvendor_lcd_disp_unblock_cb_t)(void *user_data);

/**
 * @brief 刷新矩形（含端点，与 LVGL area 一致）。
 */
typedef struct
{
  int16_t x1;  /**< 左。 */
  int16_t y1;  /**< 上。 */
  int16_t x2;  /**< 右（含）。 */
  int16_t y2;  /**< 下（含）。 */
} myvendor_lcd_disp_area_t;

/**
 * @brief 面板与条带几何，供 LVGL 分配缓冲。
 */
typedef struct
{
  uint16_t hor_res;   /**< 水平像素。 */
  uint16_t ver_res;   /**< 垂直像素。 */
  uint16_t strip_rows;/**< 单条带行数。 */
  uint32_t stride;    /**< 每行字节。 */
  size_t   buf_bytes; /**< 单缓冲字节数。 */
} myvendor_lcd_disp_info_t;

/**
 * @brief 打开 `/dev/lcd0` 并拉起 ui_flush worker。
 * @param lcd_path 设备路径，NULL 则用默认 `/dev/lcd0`。
 * @return 0 成功，负值为错误码。
 */
int myvendor_lcd_disp_init(const char *lcd_path);

/**
 * @brief 停 worker 并关闭设备。
 */
void myvendor_lcd_disp_deinit(void);

/**
 * @brief 读取面板几何。
 * @param[out] info 输出。
 * @return 0 成功。
 */
int myvendor_lcd_disp_get_info(myvendor_lcd_disp_info_t *info);

/**
 * @brief 注册 DMA 完成回调。
 */
int myvendor_lcd_disp_register_done_cb(myvendor_lcd_disp_done_cb_t cb,
                                       void *user_data);

/**
 * @brief 取消 DMA 完成回调。
 */
void myvendor_lcd_disp_unregister_done_cb(void);

/**
 * @brief 注册管道恢复后的重绘 / unblock 回调。
 */
int myvendor_lcd_disp_register_recover_cbs(myvendor_lcd_disp_refr_cb_t refr_cb,
                                           myvendor_lcd_disp_unblock_cb_t
                                               unblock_cb,
                                           void *user_data);

/**
 * @brief 取消恢复回调。
 */
void myvendor_lcd_disp_unregister_recover_cbs(void);

/**
 * @brief 排队一次刷新：等到 ui_flush 空闲，传入 LVGL SRAM 指针（零拷贝）。
 * @param area 矩形。
 * @param color_p 像素缓冲。
 * @param stride_bytes 行跨距。
 * @param is_last 本帧最后一条带。
 * @return 0 成功，负值为繁忙/错误。
 */
int myvendor_lcd_disp_submit_flush(const myvendor_lcd_disp_area_t *area,
                                   const void *color_p,
                                   uint32_t stride_bytes,
                                   bool is_last);

/**
 * @brief worker 在忙、有排队任务、或正在 recover。
 */
bool myvendor_lcd_disp_flush_busy(void);

/**
 * @brief 等到刷新区空闲。
 * @param max_ms 最长等待；0 表示等到 ui_flush 结束。
 * @param[out] waited_ms_out 实际等待毫秒，可为 NULL。
 * @return true 已空闲，false 超时。
 */
bool myvendor_lcd_disp_wait_idle(uint32_t max_ms, uint32_t * waited_ms_out);

/**
 * @brief 刷新区计数器（诊断）。
 */
typedef struct
{
  uint32_t submit_ok;         /**< 提交成功。 */
  uint32_t submit_busy;       /**< 提交时管道忙。 */
  uint32_t submit_err;        /**< 提交失败。 */
  uint32_t worker_done;       /**< worker 完成次数。 */
  uint32_t putarea_fail;      /**< PUTAREA ioctl 失败。 */
  uint32_t flush_ready;       /**< 调用 flush_ready。 */
  uint32_t wait_calls;        /**< wait_idle 次数。 */
  uint32_t wait_ms_max;       /**< 单次等待最长毫秒。 */
  uint32_t wait_timeout;      /**< 等待超时次数。 */
  uint32_t flush_ready_force; /**< 强制 flush_ready。 */
  uint32_t submit_retry;      /**< 提交重试。 */
  uint32_t wait_stall;        /**< 等待卡住。 */
  uint32_t putarea_ms_max;    /**< 单次 PUTAREA 最长毫秒。 */
  uint32_t putarea_slow;      /**< PUTAREA 过慢次数。 */
  uint32_t recover;           /**< 管道恢复次数。 */
} myvendor_lcd_disp_stats_t;

/**
 * @brief 复制统计快照。
 */
void myvendor_lcd_disp_stats_snapshot(myvendor_lcd_disp_stats_t * stats);

/**
 * @brief 清窗口统计（保留累计项视实现而定）。
 */
void myvendor_lcd_disp_stats_reset_window(void);

/**
 * @brief UI 侧记一次 flush_ready。
 */
void myvendor_lcd_disp_stats_note_flush_ready(void);

/**
 * @brief UI 侧记一次 wait。
 */
void myvendor_lcd_disp_stats_note_wait(uint32_t waited_ms, bool timeout,
    bool forced);

/**
 * @brief UI 侧记一次提交重试。
 */
void myvendor_lcd_disp_stats_note_submit_retry(void);

/**
 * @brief UI 侧记一次提交错误。
 */
void myvendor_lcd_disp_stats_note_submit_err(void);

/**
 * @brief 在 flush_wait 里 wait_idle 之后调用：只做 lv_display_flush_ready。
 */
void myvendor_lcd_disp_poll_recover_unblock(void);

/**
 * @brief 在 bicycle_ui 循环（lv_timer_handler 之前）调用：处理挂起的全屏 refr。
 */
void myvendor_lcd_disp_poll_recover_ui(void);

/**
 * @brief recover 正在排空过期局部刷新（flush_cb 应跳过 DMA）。
 */
bool myvendor_lcd_disp_recover_hold_active(void);

/**
 * @brief 手动重启 ui_flush 管道（NSH `lcd_recover`）。
 *
 * bicycle_ui 必须跑 poll_recover_ui 才能让 LVGL 跟上。
 * @return 0 成功，负值为错误码。
 */
int myvendor_lcd_disp_trigger_recover(void);

/**
 * @brief UI 大块刷新（如换主题）时暂停 recover 静默窗口。
 * @param ms 抑制时长。
 */
void myvendor_lcd_disp_suppress_recover_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __MYVENDOR_LCD_DISP_H */
