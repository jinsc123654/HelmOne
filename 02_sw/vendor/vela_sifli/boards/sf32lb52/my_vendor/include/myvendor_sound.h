/**
 * @file myvendor_sound.h
 * @brief 提示音 owner：UI 只投递 ID，声音线程查播放函数表执行。
 *
 * Helm One PA40 接无源陶瓷蜂鸣片，由 GPTIM2 CH1（脚上是 PA40_TIM）PWM 驱动。
 * 背光是 GPTIM1 CH4 / PA01，两路 GPTIM 频率独立。
 * 陶瓷片通常不分正负；有 “+” 标记则 + 接 PA40、另一脚接地。电磁/有源蜂鸣器
 * 才必须分正负，且有源件应给直流，不能喂 kHz 方波（否则只有“次那”噪声）。
 * 提示音表在声音线程里；以后换成 MP3 只需改播放函数表，调用点不用改。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_SOUND_H
#define MYVENDOR_SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 播放内容 ID，与声音线程里的函数表下标一致。 */
typedef enum {
  MYVENDOR_SOUND_KEY = 0, /**< KEY1 短按 / 列表移动。 */
  MYVENDOR_SOUND_OK,      /**< KEY2 短按 / 确认。 */
  MYVENDOR_SOUND_LONG,    /**< KEY1/KEY2 长按。 */
  MYVENDOR_SOUND_BACK,    /**< 返回、关掉浮层。 */
  MYVENDOR_SOUND_START,   /**< 开始或继续骑行（myvendor_sound_ride）。 */
  MYVENDOR_SOUND_PAUSE,   /**< 暂停骑行。 */
  MYVENDOR_SOUND_SAVE,    /**< 保存记录。 */
  MYVENDOR_SOUND_DISCARD, /**< 不保存结束。 */
  MYVENDOR_SOUND_ARRIVE,  /**< 到达途经点 / 终点。 */
  MYVENDOR_SOUND_WARN,    /**< 失败、空列表、断开。 */
  MYVENDOR_SOUND_BOOT,    /**< 开机。 */
  MYVENDOR_SOUND_PROMPT,  /**< 需要再确认（保存对话框、USB）。 */
  MYVENDOR_SOUND_COUNT
} myvendor_sound_id_t;

/**
 * @brief 启动声音线程并配置 PA40 PWM（可重复调用）。
 *
 * 栈在 .bss 静态分配；空闲时 cond_wait 挂起，不反复 create/exit。
 * @return 0 成功，负 errno。
 */
int myvendor_sound_start(void);

/**
 * @brief 把一条提示投进声音线程（非阻塞，队列满则丢最新）。
 */
void myvendor_sound_play(myvendor_sound_id_t id);

/**
 * @brief 按名称播放（NSH `test sound`）。
 * @return 0 成功，-ENOENT 未知名。
 */
int myvendor_sound_play_name(const char *name);

/** @brief ID 的短名，未知则 NULL。 */
const char *myvendor_sound_id_name(myvendor_sound_id_t id);

/**
 * @brief 阻塞输出固定频率 PWM（NSH 诊断：`test sound 4k`）。
 * @return 0 成功，负 errno。
 */
int myvendor_sound_diag_pwm(uint16_t hz, uint16_t ms);

/**
 * @brief 指定占空比的 PWM（`test sound duty 40`），10～90。
 */
int myvendor_sound_diag_pwm_duty(uint16_t hz, uint16_t ms, uint8_t duty_pct);

/**
 * @brief 阻塞把 PA40 拉高一段（NSH 诊断：`test sound dc`）。
 *
 * 有源蜂鸣器会持续响；无源陶瓷片几乎无声。结束后恢复 PWM 复用。
 * @return 0 成功，负 errno。
 */
int myvendor_sound_diag_dc(uint16_t ms);

/**
 * @brief 4 kHz～10 kHz 扫频，找陶瓷片谐振点（`test sound sweep`）。
 * @return 0 成功，负 errno。
 */
int myvendor_sound_diag_sweep(void);

/** @brief 按键 / 操作调用点：只负责投递，不在 UI 线程里出声。 */
void myvendor_sound_key(void);
void myvendor_sound_ok(void);
void myvendor_sound_long(void);
void myvendor_sound_back(void);
void myvendor_sound_ride(void);
void myvendor_sound_pause(void);
void myvendor_sound_save(void);
void myvendor_sound_discard(void);
void myvendor_sound_arrive(void);
void myvendor_sound_warn(void);
void myvendor_sound_boot(void);
void myvendor_sound_prompt(void);

/**
 * @brief 队列空且当前没有在播。
 *
 * 开机黑屏等开机提示音播完再进白屏用。声音策略关闭或线程未启动则为 true。
 */
bool myvendor_sound_idle(void);

/**
 * @brief 崩溃 note 用：是否在播、当前 ID。IRQ 安全。
 */
int myvendor_sound_crash_format(char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_SOUND_H */
