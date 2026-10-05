/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/test/test_gnss.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stream NMEA from u-blox MAX-M10S-00B-01 on USART2 (/dev/ttyS0). Prints every
 * sentence immediately (including GSV/GGA while searching). Use "raw"
 * for a byte dump. Factory NMEA is already enabled; do not send PMTK.
 * Low power is cutting module VCC (PA43), not closing the host UART.
 *
 * Assistance (UBX-MGA) lives in /mnt/lfs/eph (MTP volume root folder "eph"):
 *   test gnss eph              inject newest mga_<utc>.ubx (else dump.ubx / mga.ubx)
 *   test gnss eph <file>       inject that UBX file
 *   test gnss eph dump         poll UBX-MGA-DBD -> /mnt/lfs/eph/mga_<utc>.ubx
 *   test gnss eph dump <file>  dump the navigation database to file
 *
 * Usage:
 *   test gnss              print 10 NMEA lines (default)
 *   test gnss <count>      print up to count lines (0 = until timeout)
 *   test gnss raw          dump RX bytes for 30 s
 *   test gnss raw <sec>    dump RX bytes for sec seconds
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_BOARD_L96_GNSS)

#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <minmea/minmea.h>

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
#  include "myvendor_mtp.h"
#endif

/* Stack only while test gnss runs; not allocated when demo is not invoked. */
#define GNSS_MOD_PN              "MAX-M10S-00B-01"
#define GNSS_LINE_MAX            512
#define GNSS_DEFAULT_COUNT       10
#define GNSS_READ_TIMEOUT_SEC    120
#define GNSS_POLL_MS             200
#define GNSS_HEARTBEAT_SEC       3
#define GNSS_RAW_DEFAULT_SEC     30

#define GNSS_EPH_DIR             "/mnt/lfs/eph"
#define GNSS_EPH_FILE            GNSS_EPH_DIR "/mga.ubx"
#define GNSS_EPH_DUMP            GNSS_EPH_DIR "/dump.ubx"
#define GNSS_EPH_PATH_MAX        96
#define GNSS_TIME_MIN_UNIX       1704067200ul
#define UBX_MAX_PAYLOAD          192
#define UBX_ACK_MS               1500
#define UBX_DUMP_IDLE_MS         2000
#define UBX_DUMP_MAX_BYTES       (32 * 1024)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void gnss_flush_stdout(void)
{
  fflush(stdout);
}

static int gnss_uart_open(void)
{
  int fd;

  fd = open(CONFIG_BOARD_L96_GNSS_DEVPATH, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      printf("test gnss: open %s failed: %d\n",
             CONFIG_BOARD_L96_GNSS_DEVPATH, errno);
      return -1;
    }

  return fd;
}

static void gnss_drain_rx(int fd)
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

static int gnss_read_byte(int fd, FAR char *ch, int timeout_ms)
{
  struct pollfd pfd;
  ssize_t n;
  int pr;

  pfd.fd     = fd;
  pfd.events = POLLIN;

  pr = poll(&pfd, 1, timeout_ms);
  if (pr == 0)
    {
      return 0;
    }

  if (pr < 0)
    {
      return (errno == EINTR) ? 0 : -errno;
    }

  n = read(fd, ch, 1);
  if (n < 0)
    {
      if (errno == EAGAIN || errno == EINTR)
        {
          return 0;
        }

      return -errno;
    }

  if (n == 0)
    {
      return -EIO;
    }

  return 1;
}

static int gnss_read_line(int fd, FAR char *line, size_t maxlen,
                          time_t deadline, FAR time_t *last_hb)
{
  size_t cnt = 0;
  bool discard = false;
  char ch;
  int ret;
  time_t now;

  for (; ; )
    {
      now = time(NULL);
      if (now > deadline)
        {
          return -ETIMEDOUT;
        }

      ret = gnss_read_byte(fd, &ch, GNSS_POLL_MS);
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          if (last_hb != NULL && now - *last_hb >= GNSS_HEARTBEAT_SEC)
            {
              printf("test gnss: waiting for data... (%lds, baud=%d)\n",
                     (long)(deadline - now), CONFIG_BOARD_L96_GNSS_BAUD);
              printf("test gnss: tip: no bytes? check PA31/PA32 UART, "
                     "PA43 GNSS_PWR, baud 9600 vs 38400, or PWR polarity\n");
              *last_hb = now;
              gnss_flush_stdout();
            }

          continue;
        }

      if (ch == '\r')
        {
          continue;
        }

      if (ch == '\n')
        {
          if (discard)
            {
              return 0;
            }

          line[cnt] = '\0';
          return (int)cnt;
        }

      if (discard)
        {
          continue;
        }

      if (cnt + 1 >= maxlen)
        {
          /* Mid-stream open or glued sentences: drop until EOL, do not abort. */
          discard = true;
          continue;
        }

      line[cnt++] = ch;
    }
}

