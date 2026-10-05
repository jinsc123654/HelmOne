/**
 * @file bicycle_nsh.h
 * @brief 自行车 UI — nsh。
 */

#ifndef BICYCLE_NSH_H
#define BICYCLE_NSH_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief NSH 入口；返回进程退出码。 */
int bicycle_nsh_dispatch(int argc, char * argv[]);

#ifdef __cplusplus
}
#endif

#endif /* BICYCLE_NSH_H */
