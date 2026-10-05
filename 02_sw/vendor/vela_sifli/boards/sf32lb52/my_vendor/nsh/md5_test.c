/**
 * @file md5_test.c
 * @brief NSH 命令 md5_test：对文件计算 MD5 并可选重复校验。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <crypto/md5.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MD5_TEST_BUF_SIZE 512
#define MD5_DIGEST_HEX_LEN 32

/** @brief 将 16 字节 MD5 摘要转为十六进制字符串。 */
static void md5_to_hex(FAR const uint8_t digest[16], FAR char hex[33])
{
  int i;

  for (i = 0; i < 16; i++)
    {
      snprintf(&hex[i * 2], 3, "%02x", digest[i]);
    }

  hex[MD5_DIGEST_HEX_LEN] = '\0';
}

/** @brief 对指定路径文件计算 MD5 摘要。 */
static int md5_file(FAR const char *path, FAR uint8_t digest[16])
{
  MD5_CTX ctx;
  uint8_t buf[MD5_TEST_BUF_SIZE];
  ssize_t nread;
  int fd;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      printf("md5_test: open %s failed: %d\n", path, errno);
      return -errno;
    }

  md5init(&ctx);

  for (;;)
    {
      nread = read(fd, buf, sizeof(buf));
      if (nread == 0)
        {
          break;
        }

      if (nread < 0)
        {
          int errcode = errno;
          close(fd);
          printf("md5_test: read %s failed: %d\n", path, errcode);
          return -errcode;
        }

      md5update(&ctx, buf, (unsigned int)nread);
    }

  close(fd);
  md5final(digest, &ctx);
  return OK;
}

/**
 * @brief NSH 命令 md5_test：对文件计算 MD5 并可选重复校验。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int main(int argc, FAR char *argv[])
{
  FAR const char *path = "/etc/1.txt";
  int count = 1;
  uint8_t digest[16];
  char hex[33];
  char first_hex[33] = {0};
  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
          path = argv[++i];
        }
      else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
          count = atoi(argv[++i]);
        }
      else
        {
          printf("Usage: md5_test [-f file] [-c count]\n");
          return ERROR;
        }
    }

  if (count <= 0)
    {
      printf("md5_test: invalid count %d\n", count);
      return ERROR;
    }

  for (i = 0; i < count; i++)
    {
      if (md5_file(path, digest) < 0)
        {
          return ERROR;
        }

      md5_to_hex(digest, hex);
      printf("%s\n", hex);

      if (i == 0)
        {
          strlcpy(first_hex, hex, sizeof(first_hex));
        }
      else if (strcmp(first_hex, hex) != 0)
        {
          printf("md5_test: mismatch detected at iteration %d\n", i + 1);
          return ERROR;
        }
    }

  printf("md5_test: done count=%d\n", count);
  return OK;
}
