/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_uart2loop.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USART2 (/dev/ttyS0) loopback self-test for GNSS wiring debug.
 * Hardware: PA31 = TX, PA32 = RX (see bsp_pinmux.c).
 *
 * Usage:
 *   test uart2loop              loopback 10 times (default)
 *   test uart2loop <count>      loopback count times
 *   test uart2loop <count> <baud>  loopback at baud (e.g. 115200)
 *   test uart2loop listen       RX-only sniffer @ 9600 for 15 s
 *   test uart2loop listen <baud> [sec]
 *
 * Loopback: short PA31 (TX) to PA32 (RX) with a jumper, then run the test.
 * Without jumper the test should FAIL (proves you are not reading local echo).
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_BSP_USING_UART2)

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_BOARD_L96_GNSS
#  define UART2LOOP_DEVPATH   CONFIG_BOARD_L96_GNSS_DEVPATH
#  define UART2LOOP_BAUD_DEF  CONFIG_BOARD_L96_GNSS_BAUD
#else
#  define UART2LOOP_DEVPATH   "/dev/ttyS0"
#  define UART2LOOP_BAUD_DEF  9600
#endif

#define UART2LOOP_UART_NUM        2
#define UART2LOOP_DEFAULT_COUNT   10
#define UART2LOOP_POLL_MS         100
#define UART2LOOP_LISTEN_SEC      15
#define UART2LOOP_RX_TIMEOUT_MS   2000
#define UART2LOOP_ROUND_DELAY_US  100000

extern void sifli_uart_set_default_baud(unsigned int uart_num, uint32_t baud);

static void uart2loop_flush_stdout(void)
{
  fflush(stdout);
}

static int uart2loop_set_baud(int baud)
{
  sifli_uart_set_default_baud(UART2LOOP_UART_NUM, (uint32_t)baud);
  return OK;
}

static int uart2loop_open(void)
{
  return open(UART2LOOP_DEVPATH, O_RDWR | O_NONBLOCK);
}

static void uart2loop_drain(int fd)
{
  char dump[64];
  struct pollfd pfd;
  ssize_t n;

  for (; ; )
    {
      pfd.fd     = fd;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, 0) <= 0)
        {
          break;
        }

      n = read(fd, dump, sizeof(dump));
      if (n <= 0)
        {
          break;
        }
    }
}

static int uart2loop_read_bytes(int fd, FAR char *buf, size_t want, int timeout_ms)
{
  size_t got = 0;
  struct pollfd pfd;
  ssize_t n;

  while (got < want)
    {
      pfd.fd     = fd;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, timeout_ms) <= 0)
        {
          break;
        }

      n = read(fd, buf + got, want - got);
      if (n < 0)
        {
          if (errno == EAGAIN || errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          break;
        }

      got += (size_t)n;
    }

  return (int)got;
}

static void uart2loop_print_rx(FAR const char *rx, int nr)
{
  int i;

  for (i = 0; i < nr; i++)
    {
      if (rx[i] >= 0x20 && rx[i] < 0x7f)
        {
          putchar(rx[i]);
        }
      else
        {
          printf("<%02x>", (unsigned char)rx[i]);
        }
    }

  printf("\n");
}

static int uart2loop_round(int fd, int round, int count, bool verbose)
{
  static const char tx[] = "UART2_LOOP_42_OK\r\n";
  char rx[sizeof(tx)];
  ssize_t nw;
  int nr;
  bool match;

  uart2loop_drain(fd);

  nw = write(fd, tx, strlen(tx));
  if (nw != (ssize_t)strlen(tx))
    {
      printf("[%d/%d] FAIL TX write nw=%zd errno=%d\n", round, count, nw, errno);
      return -1;
    }

  usleep(50 * 1000);

  memset(rx, 0, sizeof(rx));
  nr = uart2loop_read_bytes(fd, rx, strlen(tx), UART2LOOP_RX_TIMEOUT_MS);
  if (nr <= 0)
    {
      printf("[%d/%d] FAIL RX timeout (%d ms), got %d bytes\n",
             round, count, UART2LOOP_RX_TIMEOUT_MS, nr);
      return -1;
    }

  match = (nr == (int)strlen(tx) && memcmp(rx, tx, strlen(tx)) == 0);
  if (match)
    {
      printf("[%d/%d] PASS loopback (%d bytes match)\n", round, count, nr);
      return 0;
    }

  printf("[%d/%d] FAIL mismatch TX (%zu): %s", round, count, strlen(tx), tx);
  printf("[%d/%d] FAIL mismatch RX (%d): ", round, count, nr);
  uart2loop_print_rx(rx, nr);
  if (verbose)
    {
      printf("test uart2loop: round %d data dump above\n", round);
    }

  return -1;
}