static void gnss_print_sentence(FAR const char *line)
{
  enum minmea_sentence_id id = minmea_sentence_id(line, false);

  switch (id)
    {
      case MINMEA_SENTENCE_RMC:
        {
          struct minmea_sentence_rmc frame;

          if (minmea_parse_rmc(&frame, line))
            {
              printf("  >> RMC valid=%d date=%02d-%02d-%02d time=%02d:%02d:%02d "
                     "lat=%.6f lon=%.6f\n",
                     frame.valid ? 1 : 0,
                     frame.date.year, frame.date.month, frame.date.day,
                     frame.time.hours, frame.time.minutes, frame.time.seconds,
                     (double)minmea_tocoord(&frame.latitude),
                     (double)minmea_tocoord(&frame.longitude));
            }
          else
            {
              printf("  >> RMC parse failed\n");
            }
        }
        break;

      case MINMEA_SENTENCE_GGA:
        {
          struct minmea_sentence_gga frame;

          if (minmea_parse_gga(&frame, line))
            {
              printf("  >> GGA fix=%d sats=%d hdop=%.1f alt=%.1f m\n",
                     frame.fix_quality,
                     frame.satellites_tracked,
                     (double)minmea_tofloat(&frame.hdop),
                     (double)minmea_tofloat(&frame.altitude));
            }
          else
            {
              printf("  >> GGA parse failed\n");
            }
        }
        break;

      case MINMEA_SENTENCE_GSA:
        {
          struct minmea_sentence_gsa frame;
          int used = 0;
          int i;

          if (minmea_parse_gsa(&frame, line))
            {
              for (i = 0; i < 12; i++)
                {
                  if (frame.sats[i] > 0)
                    {
                      used++;
                    }
                }

              printf("  >> GSA mode=%c fix=%d used=%d pdop=%.1f\n",
                     frame.mode, frame.fix_type, used,
                     (double)minmea_tofloat(&frame.pdop));
            }
          else
            {
              printf("  >> GSA parse failed\n");
            }
        }
        break;

      case MINMEA_SENTENCE_GSV:
        {
          struct minmea_sentence_gsv frame;
          int i;

          if (minmea_parse_gsv(&frame, line))
            {
              printf("  >> GSV %d/%d in_view=%d",
                     frame.msg_nr, frame.total_msgs, frame.total_sats);
              for (i = 0; i < 4; i++)
                {
                  if (frame.sats[i].nr > 0)
                    {
                      printf(" | sat%02d el=%d az=%d snr=%d",
                             frame.sats[i].nr,
                             frame.sats[i].elevation,
                             frame.sats[i].azimuth,
                             frame.sats[i].snr);
                    }
                }

              printf("\n");
            }
          else
            {
              printf("  >> GSV parse failed\n");
            }
        }
        break;

      case MINMEA_SENTENCE_VTG:
        {
          struct minmea_sentence_vtg frame;

          if (minmea_parse_vtg(&frame, line))
            {
              printf("  >> VTG track=%.1f deg speed=%.2f kn\n",
                     (double)minmea_tofloat(&frame.true_track_degrees),
                     (double)minmea_tofloat(&frame.speed_knots));
            }
        }
        break;

      case MINMEA_SENTENCE_ZDA:
        {
          struct minmea_sentence_zda frame;

          if (minmea_parse_zda(&frame, line))
            {
              printf("  >> ZDA %04d-%02d-%02d %02d:%02d:%02d\n",
                     frame.date.year, frame.date.month, frame.date.day,
                     frame.time.hours, frame.time.minutes, frame.time.seconds);
            }
        }
        break;

      default:
        if (strncmp(line, "$GNTXT", 6) == 0 ||
            strncmp(line, "$GPTXT", 6) == 0)
          {
            printf("  >> TXT (module status)\n");
          }
        else if (line[0] == '$')
          {
            printf("  >> (other NMEA)\n");
          }

        break;
    }

  gnss_flush_stdout();
}

