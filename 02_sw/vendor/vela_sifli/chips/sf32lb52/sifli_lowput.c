/****************************************************************************
 * vendor/sifli/chip/sf32lb52/sifli_lowput.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/**
 * @file sifli_lowput.c
 * @brief 板级最底层字符出口：`arm_lowputc()`（轮询写 USART 寄存器，无锁）。
 *
 * 这里同时承担两件"板级观测"的事，二者都不改变输出行为：
 *   - `g_isr_console_chars`：**在中断上下文里打印**的累计字符数（明确禁忌，
 *     但要能测量，见 myvendor_diag.c 的 ERROR 上报）；
 *   - `g_isrprint_*` 冒犯者快照：一次窗口里"是谁在打印"的证据（IPSR/CFSR/HFSR +
 *     正文开头），由 diag 线程取走，**中断侧永不调用任何日志接口**。
 *
 * 板级注记（2026-09-24 起）：控制台串口与控制台读线程都不在这里 —— 读路径在
 * `sf32lb_serial.c`/`sifli_uart.c`；本文件只管"把字符写出去"。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdbool.h>
#include "bf0_hal.h"
#include "sf32lb_serial.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/** 运行期哨兵：**在中断上下文里往控制台写字符**的累计次数（板级，2026-09-24）。
 *
 *  中断里打印是明确禁忌（拉长 ISR、可能自锁控制台路径），但"谁在中断里打印"
 *  以前只能靠读代码猜 —— 这里把它变成可测量的事实：计数放在**最底层字符出口**
 *  （`arm_lowputc()` 与 `sifli_uart.c` 的 `sifli_send()` 两处，一个字符只走一条路），
 *  零副作用；**打印一律由 diag 线程负责**（见 `myvendor_diag.c`），中断侧永不输出。 */
volatile uint32_t g_isr_console_chars;

/** 冒犯者快照：中断侧只写、diag 线程只读（都是 volatile，无锁）。
 *
 *  光有计数只能知道"有人干了"，这里把**是谁干的**也记下来：
 *  `ipsr`（哪个中断/异常）、`cfsr`/`hfsr`（是否走故障路径）、以及**输出正文的开头若干
 *  字节** —— 模块前缀（`diag:` / `gnss:` / `dump_assert_info:` …）就在正文里，等于点名。
 *  中断侧只做"还没抓就抓一次 + 逐字符抄进小缓冲"，**不调任何日志接口**：
 *  `arm_lowputc()` 本身就是 syslog 默认通道（Default → `up_putc()`）的出口，
 *  在这里 syslog 会递归回自己。输出一律由 diag 线程负责。
 *  取走后 `valid` 归零，自动武装下一次捕获（即每条 ERROR 对应一段新窗口）。 */
#define ISRPRINT_TEXT_MAX 48
volatile uint32_t g_isrprint_valid;
volatile uint32_t g_isrprint_ipsr;
volatile uint32_t g_isrprint_cfsr;
volatile uint32_t g_isrprint_hfsr;
volatile uint32_t g_isrprint_len;
volatile char     g_isrprint_text[ISRPRINT_TEXT_MAX];

/**
 * @brief diag 线程侧取走冒犯者快照（任一出参可为 NULL）。
 *
 * @details 取走后把 `g_isrprint_len/valid` 归零，**自动重新武装**下一次捕获 ——
 *          因此每条 ERROR 上报对应的都是"自上次取走以来"的新窗口，不会重复报同一段。
 *          正文做长度截断并补 '\0'；中断侧只写、这里只读，两者都是 volatile、无锁。
 *
 * @param[out] ipsr 触发写入时所在的异常/中断号（`__get_IPSR()`；0 = 线程模式）。
 * @param[out] cfsr 该时刻的 `SCB->CFSR`（是否走 fault 路径）。
 * @param[out] hfsr 该时刻的 `SCB->HFSR`。
 * @param[out] text 正文开头若干字节（模块前缀就在这里，可用来点名）。
 * @param[in]  n    `text` 缓冲大小（含结尾 '\0'）。
 */
void sifli_isrprint_take(uint32_t *ipsr, uint32_t *cfsr, uint32_t *hfsr,
                         char *text, size_t n)
{
    uint32_t len = g_isrprint_len;

    if (ipsr != NULL)
      {
        *ipsr = g_isrprint_ipsr;
      }
    if (cfsr != NULL)
      {
        *cfsr = g_isrprint_cfsr;
      }
    if (hfsr != NULL)
      {
        *hfsr = g_isrprint_hfsr;
      }

    if (text != NULL && n > 0)
      {
        if (len > (uint32_t)(n - 1))
          {
            len = (uint32_t)(n - 1);
          }

        for (uint32_t i = 0; i < len; i++)
          {
            text[i] = g_isrprint_text[i];
          }

        text[len] = '\0';
      }

    g_isrprint_len   = 0;
    g_isrprint_valid = 0;
}

void arm_lowputc(char ch)
{
    if (up_interrupt_context())
      {
        if (g_isrprint_valid == 0)
          {
            g_isrprint_valid = 1;
            g_isrprint_ipsr  = __get_IPSR();
            g_isrprint_cfsr  = SCB->CFSR;
            g_isrprint_hfsr  = SCB->HFSR;
            g_isrprint_len   = 0;
          }

        if (g_isrprint_len < (ISRPRINT_TEXT_MAX - 1))
          {
            g_isrprint_text[g_isrprint_len++] = ch;
          }

        g_isr_console_chars++;
      }

    while ((hwp_usart1->ISR & UART_FLAG_TXE) == 0);
    hwp_usart1->TDR = (uint32_t)ch;
}

/****************************************************************************
 * Name: arm_earlyserialinit
 *
 * Description:
 *   Performs the low level USART initialization early in debug so that the
 *   serial console will be available during bootup.  This must be called
 *   before arm_serialinit.
 *
 ****************************************************************************/

/* UART handler declaration */
static UART_HandleTypeDef g_early_uart_handle;

void arm_earlyserialinit(void)
{
    /* The secondary bootloader already fully configured USART1 (pins, clock,
     * 8N1, baud) and it is actively working - every breadcrumb up to here was
     * printed through it at CONFIG_UART_BAUD (1000000). Re-initialising it here
     * is both redundant and harmful in this early context:
     *   - HAL_UART_Init() -> UART_CheckIdleState() busy-waits for TEACK/REACK
     *     with a HAL_GetTick()-based timeout, but interrupts are disabled
     *     (cpsid i) so the tick never advances -> if the flag is slow the wait
     *     never times out and we hang forever.
     * So keep the bootloader's working configuration for the early console.
     * NuttX's full serial driver will (re)configure USART1 properly later,
     * once the interrupt/tick subsystem is up. */
    (void)g_early_uart_handle;
}
