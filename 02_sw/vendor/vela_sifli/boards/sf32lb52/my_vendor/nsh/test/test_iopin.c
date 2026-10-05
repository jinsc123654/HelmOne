/**
 * @file test_iopin.c
 * @brief test iopin：GPIO 焊盘/走线探针，按表逐脚翻转。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 #include <nuttx/config.h>

 #include <errno.h>
 #include <signal.h>
 #include <stdbool.h>
 #include <stddef.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 
 #include <nuttx/sched.h>
 
#ifdef UNUSED
#  undef UNUSED
#endif
#include "bf0_hal.h"
 #include "drv_io.h"
 
 #define IOPIN_PERIOD_US   2500000u
 #define IOPIN_STACK       4096
 #define IOPIN_PRIORITY    100
 #define IOPIN_TASK_NAME   "test_iopin"
 
 struct iopin_s
 {
   FAR const char *name;
   int pad;
   pin_function gpio_func;
   int gpio_pin;
   int is_porta;
 };
 
 /* Add pads here. gpio_pin is the HPSYS GPIO number (PAxx -> xx).
  * LCD QSPI (schematic SF_PA00..PA08 + PA10 power).
  * I2C1 传感器总线：PA39 SCL / PA38 SDA（翻转为 GPIO，测完需复位才回 I2C）。 */
 
 static const struct iopin_s g_iopins[] =
 {
   { "PA00(LCD_RTS)", PAD_PA00, GPIO_A0, 0, 1 },
   { "PA01(LCD_BL)",  PAD_PA01, GPIO_A1, 1, 1 },
   { "PA02(LCD_TE)",  PAD_PA02, GPIO_A2, 2, 1 },
   { "PA03(LCD_CS)",  PAD_PA03, GPIO_A3, 3, 1 },
   { "PA04(LCD_SCK)", PAD_PA04, GPIO_A4, 4, 1 },
   { "PA05(LCD_D0)",  PAD_PA05, GPIO_A5, 5, 1 },
   { "PA06(LCD_D1)",  PAD_PA06, GPIO_A6, 6, 1 },
   { "PA07(LCD_D2)",  PAD_PA07, GPIO_A7, 7, 1 },
   { "PA08(LCD_D3)",  PAD_PA08, GPIO_A8, 8, 1 },
   { "PA10(LCD_PWR)", PAD_PA10, GPIO_A10, 10, 1 },
   { "PA38(I2C1_SDA)", PAD_PA38, GPIO_A38, 38, 1 },
   { "PA39(I2C1_SCL)", PAD_PA39, GPIO_A39, 39, 1 },
 };
 
 #define IOPIN_COUNT  (sizeof(g_iopins) / sizeof(g_iopins[0]))
 
 static int g_pid = -1;
 static volatile size_t g_cur;
 static volatile bool g_auto;
 static volatile unsigned g_gen;
 static bool g_muxed;
 
 static bool iopin_alive(void)
 {
   return g_pid > 0 && kill((pid_t)g_pid, 0) == 0;
 }
 
 static void iopin_mux(void)
 {
   size_t i;
 
   if (g_muxed)
     {
       return;
     }
 
   for (i = 0; i < IOPIN_COUNT; i++)
     {
       HAL_PIN_Set(g_iopins[i].pad, g_iopins[i].gpio_func, PIN_NOPULL, 1);
     }
 
   g_muxed = true;
 }
 
 static void iopin_apply(size_t pin, bool high)
 {
   size_t i;
 
   if (pin >= IOPIN_COUNT)
     {
       pin = 0;
     }
 
   for (i = 0; i < IOPIN_COUNT; i++)
     {
       BSP_GPIO_Set(g_iopins[i].gpio_pin, (i == pin && high) ? 1 : 0,
                    g_iopins[i].is_porta);
     }
 
   printf("test iopin: %s [%zu] %s=%s\n",
          g_auto ? "all" : "hold",
          pin,
          g_iopins[pin].name,
          high ? "HIGH" : "LOW");
   fflush(stdout);
 }
 
 static void iopin_all_low(void)
 {
   size_t i;
 
   for (i = 0; i < IOPIN_COUNT; i++)
     {
       BSP_GPIO_Set(g_iopins[i].gpio_pin, 0, g_iopins[i].is_porta);
     }
 }
 
 static int iopin_worker(int argc, FAR char *argv[])
 {
   UNUSED(argc);
   UNUSED(argv);
 
   while (1)
     {
       unsigned gen = g_gen;
       size_t pin = g_cur;
 
       iopin_apply(pin, true);
       usleep(IOPIN_PERIOD_US);
       if (g_gen != gen)
         {
           continue;
         }
 
       iopin_apply(pin, false);
       usleep(IOPIN_PERIOD_US);
       if (g_gen != gen)
         {
           continue;
         }
 
       if (g_auto)
         {
           g_cur = (g_cur + 1) % IOPIN_COUNT;
         }
     }
 
   return 0;
 }
 
 static int iopin_start(void)
 {
   int pid;
 
   if (iopin_alive())
     {
       return EXIT_SUCCESS;
     }
 
   g_pid = -1;
   pid = task_create(IOPIN_TASK_NAME, IOPIN_PRIORITY, IOPIN_STACK,
                     iopin_worker, NULL);
   if (pid < 0)
     {
       printf("test iopin: task_create failed errno=%d\n", errno);
       return EXIT_FAILURE;
     }
 
   g_pid = pid;
   printf("test iopin: started pid=%d, %zu pin(s), 2.5s HIGH/LOW\n",
          pid, (size_t)IOPIN_COUNT);
   fflush(stdout);
   return EXIT_SUCCESS;
 }
 
 static int iopin_stop(void)
 {
   if (!iopin_alive())
     {
       printf("test iopin: not running\n");
       g_pid = -1;
       return EXIT_SUCCESS;
     }
 
   if (nxtask_delete((pid_t)g_pid) != OK)
     {
       printf("test iopin: nxtask_delete(%d) failed errno=%d\n",
              g_pid, errno);
       return EXIT_FAILURE;
     }
 
   printf("test iopin: stopped pid=%d\n", g_pid);
   g_pid = -1;
   iopin_all_low();
   return EXIT_SUCCESS;
 }
 
 static int iopin_step(int dir)
 {
   iopin_mux();
 
   if (!iopin_alive())
     {
       g_cur = (dir > 0) ? 0 : (IOPIN_COUNT - 1);
       g_auto = false;
       g_gen++;
       return iopin_start();
     }
 
   g_auto = false;
   if (dir > 0)
     {
       g_cur = (g_cur + 1) % IOPIN_COUNT;
     }
   else
     {
       g_cur = (g_cur == 0) ? (IOPIN_COUNT - 1) : (g_cur - 1);
     }
 
   g_gen++;
   iopin_apply(g_cur, true);
   return EXIT_SUCCESS;
 }
 
 static int iopin_goto(size_t idx)
 {
   iopin_mux();
   g_cur = idx;
   g_auto = false;
   g_gen++;
 
   if (!iopin_alive())
     {
       return iopin_start();
     }
 
   iopin_apply(g_cur, true);
   return EXIT_SUCCESS;
 }
 
 static int iopin_all(void)
 {
   iopin_mux();
   g_auto = true;
 
   if (!iopin_alive())
     {
       g_cur = 0;
       g_gen++;
       return iopin_start();
     }
 
   g_gen++;
   printf("test iopin: auto from [%zu] %s\n",
          g_cur, g_iopins[g_cur].name);
   fflush(stdout);
   return EXIT_SUCCESS;
 }
 
 static void iopin_print_list(void)
 {
   size_t i;
   size_t cur = g_cur;
   bool running = iopin_alive();
 
   printf("IO list (%zu):\n", (size_t)IOPIN_COUNT);
   for (i = 0; i < IOPIN_COUNT; i++)
     {
       printf("  %c [%zu] %s  gpio=%d\n",
              (running && i == cur) ? '*' : ' ',
              i,
              g_iopins[i].name,
              g_iopins[i].gpio_pin);
     }
 }
 
 static void iopin_usage(void)
 {
   printf("Usage: test iopin [N|next|prev|all|stop]\n");
   printf("  (none) print the IO table only, no toggle\n");
   printf("  N      hold-toggle list index N (1..%zu)\n", (size_t)IOPIN_COUNT);
   printf("  next   hold-toggle current pin; each call selects the next pad\n");
   printf("  prev   same, but select the previous pad\n");
   printf("  all    auto-advance through the table (one HIGH at a time)\n");
   printf("  stop   stop the worker and drive all pads LOW\n");
 }
 
 int test_iopin_main(int argc, FAR char *argv[])
 {
   if (argc < 2)
     {
       iopin_print_list();
       return EXIT_SUCCESS;
     }
 
   if (strcmp(argv[1], "next") == 0)
     {
       return iopin_step(1);
     }
 
   if (strcmp(argv[1], "prev") == 0)
     {
       return iopin_step(-1);
     }
 
   if (strcmp(argv[1], "all") == 0)
     {
       return iopin_all();
     }
 
   if (strcmp(argv[1], "stop") == 0)
     {
       return iopin_stop();
     }
 
   {
     char *end = NULL;
     long v = strtol(argv[1], &end, 10);
 
     if (end != argv[1] && end != NULL && *end == '\0')
       {
         if (v < 1 || v > (long)IOPIN_COUNT)
           {
             printf("test iopin: index %ld out of range (1..%zu)\n",
                    v, (size_t)IOPIN_COUNT);
             iopin_print_list();
             return EXIT_FAILURE;
           }
 
         return iopin_goto((size_t)(v - 1));
       }
   }
 
   printf("test iopin: unknown arg '%s'\n", argv[1]);
   iopin_usage();
   return EXIT_FAILURE;
 }
 