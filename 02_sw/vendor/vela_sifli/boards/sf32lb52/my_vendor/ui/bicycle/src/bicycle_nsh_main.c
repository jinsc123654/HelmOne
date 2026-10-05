/**
 * @file bicycle_nsh_main.c
 * @brief 自行车 UI — nsh_main。
 */

#include <nuttx/config.h>

#include "bicycle_nsh.h"

/**
 * @brief main 接口。
 */
int main(int argc, char * argv[])
{
    if (argc >= 2) {
        return bicycle_nsh_dispatch(argc - 1, argv + 1);
    }

    return bicycle_nsh_dispatch(0, argv + argc);
}
