/**
 * @file gpx_port.h
 * @brief GPX 平台抽象层：堆内存、环形队列、POSIX 文件 I/O、线程与同步。
 *
 * 便于移植时替换底层实现（仅改 gpx_port.c）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MY_VENDOR_GPX_PORT_H
#define MY_VENDOR_GPX_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* 堆内存                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief 从 BoardPSRAM 堆分配（board_malloc_psram；池满时回退 SRAM）。
 *
 * @param size  字节数
 * @return 指针；失败 NULL
 */
void *gpx_port_malloc(size_t size);

/**
 * @brief 从 BoardPSRAM 堆分配并清零。
 *
 * @param nmemb  元素个数
 * @param size   元素字节数
 * @return 指针；失败 NULL
 */
void *gpx_port_calloc(size_t nmemb, size_t size);

/**
 * @brief 释放 gpx_port_malloc / gpx_port_calloc 分配的内存。
 *
 * @param ptr  指针；NULL 安全
 */
void gpx_port_free(void *ptr);

/* ------------------------------------------------------------------------- */
/* 环形队列（定长元素，非线程安全）                                          */
/* ------------------------------------------------------------------------- */

/** 环形队列控制块（storage 由调用方提供或通过 port 分配）。 */
typedef struct gpx_port_queue
{
  uint8_t *storage;     /* 元素存储区 */
  unsigned capacity;    /* 槽位总数（须 >= 2） */
  unsigned elem_size;   /* 单个元素字节数 */
  unsigned head;        /* 队首索引 */
  unsigned tail;        /* 队尾下一个写入索引 */
} gpx_port_queue_t;

/**
 * @brief 根据批大小推荐队列深度：ceil(batch * 1.2)。
 *
 * @param batch_size  批写大小 N
 * @return 推荐 queue depth；batch_size 为 0 时返回 0
 */
unsigned gpx_port_queue_depth_for_batch(unsigned batch_size);

/**
 * @brief 初始化环形队列（不分配 storage）。
 *
 * @param queue      队列控制块
 * @param storage    外部存储区
 * @param capacity   槽位数（须 >= 2）
 * @param elem_size  元素大小（字节）
 */
void gpx_port_queue_init(gpx_port_queue_t *queue, void *storage,
                         unsigned capacity, unsigned elem_size);

/**
 * @brief 重置队列（清空 head/tail，不修改 storage 内容）。
 *
 * @param queue  队列控制块
 */
void gpx_port_queue_reset(gpx_port_queue_t *queue);

/**
 * @brief 查询当前队列元素个数。
 *
 * @param queue  队列控制块
 * @return 元素个数
 */
unsigned gpx_port_queue_count(const gpx_port_queue_t *queue);

/**
 * @brief 查询剩余可 push 槽位数（保留 1 槽区分满/空）。
 *
 * @param queue  队列控制块
 * @return 空闲槽位数
 */
unsigned gpx_port_queue_free_slots(const gpx_port_queue_t *queue);

/**
 * @brief 判断队列是否为空。
 *
 * @param queue  队列控制块
 * @return true 空，false 非空
 */
bool gpx_port_queue_empty(const gpx_port_queue_t *queue);

/**
 * @brief 判断队列是否已满。
 *
 * @param queue  队列控制块
 * @return true 满，false 未满
 */
bool gpx_port_queue_full(const gpx_port_queue_t *queue);

/**
 * @brief 向队尾追加一个元素。
 *
 * @param queue  队列控制块
 * @param elem   待写入元素指针
 * @return 0 成功；-ENOSPC 满；-EINVAL 参数无效
 */
int gpx_port_queue_push(gpx_port_queue_t *queue, const void *elem);

/**
 * @brief 按索引窥视队首起第 index 个元素（0 = 队首）。
 *
 * @param queue  队列控制块
 * @param index  相对队首偏移
 * @return 元素指针；越界或 queue 无效时 NULL
 */
const void *gpx_port_queue_peek(const gpx_port_queue_t *queue, unsigned index);

/**
 * @brief 弹出队首元素。
 *
 * @param queue  队列控制块
 * @param elem   输出缓冲区；可为 NULL（仅 advance head）
 * @return 0 成功；-EINVAL 空或参数无效
 */
