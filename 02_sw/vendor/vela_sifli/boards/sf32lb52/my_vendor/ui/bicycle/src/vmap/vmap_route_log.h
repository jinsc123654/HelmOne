/**
 * @file vmap_route_log.h
 * @brief vmap route_log 模块。
 *
 * 经 vsyslog/vprintf 转发，避免本平台 uint32_t=unsigned long 与 %u 的 -Wformat。
 */

#ifndef VMAP_ROUTE_LOG_H
#define VMAP_ROUTE_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

static inline void vmap_nav_vsyslog(int prio, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsyslog(prio, fmt, ap);
    va_end(ap);
}

static inline void vmap_nav_vprintf_ln(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define VMAP_NAV_LOG(fmt, ...)  vmap_nav_vsyslog(LOG_NOTICE,  "[nav] " fmt, ##__VA_ARGS__)
#define VMAP_NAV_WARN(fmt, ...) vmap_nav_vsyslog(LOG_WARNING, "[nav] " fmt, ##__VA_ARGS__)
#define VMAP_NAV_ERR(fmt, ...)  vmap_nav_vsyslog(LOG_ERR,     "[nav] " fmt, ##__VA_ARGS__)

/** @brief syslog + printf — 路径规划算法步骤调试用。 */
#define VMAP_NAV_OUT(fmt, ...) do { \
        vmap_nav_vsyslog(LOG_NOTICE, "[nav] " fmt, ##__VA_ARGS__); \
        vmap_nav_vprintf_ln("[nav] " fmt, ##__VA_ARGS__); \
    } while (0)

#endif /* VMAP_ROUTE_LOG_H */
