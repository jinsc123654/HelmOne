/* long_work.c - Workqueue intended for long-running operations. */

/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board copy: a second long_wq_init() used to k_work_queue_init() (zero the
 * live queue) then start another pthread on bt_lw_stack_area.
 */

#include <zephyr/kernel.h>

/* ---- 抽换件标记（2026-09-18）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件 —— 2026-09-18 曾有个抽换件其实根本不在编译库里（已剔除），
 *    改了半天没生效，就靠这种标记一眼看出来；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉），不依赖任何编译选项（有些目标带 -w，会把 #warning 压掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : external/zblue/zblue / subsys/bluetooth/host/long_wq.c
 *   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
 *   上游 blob  : b7b6e3dc03b13e1e2aecb564230aa930b8d32d45     （git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/long_wq.c 应等于它）
 *   为什么抽换 : 禁止第二条 BT LW WQ 共用 BSS 栈
 *   版本漂移自查:
 *     git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/long_wq.c   # 与上面的 blob 比对
 *     git -C external/zblue/zblue diff -- subsys/bluetooth/host/long_wq.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/zblue/long_wq.c -- 上游 external/zblue/zblue:subsys/bluetooth/host/long_wq.c@b7b6e3dc03b1 -- 禁止第二条 BT LW WQ 共用 BSS 栈")
const char myvendor_override_marker_zblue_long_wq_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/zblue/long_wq.c -- 上游 external/zblue/zblue:subsys/bluetooth/host/long_wq.c@b7b6e3dc03b1 -- 禁止第二条 BT LW WQ 共用 BSS 栈";
#include <zephyr/init.h>

K_THREAD_STACK_DEFINE(bt_lw_stack_area, CONFIG_BT_LONG_WQ_STACK_SIZE);
static struct k_work_q bt_long_wq;

int bt_long_wq_schedule(struct k_work_delayable *dwork, k_timeout_t timeout)
{
	return k_work_schedule_for_queue(&bt_long_wq, dwork, timeout);
}

int bt_long_wq_reschedule(struct k_work_delayable *dwork, k_timeout_t timeout)
{
	return k_work_reschedule_for_queue(&bt_long_wq, dwork, timeout);
}

int bt_long_wq_submit(struct k_work *work)
{
	return k_work_submit_to_queue(&bt_long_wq, work);
}

static int long_wq_init(void)
{
	const struct k_work_queue_config cfg = {.name = "BT LW WQ"};

	if (bt_long_wq.thread.init_data != NULL) {
		return 0;
	}

	k_work_queue_init(&bt_long_wq);

	k_work_queue_start(&bt_long_wq, bt_lw_stack_area,
			   K_THREAD_STACK_SIZEOF(bt_lw_stack_area),
			   CONFIG_BT_LONG_WQ_PRIO, &cfg);

	return 0;
}

SYS_INIT(long_wq_init, POST_KERNEL, CONFIG_BT_LONG_WQ_INIT_PRIO);