int gpx_port_queue_pop(gpx_port_queue_t *queue, void *elem);

/**
 * @brief 批量丢弃队首 count 个元素（不拷贝）。
 *
 * @param queue  队列控制块
 * @param count  丢弃个数
 * @return 0 成功；-EINVAL count 无效
 */
int gpx_port_queue_advance(gpx_port_queue_t *queue, unsigned count);

/* ------------------------------------------------------------------------- */
/* 文件 I/O（POSIX）                                                         */
/* ------------------------------------------------------------------------- */

#ifndef GPX_PORT_STREAM_BUF
#  define GPX_PORT_STREAM_BUF  4096
#endif

#ifndef GPX_PORT_STREAM_BUF_MIN
#  define GPX_PORT_STREAM_BUF_MIN  256
#endif

/** REC 写缓存：32 × 4KiB PSRAM，SD 暂不可写时先攒着，关机前一直重试。 */
#ifndef GPX_PORT_WRITE_BUF
#  define GPX_PORT_WRITE_BUF  (32u * 4096u)
#endif

#ifndef GPX_PORT_FILE_MAX
#  define GPX_PORT_FILE_MAX  (32u * 1024u * 1024u)
#endif

#ifndef GPX_PORT_PATH_MAX
#  define GPX_PORT_PATH_MAX  256
#endif

/**
 * 写路径：开录先建空文件；点先入 PSRAM wbuf；落盘 open(O_APPEND)→write→fsync→close。
 * 读路径仍可持有 fd（decode）。write() 的 user buffer 经 SRAM bounce。
 */
typedef struct gpx_port_file
{
  int fd;
  bool created;
  uint16_t rpos;
  uint16_t rlen;
  uint16_t rcap;
  uint32_t wlen;
  uint32_t wcap;
  uint8_t *rbuf;
  uint8_t *wbuf;
  char path[GPX_PORT_PATH_MAX];
} gpx_port_file_t;

/**
 * @brief 以只读方式打开文件。
 *
 * @param file  文件句柄
 * @param path  路径
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_open_read(gpx_port_file_t *file, const char *path);

/**
 * @brief 记下输出路径、建空文件、分配 PSRAM 写缓存，不挂着 fd。
 *
 * 空文件先占住 inode，后续 dump / 保存走 O_APPEND。
 * 真正落盘仍是 flush/close：open → write → fsync → close。
 *
 * @param file  文件句柄
 * @param path  路径
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_open_write(gpx_port_file_t *file, const char *path);

/**
 * @brief 打开已有 GPX 以便追加（不建空文件、不截断）。
 *
 * 写缓存与 open_write 相同；落盘仍走 O_APPEND。
 *
 * @param file  文件句柄
 * @param path  已存在的路径
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_open_append(gpx_port_file_t *file, const char *path);

/**
 * @brief 关闭文件。
 *
 * @param file  文件句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_close(gpx_port_file_t *file);

/**
 * @brief 读取一个字节。
 *
 * @param file  文件句柄
 * @return 0–255 字节值，失败返回 -1
 */
int gpx_port_file_read_byte(gpx_port_file_t *file);

/**
 * @brief 只读文件大小（打开后 fstat）。
 *
 * @param file      文件句柄
 * @param size_out  输出字节数
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_size(gpx_port_file_t *file, uint32_t *size_out);

/**
 * @brief 写入原始字节（循环写满 len）。
 *
 * @param file  文件句柄
 * @param data  数据指针
 * @param len   字节数
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_write(gpx_port_file_t *file, const void *data, size_t len);

/**
 * @brief 写入 C 字符串（不含 '\0'）。
 *
 * @param file  文件句柄
 * @param str   字符串；NULL 视为成功
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_write_str(gpx_port_file_t *file, const char *str);

/**
 * @brief 格式化写入（内部缓冲 512 字节）。
 *
 * @param file  文件句柄
 * @param fmt   printf 格式串
 * @param ...   可变参数
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_write_fmt(gpx_port_file_t *file, const char *fmt, ...);

/**
 * @brief 把 wbuf 一次性写入（open→write→fsync→close）。失败不丢缓存。
 */