static int gnss_run_nmea(int fd, int count, int timeout_sec)
{
  char line[GNSS_LINE_MAX];
  time_t deadline;
  time_t last_hb;
  int i;
  int len;
  int got = 0;

  deadline = time(NULL) + timeout_sec;
  last_hb  = time(NULL);

  printf("test gnss: " GNSS_MOD_PN " %s @ %d baud",
         CONFIG_BOARD_L96_GNSS_DEVPATH, CONFIG_BOARD_L96_GNSS_BAUD);
  if (count > 0)
    {
      printf(" (stop after %d lines)", count);
    }
  else
    {
      printf(" (until %ds timeout)", timeout_sec);
    }

  printf("\n");
  gnss_flush_stdout();

  gnss_drain_rx(fd);

  for (i = 0; count == 0 || i < count; )
    {
      len = gnss_read_line(fd, line, sizeof(line), deadline, &last_hb);
      if (len == -ETIMEDOUT)
        {
          printf("test gnss: timeout after %d s (%d lines received)\n",
                 timeout_sec, got);
          break;
        }

      if (len < 0)
        {
          printf("test gnss: read error %d\n", -len);
          break;
        }

      if (len == 0)
        {
          continue;
        }

      /* Print every non-empty line; highlight NMEA. */

      got++;
      i++;
      if (line[0] == '$')
        {
          printf("[%d] %s\n", got, line);
          gnss_print_sentence(line);
        }
      else
        {
          printf("[%d] (non-NMEA) %s\n", got, line);
          gnss_flush_stdout();
        }
    }

  return got;
}

