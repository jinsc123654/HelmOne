/**
 * @file sf32lb52_buttons.c
 * @brief 自行车 UI 按键 lower-half：KEY1=PA30、KEY2=PA33、PWR=PA34。
 *
 * KEY1/KEY2 上拉按下接地；PWR 下拉按下为高（开机键，硬件 10s 复位）。
 * 注册 /dev/buttons。只由板级线程 10 ms 读 GPIO，不上边沿中断、
 * 不挂 NuttX 软件 wdog。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#include <nuttx/input/buttons.h>
#include <nuttx/irq.h>
#include <nuttx/kthread.h>
#include <nuttx/sched.h>
#include <arch/irq.h>

#include "sifli_gpio.h"
#include "sf32lb52_buttons.h"

#ifdef CONFIG_INPUT_BUTTONS

#define SF32LB52_BTN_CLICK_PIN   GET_PIN_2(hwp_gpio1, 33) /**< KEY2 PA33，bit0。 */
#define SF32LB52_BTN_SCROLL_PIN  GET_PIN_2(hwp_gpio1, 30) /**< KEY1 PA30，bit1。 */
#define SF32LB52_BTN_PWR_PIN     GET_PIN_2(hwp_gpio1, 34) /**< PWR PA34，bit2。 */
#define SF32LB52_BTN_CLICK_BIT   0  /**< KEY2 位掩码下标。 */
#define SF32LB52_BTN_SCROLL_BIT  1  /**< KEY1 位掩码下标。 */
#define SF32LB52_BTN_PWR_BIT     2  /**< PWR 位掩码下标。 */
#define SF32LB52_BTN_ACTIVE_HIGH 0  /**< KEY1/KEY2：0=上拉按下为低。 */
#define SF32LB52_BUTTON_POLL_MS  10 /**< 轮询周期（毫秒）。 */
#define SF32LB52_BUTTON_POLL_PRIO 50
#define SF32LB52_BUTTON_POLL_STACK 1536

static uint8_t g_btn_poll_stack[SF32LB52_BUTTON_POLL_STACK]
    __attribute__((aligned(16)));

/** @brief 按键 lower-half 私有状态。 */
struct sf32lb52_btnlower_s {
    struct btn_lowerhalf_s lower;
    btn_handler_t handler;       /**< NuttX 上报回调。 */
    FAR void *arg;               /**< 回调参数。 */
    volatile bool poll_enabled;  /**< 轮询线程是否采样。 */
    int poll_tid;                /**< 轮询线程 pid，0 表示未创建。 */
    btn_buttonset_t last_sample; /**< 上次采样位图。 */
};

static btn_buttonset_t sf32lb52_btn_supported(
    FAR const struct btn_lowerhalf_s *lower);
static btn_buttonset_t sf32lb52_btn_buttons(
    FAR const struct btn_lowerhalf_s *lower);
static void sf32lb52_btn_enable(FAR const struct btn_lowerhalf_s *lower,
                                btn_buttonset_t press,
                                btn_buttonset_t release,
                                btn_handler_t handler,
                                FAR void *arg);

/** 全局按键 lower-half 单例。 */
static struct sf32lb52_btnlower_s g_btnlower = {
    {
        sf32lb52_btn_supported,
        sf32lb52_btn_buttons,
        sf32lb52_btn_enable,
        NULL,
    },
    NULL,
    NULL,
    false,
    0,
    0,
};

/** @brief 读取当前按键位图。 */
static btn_buttonset_t sf32lb52_button_readset(void)
{
    btn_buttonset_t set = 0;
    bool click = sifli_gpio_read(SF32LB52_BTN_CLICK_PIN);
    bool scroll = sifli_gpio_read(SF32LB52_BTN_SCROLL_PIN);
    bool pwr = sifli_gpio_read(SF32LB52_BTN_PWR_PIN);

#if SF32LB52_BTN_ACTIVE_HIGH
    if (click) {
        set |= (1u << SF32LB52_BTN_CLICK_BIT);
    }
    if (scroll) {
        set |= (1u << SF32LB52_BTN_SCROLL_BIT);
    }
#else
    if (!click) {
        set |= (1u << SF32LB52_BTN_CLICK_BIT);
    }
    if (!scroll) {
        set |= (1u << SF32LB52_BTN_SCROLL_BIT);
    }
#endif

    /* PWR_KEY_READ：按下为高。 */
    if (pwr) {
        set |= (1u << SF32LB52_BTN_PWR_BIT);
    }

    return set;
}

/** @brief 状态变化则通知 upper-half。 */
static void sf32lb52_button_notify(FAR struct sf32lb52_btnlower_s *priv)
{
    btn_buttonset_t sample;
    btn_handler_t handler;
    FAR void *arg;
    irqstate_t flags;

    sample = sf32lb52_button_readset();
    if (sample == priv->last_sample) {
        return;
    }

    /* handler 与 arg 必须在同一临界区里成对取出：enable 侧会在临界区内把两个
     * 一起置 NULL。以前是分两次裸读，中间被 enable 插进来就会用 NULL 调用
     * handler —— btn_interrupt 直接解引用 arg。回调在临界区外调用，避免带着
     * 关中断去跑上层代码。 */
    flags = up_irq_save();
    priv->last_sample = sample;
    handler = priv->handler;
    arg     = priv->arg;
    up_irq_restore(flags);

    if (handler != NULL && arg != NULL) {
        handler(&priv->lower, arg);
    }
}