int gpx_port_file_flush(gpx_port_file_t *file);

/**
 * @brief 把缓存落到存储并关闭文件（关 REC 用）。
 */
int gpx_port_file_sync(gpx_port_file_t *file);

/**
 * @brief 确保输出文件父目录存在（单层 mkdir）。
 *
 * @param path  完整文件路径
 * @return 0 成功，负 errno 失败
 */
int gpx_port_file_ensure_parent_dir(const char *path);

/* ------------------------------------------------------------------------- */
/* 线程与同步（gpx_record 所需最小集合）                                     */
/* ------------------------------------------------------------------------- */

/** 互斥锁不透明存储（具体类型见 gpx_port.c）。 */
#define GPX_PORT_MUTEX_STORAGE   64
typedef struct gpx_port_mutex
{
  uint8_t storage[GPX_PORT_MUTEX_STORAGE];
} gpx_port_mutex_t;

/** 信号量不透明存储。 */
#define GPX_PORT_SEM_STORAGE     64
typedef struct gpx_port_sem
{
  uint8_t storage[GPX_PORT_SEM_STORAGE];
} gpx_port_sem_t;

/** 线程句柄不透明存储。 */
#define GPX_PORT_THREAD_STORAGE  64
typedef struct gpx_port_thread
{
  uint8_t storage[GPX_PORT_THREAD_STORAGE];
} gpx_port_thread_t;

/** 线程入口函数类型。 */
typedef void *(*gpx_port_thread_entry_t)(void *arg);

/**
 * @brief 初始化互斥锁。
 *
 * @param mutex  互斥锁
 * @return 0 成功，负 errno 失败
 */
int gpx_port_mutex_init(gpx_port_mutex_t *mutex);

/**
 * @brief 销毁互斥锁。
 *
 * @param mutex  互斥锁
 */
void gpx_port_mutex_destroy(gpx_port_mutex_t *mutex);

/**
 * @brief 加锁。
 *
 * @param mutex  互斥锁
 */
void gpx_port_mutex_lock(gpx_port_mutex_t *mutex);

/**
 * @brief 解锁。
 *
 * @param mutex  互斥锁
 */
void gpx_port_mutex_unlock(gpx_port_mutex_t *mutex);

/**
 * @brief 初始化计数信号量。
 *
 * @param sem      信号量
 * @param initial  初始计数值
 * @return 0 成功，负 errno 失败
 */
int gpx_port_sem_init(gpx_port_sem_t *sem, unsigned initial);

/**
 * @brief 销毁信号量。
 *
 * @param sem  信号量
 */
void gpx_port_sem_destroy(gpx_port_sem_t *sem);

/**
 * @brief 信号量 V 操作。
 *
 * @param sem  信号量
 */
void gpx_port_sem_post(gpx_port_sem_t *sem);

/**
 * @brief 信号量 P 操作（阻塞）。
 *
 * @param sem  信号量
 * @return 0 成功，负 errno 失败
 */
int gpx_port_sem_wait(gpx_port_sem_t *sem);

/**
 * @brief 创建并启动线程。
 *
 * @param thread      线程句柄
 * @param entry       入口函数
 * @param arg         入口参数
 * @param stack_size  栈大小（字节）；0 用实现默认
 * @param priority    调度优先级；实现相关
 * @param name        线程名（ps 显示）；NULL 则继承父任务名
 * @param stack_addr  调用方 BSS 栈；NULL 则 pthread 从 Umem malloc
 * @return 0 成功，负 errno 失败
 */
int gpx_port_thread_create(gpx_port_thread_t *thread,
                           gpx_port_thread_entry_t entry, void *arg,
                           unsigned stack_size, unsigned priority,
                           const char *name, void *stack_addr);

/**
 * @brief 等待线程结束。
 *
 * @param thread  线程句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_port_thread_join(gpx_port_thread_t *thread);

/**
 * @brief 当前线程睡眠若干毫秒。
 *
 * @param ms  毫秒数
 */
void gpx_port_sleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif

#endif /* MY_VENDOR_GPX_PORT_H */