static int gnss_run_raw(int fd, int duration_sec)
{
  char ch;
  time_t deadline;
  time_t last_hb;
  time_t now;
  unsigned long bytes = 0;
  int ret;

  deadline = time(NULL) + duration_sec;
  last_hb  = time(NULL);

  printf("test gnss: RAW RX dump %s @ %d baud for %d s\n",
         CONFIG_BOARD_L96_GNSS_DEVPATH, CONFIG_BOARD_L96_GNSS_BAUD,
         duration_sec);
  printf("test gnss: (char printable, else hex <xx>)\n");
  gnss_flush_stdout();

  while ((now = time(NULL)) <= deadline)
    {
      ret = gnss_read_byte(fd, &ch, GNSS_POLL_MS);
      if (ret < 0)
        {
          printf("\ntest gnss: read error %d\n", -ret);
          break;
        }

      if (ret == 0)
        {
          if (now - last_hb >= GNSS_HEARTBEAT_SEC)
            {
              printf("\ntest gnss: raw waiting... %lu bytes so far\n", bytes);
              gnss_flush_stdout();
              last_hb = now;
            }

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

      gnss_flush_stdout();
    }

  printf("\ntest gnss: raw done, %lu bytes\n", bytes);
  return (bytes > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/****************************************************************************
 * UBX-MGA inject / dump
 ****************************************************************************/

static int64_t gnss_mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000;
}

static void gnss_ubx_checksum(FAR const uint8_t *data, size_t n,
                              FAR uint8_t *cka, FAR uint8_t *ckb)
{
  uint8_t a = 0;
  uint8_t b = 0;
  size_t i;

  for (i = 0; i < n; i++)
    {
      a = (uint8_t)(a + data[i]);
      b = (uint8_t)(b + a);
    }

  *cka = a;
  *ckb = b;
}

static int gnss_ubx_build(FAR uint8_t *out, size_t outsz, uint8_t cls,
                          uint8_t id, FAR const uint8_t *payload,
                          uint16_t plen)
{
  uint8_t cka;
  uint8_t ckb;

  if (outsz < (size_t)plen + 8)
    {
      return -ENOMEM;
    }

  out[0] = 0xb5;
  out[1] = 0x62;
  out[2] = cls;
  out[3] = id;
  out[4] = (uint8_t)(plen & 0xff);
  out[5] = (uint8_t)((plen >> 8) & 0xff);
  if (plen > 0 && payload != NULL)
    {
      memcpy(out + 6, payload, plen);
    }

  gnss_ubx_checksum(out + 2, (size_t)plen + 4, &cka, &ckb);
  out[6 + plen] = cka;
  out[7 + plen] = ckb;
  return (int)plen + 8;
}

static int gnss_ubx_write(int fd, FAR const uint8_t *buf, int len)
{
  int off = 0;

  while (off < len)
    {
      struct pollfd pfd;
      ssize_t n;

      pfd.fd     = fd;
      pfd.events = POLLOUT;
      if (poll(&pfd, 1, 1000) <= 0)
        {
          return -ETIMEDOUT;
        }

      n = write(fd, buf + off, (size_t)(len - off));
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
          return -EIO;
        }

      off += (int)n;
    }

  return 0;
}

static int gnss_ubx_read_frame(int fd, FAR uint8_t *cls, FAR uint8_t *id,
                               FAR uint8_t *payload, FAR uint16_t *plen,
                               int timeout_ms)
{
  enum
    {
      ST_B5 = 0,
      ST_62,
      ST_CLS,
      ST_ID,
      ST_L1,
      ST_L2,
      ST_PAY,
      ST_CKA,
      ST_CKB,
      ST_NMEA
    };

  int st = ST_B5;
  uint8_t cka = 0;
  uint8_t ckb = 0;
  uint8_t got_cka = 0;
  uint16_t len = 0;
  uint16_t idx = 0;
  int64_t deadline = gnss_mono_ms() + timeout_ms;
  char ch;
  int ret;
  int slice;

  while (gnss_mono_ms() <= deadline)
    {
      slice = (int)(deadline - gnss_mono_ms());
      if (slice < 1)
        {
          break;
        }

      if (slice > GNSS_POLL_MS)
        {
          slice = GNSS_POLL_MS;
        }

      ret = gnss_read_byte(fd, &ch, slice);
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          continue;
        }

      switch (st)
        {
          case ST_B5:
            if ((uint8_t)ch == 0xb5)
              {
                st = ST_62;
              }
            else if (ch == '$')
              {
                st = ST_NMEA;
              }
            break;

          case ST_62:
            st = ((uint8_t)ch == 0x62) ? ST_CLS : ST_B5;
            break;

          case ST_CLS:
            *cls = (uint8_t)ch;
            cka = *cls;
            ckb = cka;
            st = ST_ID;
            break;

          case ST_ID:
            *id = (uint8_t)ch;
            cka = (uint8_t)(cka + *id);
            ckb = (uint8_t)(ckb + cka);
            st = ST_L1;
            break;

          case ST_L1:
            len = (uint8_t)ch;
            cka = (uint8_t)(cka + (uint8_t)ch);
            ckb = (uint8_t)(ckb + cka);
            st = ST_L2;
            break;

          case ST_L2:
            len |= ((uint16_t)(uint8_t)ch) << 8;
            cka = (uint8_t)(cka + (uint8_t)ch);
            ckb = (uint8_t)(ckb + cka);
            if (len > UBX_MAX_PAYLOAD)
              {
                st = ST_B5;
                break;
              }

            idx = 0;
            st = (len == 0) ? ST_CKA : ST_PAY;
            break;

          case ST_PAY:
            if (idx < UBX_MAX_PAYLOAD)
              {
                payload[idx] = (uint8_t)ch;
              }

            cka = (uint8_t)(cka + (uint8_t)ch);
            ckb = (uint8_t)(ckb + cka);
            idx++;
            if (idx >= len)
              {
                st = ST_CKA;
              }
            break;

          case ST_CKA:
            got_cka = (uint8_t)ch;
            st = ST_CKB;
            break;

          case ST_CKB:
            if (got_cka == cka && (uint8_t)ch == ckb)
              {
                *plen = len;
                return 1;
              }

            st = ST_B5;
            break;

          case ST_NMEA:
            if (ch == '\n')
              {
                st = ST_B5;
              }
            break;

          default:
            st = ST_B5;
            break;
        }
    }

  return 0;
}

static FAR const char *gnss_mga_info(uint8_t code)
{
  switch (code)
    {
      case 0:
        return "accepted";
      case 1:
        return "no-time";
      case 2:
        return "type-not-supported";
      case 3:
        return "size-mismatch";
      case 4:
        return "db-store-failed";
      case 5:
        return "not-ready";
      case 6:
        return "unknown-type";
      default:
        return "?";
    }
}