static int uart2loop_run_loopback(int fd, int baud, int count)
{
  int round;
  int pass = 0;

  printf("test uart2loop: USART2 %s @ %d baud x%d rounds\n",
         UART2LOOP_DEVPATH, baud, count);
  printf("test uart2loop: short PA31(TX) <-> PA32(RX) for external loopback\n");
  uart2loop_flush_stdout();

  for (round = 1; round <= count; round++)
    {
      if (uart2loop_round(fd, round, count, round == 1) == 0)
        {
          pass++;
        }

      if (round < count)
        {
          usleep(UART2LOOP_ROUND_DELAY_US);
        }

      uart2loop_flush_stdout();
    }

  printf("test uart2loop: summary %d/%d PASS\n", pass, count);
  return (pass == count) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int uart2loop_run_listen(int fd, int baud, int sec)
{
  char ch;
  struct pollfd pfd;
  time_t deadline;
  time_t last_hb;
  time_t now;
  unsigned long bytes = 0;
  ssize_t n;

  printf("test uart2loop: listen on %s @ %d baud for %d s (RX only)\n",
         UART2LOOP_DEVPATH, baud, sec);
  printf("test uart2loop: connect GNSS or loopback jumper, Ctrl+C to abort\n");
  uart2loop_flush_stdout();

  uart2loop_drain(fd);
  deadline = time(NULL) + sec;
  last_hb  = time(NULL);

  while ((now = time(NULL)) <= deadline)
    {
      pfd.fd     = fd;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, UART2LOOP_POLL_MS) <= 0)
        {
          if (now - last_hb >= 3)
            {
              printf("test uart2loop: listen... %lu bytes\n", bytes);
              uart2loop_flush_stdout();
              last_hb = now;
            }

          continue;
        }

      n = read(fd, &ch, 1);
      if (n <= 0)
        {
          continue;
        }

      bytes++;
      if (ch == '\n')
        {
          printf("\n");
        }
      else if (ch == '\r')
        {
          /* skip */
        }
      else if (ch >= 0x20 && ch < 0x7f)
        {
          putchar(ch);
        }
      else
        {
          printf("<%02x>", (unsigned char)ch);
        }

      uart2loop_flush_stdout();
    }

  printf("\ntest uart2loop: listen done, %lu bytes\n", bytes);
  return (bytes > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int test_uart2loop_main(int argc, FAR char *argv[])
{
  int fd;
  int baud;
  int count;
  bool listen = false;
  int listen_sec = UART2LOOP_LISTEN_SEC;
  int ret;

  baud  = UART2LOOP_BAUD_DEF;
  count = UART2LOOP_DEFAULT_COUNT;

  if (argc > 1 && strcmp(argv[1], "listen") == 0)
    {
      listen = true;
      if (argc > 2)
        {
          baud = atoi(argv[2]);
        }

      if (argc > 3)
        {
          listen_sec = atoi(argv[3]);
        }
    }
  else
    {
      if (argc > 1)
        {
          count = atoi(argv[1]);
          if (count < 1)
            {
              printf("test uart2loop: invalid count '%s'\n", argv[1]);
              return EXIT_FAILURE;
            }
        }

      if (argc > 2)
        {
          baud = atoi(argv[2]);
        }
    }

  if (!listen && baud < 1200)
    {
      printf("test uart2loop: invalid baud %d\n", baud);
      return EXIT_FAILURE;
    }

  uart2loop_set_baud(baud);

  fd = uart2loop_open();
  if (fd < 0)
    {
      printf("test uart2loop: open %s failed: %d\n",
             UART2LOOP_DEVPATH, errno);
      return EXIT_FAILURE;
    }

  if (listen)
    {
      if (listen_sec < 1)
        {
          listen_sec = UART2LOOP_LISTEN_SEC;
        }

      ret = uart2loop_run_listen(fd, baud, listen_sec);
    }
  else
    {
      ret = uart2loop_run_loopback(fd, baud, count);
    }

  close(fd);
  return ret;
}

#endif /* CONFIG_BSP_USING_UART2 */
