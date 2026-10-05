/**
 * @file test_demos.h
 * @brief test 自测分发器各 demo 的入口声明头文件。
 *
 * 每个 demo 在独立 test_<name>.c 中实现 test_<name>_main()，
 * 由 test_main.c 按子命令路由（如 test lfs）。argv[0] 设为子命令名。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __VENDOR_MYVENDOR_TEST_TEST_DEMOS_H
#define __VENDOR_MYVENDOR_TEST_TEST_DEMOS_H

#include <nuttx/config.h>

/** @brief test lfs 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_lfs_main(int argc, FAR char *argv[]);
/** @brief test fat 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_fat_main(int argc, FAR char *argv[]);
/** @brief test sd / posix 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_sd_main(int argc, FAR char *argv[]);
/** @brief test sdio 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_sdio_main(int argc, FAR char *argv[]);
/** @brief test layers 分层 SD 联测入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_layers_main(int argc, FAR char *argv[]);
/** @brief test diag 异常日志落盘验证入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_diag_main(int argc, FAR char *argv[]);
/** @brief test led 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_led_main(int argc, FAR char *argv[]);
/** @brief test iopin 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_iopin_main(int argc, FAR char *argv[]);
/** @brief test ioread 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_ioread_main(int argc, FAR char *argv[]);
/** @brief test iowrite 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_iowrite_main(int argc, FAR char *argv[]);
/** @brief test sound 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_sound_main(int argc, FAR char *argv[]);
/** @brief test eta 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_eta_main(int argc, FAR char *argv[]);
/** @brief test screen 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_screen_main(int argc, FAR char *argv[]);
#ifdef CONFIG_BOARD_BMP388
/** @brief test bmp388 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_bmp388_main(int argc, FAR char *argv[]);
#endif
#ifdef CONFIG_BOARD_MMC5983MA
/** @brief test mmc5983ma 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_mmc5983ma_main(int argc, FAR char *argv[]);
#endif
#ifdef CONFIG_BOARD_BMI270
/** @brief test bmi270 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_bmi270_main(int argc, FAR char *argv[]);
#endif
#ifdef CONFIG_BOARD_L96_GNSS
/** @brief test gnss 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_gnss_main(int argc, FAR char *argv[]);
#endif
#ifdef CONFIG_BSP_USING_UART2
/** @brief test uart2loop 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_uart2loop_main(int argc, FAR char *argv[]);
#endif
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
/** @brief test sensor BLE 扫描/连接演示。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_sensor_main(int argc, FAR char *argv[]);
/** @brief test notif 0xFF17 通知打印开关。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_notif_main(int argc, FAR char *argv[]);
/** @brief test ctrl 0xFF13/0xFF19 控制打印开关。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_ctrl_main(int argc, FAR char *argv[]);
#endif
/** @brief test cpu 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_cpu_main(int argc, FAR char *argv[]);
/** @brief test task 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_task_main(int argc, FAR char *argv[]);
/** @brief test idle 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_idle_main(int argc, FAR char *argv[]);
/** @brief test time 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_time_main(int argc, FAR char *argv[]);
/** @brief test tick 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_tick_main(int argc, FAR char *argv[]);
/** @brief test wdt 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_wdt_main(int argc, FAR char *argv[]);
/** @brief test rtc 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_rtc_main(int argc, FAR char *argv[]);
/** @brief test fault 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_fault_main(int argc, FAR char *argv[]);
#ifdef CONFIG_KVDB
/** @brief test kv 子命令入口。 @param argc 参数个数。 @param argv 参数向量。 @return 退出码。 */
int test_kv_main(int argc, FAR char *argv[]);
#endif

#endif /* __VENDOR_MYVENDOR_TEST_TEST_DEMOS_H */