static void gnss_print_ack(uint8_t cls, uint8_t id, FAR const uint8_t *pl,
                           uint16_t plen)
{
  if (cls == 0x05 && id == 0x01 && plen >= 2)
    {
      printf("  ACK-ACK cls=0x%02x id=0x%02x\n", pl[0], pl[1]);
    }
  else if (cls == 0x05 && id == 0x00 && plen >= 2)
    {
      printf("  ACK-NAK cls=0x%02x id=0x%02x\n", pl[0], pl[1]);
    }
  else if (cls == 0x13 && id == 0x60 && plen >= 8)
    {
      printf("  MGA-ACK type=%u info=%u(%s) msgId=0x%02x "
             "start=%02x %02x %02x %02x\n",
             pl[0], pl[2], gnss_mga_info(pl[2]), pl[3],
             pl[4], pl[5], pl[6], pl[7]);
    }
  else
    {
      printf("  UBX cls=0x%02x id=0x%02x len=%u\n", cls, id, plen);
    }

  gnss_flush_stdout();
}

static int gnss_ubx_send(int fd, uint8_t cls, uint8_t id,
                         FAR const uint8_t *payload, uint16_t plen)
{
  uint8_t frame[UBX_MAX_PAYLOAD + 8];
  int n;

  n = gnss_ubx_build(frame, sizeof(frame), cls, id, payload, plen);
  if (n < 0)
    {
      return n;
    }

  return gnss_ubx_write(fd, frame, n);
}

static int gnss_ubx_wait_ack(int fd, uint8_t want_cls, uint8_t want_id,
                             int timeout_ms)
{
  uint8_t cls;
  uint8_t id;
  uint8_t pl[UBX_MAX_PAYLOAD];
  uint16_t plen;
  int64_t deadline = gnss_mono_ms() + timeout_ms;
  int ret;
  int slice;

  while (gnss_mono_ms() <= deadline)
    {
      slice = (int)(deadline - gnss_mono_ms());
      if (slice < 1)
        {
          break;
        }

      ret = gnss_ubx_read_frame(fd, &cls, &id, pl, &plen, slice);
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          continue;
        }

      gnss_print_ack(cls, id, pl, plen);

      if (cls == 0x13 && id == 0x60 && plen >= 8 && pl[3] == want_id)
        {
          return (pl[0] == 1 && pl[2] == 0) ? 1 : -EIO;
        }

      if (cls == 0x05 && id == 0x01 && plen >= 2 &&
          pl[0] == want_cls && pl[1] == want_id)
        {
          return 1;
        }

      if (cls == 0x05 && id == 0x00 && plen >= 2 &&
          pl[0] == want_cls && pl[1] == want_id)
        {
          return -EIO;
        }
    }

  return 0;
}

static int gnss_mga_enable_ack(int fd)
{
  /* CFG-VALSET RAM: CFG-NAVSPG-ACKAIDING (0x10110025) = true */
  static const uint8_t pl[] =
    {
      0x00, 0x01, 0x00, 0x00,
      0x25, 0x00, 0x11, 0x10,
      0x01
    };
  int ret;

  gnss_drain_rx(fd);
  ret = gnss_ubx_send(fd, 0x06, 0x8a, pl, sizeof(pl));
  if (ret < 0)
    {
      return ret;
    }

  ret = gnss_ubx_wait_ack(fd, 0x06, 0x8a, UBX_ACK_MS);
  if (ret <= 0)
    {
      printf("test gnss: ACKAIDING VALSET %s\n",
             (ret == 0) ? "no ACK (continuing)" : "NAK");
    }

  return 0;
}

