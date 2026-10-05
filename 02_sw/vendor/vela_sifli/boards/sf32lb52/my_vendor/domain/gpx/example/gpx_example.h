/**
 * @file gpx_example.h
 * @brief NSH: gpx example  — 一键 3 路录制 + decode 演示
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_EXAMPLE_H
#define MY_VENDOR_GPX_EXAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief example 入口。无参数时运行一键演示；也可传 record/decode/status。
 *
 * @param argc  0 = 一键演示；否则 argv[0] 为子命令
 * @param argv  参数列表
 * @return EXIT_SUCCESS 或 EXIT_FAILURE
 */
int gpx_example_main(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_EXAMPLE_H */