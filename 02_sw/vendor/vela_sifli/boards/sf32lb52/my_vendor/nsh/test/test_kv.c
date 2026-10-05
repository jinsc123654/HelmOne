/**
 * @file test_kv.c
 * @brief test kv：VELA KVDB property API 探针（非 LittleFS）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <kvdb.h>

#include "test_demos.h"

#define LOGI(fmt, ...) \
  do \
    { \
      printf("test kv: " fmt "\n", ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)
#define LOGE(fmt, ...) \
  do \
    { \
      printf("test kv: ERROR " fmt "\n", ##__VA_ARGS__); \
      fflush(stdout); \
    } \
  while (0)

#define KV_STR_KEY   "persist.test.kv.str"
#define KV_I32_KEY   "persist.test.kv.i32"
#define KV_I64_KEY   "persist.test.kv.i64"
#define KV_BOOL_KEY  "persist.test.kv.bool"
#define KV_BIN_KEY   "persist.test.kv.bin"
#define KV_DEL_KEY   "persist.test.kv.del"
#define KV_TMP_KEY   "test.kv.nopersist"
#define KV_PREFIX    "persist.test.kv."

static int g_fail;
static int g_pass;
static int g_listed;

/** @brief 记录单项断言通过或失败。 */
static void kv_ok(bool cond, FAR const char *what)
{
  if (cond)
    {
      LOGI("ok  %s", what);
      g_pass++;
    }
  else
    {
      LOGE("FAIL %s", what);
      g_fail++;
    }
}

/** @brief 删除测试用 persist.test.kv.* 键。 */
static void kv_cleanup(void)
{
  (void)property_delete(KV_STR_KEY);
  (void)property_delete(KV_I32_KEY);
  (void)property_delete(KV_I64_KEY);
  (void)property_delete(KV_BOOL_KEY);
  (void)property_delete(KV_BIN_KEY);
  (void)property_delete(KV_DEL_KEY);
  (void)property_delete(KV_TMP_KEY);
}

/** @brief property_list 回调，统计测试键数量。 */
static void kv_list_cb(const char *key, const char *value, void *cookie)
{
  (void)value;
  (void)cookie;
  if (key != NULL && strncmp(key, KV_PREFIX, sizeof(KV_PREFIX) - 1) == 0)
    {
      g_listed++;
    }
}

/** @brief 打印 test kv 用法。 */
static void kv_usage(void)
{
  printf("Usage: test kv\n"
         "  VELA KVDB property API (property_set/get/delete/list).\n"
         "  Not a LittleFS test - does not open the persist path or format.\n"
         "  Keys: persist.test.kv.*  (cleaned up on exit)\n");
#ifdef CONFIG_KVDB_PERSIST_PATH
  printf("  Persist path (config only): %s\n", CONFIG_KVDB_PERSIST_PATH);
#endif
}

/** @brief 执行 KVDB API 全套探针。 */
static int kv_run(void)
{
  char buf[PROPERTY_VALUE_MAX];
  uint8_t bin_in[4] =
    {
      0x00, 0x11, 0x89, 0xff
    };
  uint8_t bin_out[8];
  ssize_t n;
  int rc;

  g_fail = 0;
  g_pass = 0;
  g_listed = 0;
  kv_cleanup();

#ifdef CONFIG_KVDB_PERSIST_PATH
  LOGI("API persist string (path=%s)", CONFIG_KVDB_PERSIST_PATH);
#else
  LOGI("API persist string");
#endif

  rc = property_set(KV_STR_KEY, "hello-kv");
  kv_ok(rc == 0, "property_set persist string");
  if (rc != 0)
    {
      LOGE("property_set rc=%d (KVDB persist not ready?)", rc);
    }

  memset(buf, 0, sizeof(buf));
  rc = property_get(KV_STR_KEY, buf, "missing");
  kv_ok(rc > 0 && strcmp(buf, "hello-kv") == 0, "property_get persist string");

  rc = property_set(KV_STR_KEY, "overwrite");
  kv_ok(rc == 0, "property_set overwrite");
  memset(buf, 0, sizeof(buf));
  (void)property_get(KV_STR_KEY, buf, "missing");
  kv_ok(strcmp(buf, "overwrite") == 0, "property_get after overwrite");

  LOGI("API persist int32/int64/bool");
  rc = property_set_int32(KV_I32_KEY, -12345);
  kv_ok(rc == 0, "property_set_int32");
  kv_ok(property_get_int32(KV_I32_KEY, 0) == -12345, "property_get_int32");

  rc = property_set_int64(KV_I64_KEY, INT64_C(0x123456789));
  kv_ok(rc == 0, "property_set_int64");
  kv_ok(property_get_int64(KV_I64_KEY, 0) == INT64_C(0x123456789),
        "property_get_int64");

  rc = property_set_bool(KV_BOOL_KEY, 1);
  kv_ok(rc == 0, "property_set_bool");
  kv_ok(property_get_bool(KV_BOOL_KEY, 0) == 1, "property_get_bool");

  LOGI("API persist binary");
  rc = property_set_binary(KV_BIN_KEY, bin_in, sizeof(bin_in), false);
  kv_ok(rc == 0, "property_set_binary");
  memset(bin_out, 0, sizeof(bin_out));
  n = property_get_binary(KV_BIN_KEY, bin_out, sizeof(bin_out));
  kv_ok(n == (ssize_t)sizeof(bin_in) &&
        memcmp(bin_out, bin_in, sizeof(bin_in)) == 0,
        "property_get_binary");

  LOGI("API delete");
  rc = property_set(KV_DEL_KEY, "gone");
  kv_ok(rc == 0, "property_set before delete");
  rc = property_delete(KV_DEL_KEY);
  kv_ok(rc == 0, "property_delete");
  memset(buf, 0, sizeof(buf));
  (void)property_get(KV_DEL_KEY, buf, "absent");
  kv_ok(strcmp(buf, "absent") == 0, "property_get after delete uses default");

  rc = property_commit();
  kv_ok(rc == 0, "property_commit");

  g_listed = 0;
  rc = property_list(kv_list_cb, NULL);
  kv_ok(rc == 0, "property_list");
  kv_ok(g_listed >= 3, "property_list saw persist.test.kv.*");

#ifndef CONFIG_KVDB_TEMPORARY_STORAGE
  LOGI("non-persist key must fail (no temporary KV)");
  rc = property_set(KV_TMP_KEY, "ram-only");
  kv_ok(rc < 0, "property_set without persist. is rejected");
#endif

  kv_cleanup();
  LOGI("done pass=%d fail=%d", g_pass, g_fail);
  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/**
 * @brief test kv 子命令入口。
 * @param argc 参数个数。
 * @param argv 参数向量。
 * @return 成功/失败退出码。
 */
int test_kv_main(int argc, FAR char *argv[])
{
  if (argc >= 2 &&
      (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0))
    {
      kv_usage();
      return EXIT_SUCCESS;
    }

  return kv_run();
}