static int gnss_mga_send_time_now(int fd)
{
  uint8_t pl[24];
  struct tm tm;
  time_t now;
  int ret;

  now = time(NULL);
  {
    FAR struct tm *tmp = gmtime(&now);

    if (tmp == NULL || tmp->tm_year + 1900 < 2020)
      {
        printf("test gnss: RTC not set, skip host TIME_UTC\n");
        return 0;
      }

    tm = *tmp;
  }

  memset(pl, 0, sizeof(pl));
  pl[0] = 0x10;
  pl[1] = 0x00;
  pl[2] = 0x00;
  pl[3] = 18;
  pl[4] = (uint8_t)((tm.tm_year + 1900) & 0xff);
  pl[5] = (uint8_t)(((tm.tm_year + 1900) >> 8) & 0xff);
  pl[6] = (uint8_t)(tm.tm_mon + 1);
  pl[7] = (uint8_t)tm.tm_mday;
  pl[8] = (uint8_t)tm.tm_hour;
  pl[9] = (uint8_t)tm.tm_min;
  pl[10] = (uint8_t)tm.tm_sec;
  pl[16] = 5;
  pl[17] = 0;

  printf("test gnss: TIME_UTC %04d-%02d-%02d %02d:%02d:%02d (tAcc 5s)\n",
         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
         tm.tm_hour, tm.tm_min, tm.tm_sec);
  gnss_flush_stdout();

  gnss_drain_rx(fd);
  ret = gnss_ubx_send(fd, 0x13, 0x40, pl, sizeof(pl));
  if (ret < 0)
    {
      return ret;
    }

  ret = gnss_ubx_wait_ack(fd, 0x13, 0x40, UBX_ACK_MS);
  if (ret == 0)
    {
      printf("test gnss: TIME_UTC no MGA-ACK\n");
    }

  return 0;
}

static int gnss_eph_prepare_dir(void)
{
  int ret;

  ret = mkdir("/mnt/lfs", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      printf("test gnss: mkdir /mnt/lfs failed: %d\n", errno);
    }

  ret = mkdir(GNSS_EPH_DIR, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      printf("test gnss: mkdir %s failed: %d\n", GNSS_EPH_DIR, errno);
      return -errno;
    }

  return 0;
}

static void gnss_eph_fs_begin(void)
{
#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  myvendor_mtp_lfs_hold("test");
#endif
}

static void gnss_eph_fs_end(void)
{
#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  myvendor_mtp_lfs_release("test");
#endif
}

static int gnss_eph_inject_file(int uartfd, FAR const char *path)
{
  uint8_t buf[UBX_MAX_PAYLOAD + 8];
  uint8_t payload[UBX_MAX_PAYLOAD];
  uint8_t cka;
  uint8_t ckb;
  int fd;
  int frames = 0;
  int acked = 0;
  int nacked = 0;
  int noack = 0;
  ssize_t n;
  uint16_t plen;
  int ret;
  int wr;

  gnss_eph_fs_begin();
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      gnss_eph_fs_end();
      printf("test gnss: open %s failed: %d\n", path, errno);
      return -errno;
    }

  printf("test gnss: inject %s -> %s\n", path, CONFIG_BOARD_L96_GNSS_DEVPATH);
  gnss_flush_stdout();

  for (; ; )
    {
      n = read(fd, buf, 6);
      if (n == 0)
        {
          break;
        }

      if (n < 0)
        {
          printf("test gnss: read %s failed: %d\n", path, errno);
          close(fd);
          gnss_eph_fs_end();
          return -errno;
        }

      if (n != 6 || buf[0] != 0xb5 || buf[1] != 0x62)
        {
          printf("test gnss: %s is not a UBX stream (need 0xb5 0x62 frames)\n",
                 path);
          close(fd);
          gnss_eph_fs_end();
          return -EINVAL;
        }

      plen = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
      if (plen > UBX_MAX_PAYLOAD)
        {
          printf("test gnss: UBX payload %u too large\n", plen);
          close(fd);
          gnss_eph_fs_end();
          return -E2BIG;
        }

      if (plen > 0)
        {
          n = read(fd, payload, plen);
          if (n != (ssize_t)plen)
            {
              printf("test gnss: truncated UBX payload\n");
              close(fd);
              gnss_eph_fs_end();
              return -EIO;
            }
        }

      n = read(fd, buf + 6, 2);
      if (n != 2)
        {
          printf("test gnss: truncated UBX checksum\n");
          close(fd);
          gnss_eph_fs_end();
          return -EIO;
        }

      gnss_ubx_checksum(buf + 2, 4, &cka, &ckb);
      if (plen > 0)
        {
          uint8_t a = cka;
          uint8_t b = ckb;
          uint16_t i;

          for (i = 0; i < plen; i++)
            {
              a = (uint8_t)(a + payload[i]);
              b = (uint8_t)(b + a);
            }

          cka = a;
          ckb = b;
        }

      if (buf[6] != cka || buf[7] != ckb)
        {
          printf("test gnss: bad checksum frame %d cls=0x%02x id=0x%02x\n",
                 frames, buf[2], buf[3]);
          close(fd);
          gnss_eph_fs_end();
          return -EINVAL;
        }

      wr = gnss_ubx_build(buf, sizeof(buf), buf[2], buf[3], payload, plen);
      gnss_drain_rx(uartfd);
      ret = gnss_ubx_write(uartfd, buf, wr);
      if (ret < 0)
        {
          printf("test gnss: UART write failed: %d\n", -ret);
          close(fd);
          gnss_eph_fs_end();
          return ret;
        }

      frames++;
      printf("  send #%d cls=0x%02x id=0x%02x len=%u\n",
             frames, buf[2], buf[3], plen);
      gnss_flush_stdout();

      ret = gnss_ubx_wait_ack(uartfd, buf[2], buf[3], UBX_ACK_MS);
      if (ret > 0)
        {
          acked++;
        }
      else if (ret < 0)
        {
          nacked++;
        }
      else
        {
          noack++;
        }
    }

  close(fd);
  gnss_eph_fs_end();

  printf("test gnss: injected %d frames (ack=%d nak=%d none=%d)\n",
         frames, acked, nacked, noack);
  return (frames > 0) ? 0 : -ENOENT;
}