/** @brief 板级轮询线程：只在线程上下文读键。 */
static int sf32lb52_button_poll_task(int argc, FAR char *argv[])
{
    FAR struct sf32lb52_btnlower_s *priv = &g_btnlower;

    (void)argc;
    (void)argv;

    for (;;) {
        if (priv->poll_enabled) {
            sf32lb52_button_notify(priv);
        }

        usleep(SF32LB52_BUTTON_POLL_MS * 1000);
    }

    return 0;
}

/** @brief 返回支持的按键位掩码。 */
static btn_buttonset_t sf32lb52_btn_supported(
    FAR const struct btn_lowerhalf_s *lower)
{
    (void)lower;
    return (1u << SF32LB52_BTN_CLICK_BIT) |
           (1u << SF32LB52_BTN_SCROLL_BIT) |
           (1u << SF32LB52_BTN_PWR_BIT);
}

/** @brief 返回当前按下的按键位。 */
static btn_buttonset_t sf32lb52_btn_buttons(
    FAR const struct btn_lowerhalf_s *lower)
{
    (void)lower;
    return sf32lb52_button_readset();
}

/** @brief 使能/禁用轮询并挂接 handler。 */
static void sf32lb52_btn_enable(FAR const struct btn_lowerhalf_s *lower,
                                btn_buttonset_t press,
                                btn_buttonset_t release,
                                btn_handler_t handler,
                                FAR void *arg)
{
    FAR struct sf32lb52_btnlower_s *priv = (FAR struct sf32lb52_btnlower_s *)lower;
    btn_buttonset_t either = press | release;
    irqstate_t flags;
    int ret;

    /* handler/arg 成对更新，与 sf32lb52_button_notify 的成对读取用同一临界区
     * 互斥；否则轮询线程可能取到"旧 handler + NULL arg"。 */
    if (either == 0 || handler == NULL) {
        flags = up_irq_save();
        priv->poll_enabled = false;
        priv->handler = NULL;
        priv->arg = NULL;
        up_irq_restore(flags);
        return;
    }

    {
        btn_buttonset_t snap = sf32lb52_button_readset();

        flags = up_irq_save();
        priv->handler = handler;
        priv->arg = arg;
        priv->last_sample = snap;
        priv->poll_enabled = true;
        up_irq_restore(flags);
    }

    if (priv->poll_tid <= 0) {
        ret = kthread_create_with_stack("btn_poll",
                                        SF32LB52_BUTTON_POLL_PRIO,
                                        g_btn_poll_stack,
                                        sizeof(g_btn_poll_stack),
                                        sf32lb52_button_poll_task, NULL);
        if (ret > 0) {
            priv->poll_tid = ret;
        }
    }
}

/**
 * @brief 初始化 PA30/PA33/PA34 并注册 /dev/buttons。
 * @param devname 字符设备路径。
 * @return 0 成功，负值为 btn_register 错误码。
 */
int sf32lb52_button_initialize(FAR const char *devname)
{
    int ret;

    sifli_gpio_config(SF32LB52_BTN_CLICK_PIN, GPIO_INPUT);
    sifli_gpio_config(SF32LB52_BTN_SCROLL_PIN, GPIO_INPUT);
    sifli_gpio_config(SF32LB52_BTN_PWR_PIN, GPIO_INPUT);

    ret = btn_register(devname, &g_btnlower.lower);
    if (ret < 0) {
        return ret;
    }

    return OK;
}

/**
 * @brief 调试直读按键原始电平（不经 debounce）。
 * @param[out] click  KEY2 PA33 逻辑按下（1=按下），可 NULL。
 * @param[out] scroll KEY1 PA30 逻辑按下（1=按下），可 NULL。
 * @param[out] pwr    PWR PA34 逻辑按下（1=按下），可 NULL。
 */
void sf32lb52_button_debug_read(bool *click, bool *scroll, bool *pwr)
{
    bool c = sifli_gpio_read(SF32LB52_BTN_CLICK_PIN);
    bool s = sifli_gpio_read(SF32LB52_BTN_SCROLL_PIN);
    bool p = sifli_gpio_read(SF32LB52_BTN_PWR_PIN);

#if SF32LB52_BTN_ACTIVE_HIGH
    if (click != NULL) {
        *click = c;
    }
    if (scroll != NULL) {
        *scroll = s;
    }
#else
    if (click != NULL) {
        *click = !c;
    }
    if (scroll != NULL) {
        *scroll = !s;
    }
#endif

    if (pwr != NULL) {
        *pwr = p;
    }
}

#endif /* CONFIG_INPUT_BUTTONS */
