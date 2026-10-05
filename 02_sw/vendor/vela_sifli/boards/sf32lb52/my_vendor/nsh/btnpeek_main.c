/**
 * @file btnpeek_main.c
 * @brief NSH 命令 btnpeek：轮询 KEY1=PA30 / KEY2=PA33 / PWR=PA34 与 /dev/buttons。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <nuttx/input/buttons.h>

#include "sf32lb52_buttons.h"

/**
 * @brief NSH 命令 btnpeek：轮询 KEY1=PA30 / KEY2=PA33 / PWR=PA34 与 /dev/buttons。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, char *argv[])
{
    int sec = 30;
    int fd;
    int i;

    if (argc >= 2) {
        sec = atoi(argv[1]);
        if (sec <= 0) {
            sec = 30;
        }
    }

    fd = open("/dev/buttons", O_RDONLY);
    if (fd < 0) {
        printf("btnpeek: open /dev/buttons failed\n");
        return 1;
    }

    printf("btnpeek: KEY2=PA33 KEY1=PA30 PWR=PA34 (1=pressed)\n");
    printf("  logical | /dev/buttons bits\n");

    for (i = 0; i < sec * 5; i++) {
        btn_buttonset_t set = 0;
        bool key2 = false;
        bool key1 = false;
        bool pwr = false;

        sf32lb52_button_debug_read(&key2, &key1, &pwr);

        if (read(fd, &set, sizeof(set)) == (ssize_t)sizeof(set)) {
            printf(
                "  KEY2=%d KEY1=%d PWR=%d  buttons=0x%02x%s%s%s\n",
                key2 ? 1 : 0,
                key1 ? 1 : 0,
                pwr ? 1 : 0,
                (unsigned)set,
                (set & 1) ? " [KEY2]" : "",
                (set & 2) ? " [KEY1]" : "",
                (set & 4) ? " [PWR]" : "");
        }

        usleep(200 * 1000);
    }

    close(fd);
    return 0;
}