static int gnss_eph_dump_file(int uartfd, FAR const char *path)
{
  uint8_t cls;
  uint8_t id;
  uint8_t pl[UBX_MAX_PAYLOAD];
  uint8_t frame[UBX_MAX_PAYLOAD + 8];
  uint16_t plen;
  int fd;
  int n;
  int dbd = 0;
  int total = 0;
  int ret;
  int64_t deadline;

  gnss_eph_prepare_dir();
  gnss_eph_fs_begin();
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      gnss_eph_fs_end();
      printf("test gnss: create %s failed: %d\n", path, errno);
      return -errno;
    }

  printf("test gnss: dump MGA-DBD -> %s\n", path);
  gnss_flush_stdout();

  gnss_drain_rx(uartfd);
  ret = gnss_ubx_send(uartfd, 0x13, 0x80, NULL, 0);
  if (ret < 0)
    {
      close(fd);
      gnss_eph_fs_end();
      return ret;
    }

  deadline = gnss_mono_ms() + 8000;
  while (gnss_mono_ms() <= deadline && total < UBX_DUMP_MAX_BYTES)
    {
      ret = gnss_ubx_read_frame(uartfd, &cls, &id, pl, &plen, UBX_DUMP_IDLE_MS);
      if (ret < 0)
        {
          close(fd);
          gnss_eph_fs_end();
          return ret;
        }

      if (ret == 0)
        {
          break;
        }

      if (cls == 0x13 && id == 0x80)
        {
          n = gnss_ubx_build(frame, sizeof(frame), cls, id, pl, plen);
          if (n > 0 && write(fd, frame, (size_t)n) == n)
            {
              dbd++;
              total += n;
            }
        }
      else if (cls == 0x13 && id == 0x60)
        {
          gnss_print_ack(cls, id, pl, plen);
          break;
        }
      else
        {
          gnss_print_ack(cls, id, pl, plen);
        }
    }

  close(fd);
  gnss_eph_fs_end();
  printf("test gnss: dumped %d DBD messages, %d bytes\n", dbd, total);
  return 0;
}

static bool gnss_eph_parse_utc_name(FAR const char *name, unsigned long *utc)
{
  unsigned long v;
  int y;
  int mo;
  int d;
  int h;
  int mi;
  int s;
  char extra;
  struct tm tm;
  time_t t;

  extra = 0;
  if (sscanf(name, "mga_%lu.ubx%c", &v, &extra) == 1 &&
      v >= GNSS_TIME_MIN_UNIX)
    {
      *utc = v;
      return true;
    }

  extra = 0;
  if (sscanf(name, "mga_%4d%2d%2dT%2d%2d%2dZ.ubx%c",
             &y, &mo, &d, &h, &mi, &s, &extra) == 6)
    {
      memset(&tm, 0, sizeof(tm));
      tm.tm_year = y - 1900;
      tm.tm_mon = mo - 1;
      tm.tm_mday = d;
      tm.tm_hour = h;
      tm.tm_min = mi;
      tm.tm_sec = s;
      t = mktime(&tm);
      if (t >= (time_t)GNSS_TIME_MIN_UNIX)
        {
          *utc = (unsigned long)t;
          return true;
        }
    }

  return false;
}

static FAR const char *gnss_eph_latest(FAR char *buf, size_t buflen)
{
  DIR *dir;
  FAR struct dirent *de;
  struct stat st;
  unsigned long best = 0;
  unsigned long utc;
  char path[GNSS_EPH_PATH_MAX];
  int n;

  buf[0] = '\0';
  dir = opendir(GNSS_EPH_DIR);
  if (dir != NULL)
    {
      while ((de = readdir(dir)) != NULL)
        {
          if (!gnss_eph_parse_utc_name(de->d_name, &utc) || utc < best)
            {
              continue;
            }

          n = snprintf(path, sizeof(path), "%s/%s", GNSS_EPH_DIR, de->d_name);
          if (n < 0 || n >= (int)sizeof(path))
            {
              continue;
            }

          if (stat(path, &st) != 0 || st.st_size <= 64)
            {
              continue;
            }

          best = utc;
          if ((size_t)n + 1u <= buflen)
            {
              memcpy(buf, path, (size_t)n + 1u);
            }
        }

      closedir(dir);
    }

  if (buf[0] != '\0')
    {
      return buf;
    }

  if (stat(GNSS_EPH_DUMP, &st) == 0 && st.st_size > 64)
    {
      return GNSS_EPH_DUMP;
    }

  if (stat(GNSS_EPH_FILE, &st) == 0 && st.st_size > 64)
    {
      return GNSS_EPH_FILE;
    }

  return NULL;
}

static int gnss_run_eph(int argc, FAR char *argv[])
{
  static char latest[GNSS_EPH_PATH_MAX];
  char dump_path[GNSS_EPH_PATH_MAX];
  FAR const char *path;
  bool dump = false;
  int fd;
  int ret;

  snprintf(dump_path, sizeof(dump_path), "%s", GNSS_EPH_DUMP);
  path = gnss_eph_latest(latest, sizeof(latest));
  if (path == NULL)
    {
      path = GNSS_EPH_FILE;
    }

  if (argc > 1 && strcmp(argv[1], "dump") == 0)
    {
      dump = true;
      if (argc > 2)
        {
          path = argv[2];
        }
      else
        {
          time_t now = time(NULL);
          unsigned long utc;

          utc = (now >= (time_t)GNSS_TIME_MIN_UNIX) ?
                (unsigned long)now : GNSS_TIME_MIN_UNIX;
          snprintf(dump_path, sizeof(dump_path),
                   "%s/mga_%lu.ubx", GNSS_EPH_DIR, utc);
          path = dump_path;
        }
    }
  else if (argc > 1)
    {
      path = argv[1];
    }

  gnss_eph_prepare_dir();

  fd = gnss_uart_open();
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  gnss_mga_enable_ack(fd);

  if (dump)
    {
      ret = gnss_eph_dump_file(fd, path);
    }
  else
    {
      ret = gnss_mga_send_time_now(fd);
      if (ret == 0)
        {
          ret = gnss_eph_inject_file(fd, path);
        }
    }

  close(fd);
  return (ret < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int test_gnss_main(int argc, FAR char *argv[])
{
  int fd;
  int count;
  int got;
  bool raw_mode = false;
  int raw_sec = GNSS_RAW_DEFAULT_SEC;

  if (argc > 1 && strcmp(argv[1], "eph") == 0)
    {
      return gnss_run_eph(argc - 1, &argv[1]);
    }

  if (argc > 1 && strcmp(argv[1], "raw") == 0)
    {
      raw_mode = true;
      if (argc > 2)
        {
          raw_sec = atoi(argv[2]);
          if (raw_sec < 1)
            {
              printf("test gnss: invalid raw duration '%s'\n", argv[2]);
              return EXIT_FAILURE;
            }
        }
    }
  else
    {
      count = GNSS_DEFAULT_COUNT;
      if (argc > 1)
        {
          count = atoi(argv[1]);
          if (count < 0)
            {
              printf("test gnss: invalid count '%s'\n", argv[1]);
              return EXIT_FAILURE;
            }
        }
    }

  fd = gnss_uart_open();
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  if (raw_mode)
    {
      got = gnss_run_raw(fd, raw_sec);
    }
  else
    {
      got = gnss_run_nmea(fd, count, GNSS_READ_TIMEOUT_SEC);
    }

  close(fd);
  return (got > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* CONFIG_BOARD_L96_GNSS */
