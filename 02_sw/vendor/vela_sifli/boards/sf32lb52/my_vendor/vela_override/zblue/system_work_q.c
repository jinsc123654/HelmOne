/*
 * Copyright (c) 2016 Wind River Systems, Inc.
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board copy: skip a second k_work_queue_start so two pthreads do not share
 * sys_work_q_stack (see z_sys_init once-guard in init.c).
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
 *   替的是上游 : external/zblue/zblue / kernel/system_work_q.c
 *   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
 *   上游 blob  : 5cc26fd82e14a865367fe36e4ca1a61bbd281abf     （git -C external/zblue/zblue rev-parse HEAD:kernel/system_work_q.c 应等于它）
 *   为什么抽换 : 禁止第二条 sysworkq 共用 BSS 栈
 *   版本漂移自查:
 *     git -C external/zblue/zblue rev-parse HEAD:kernel/system_work_q.c   # 与上面的 blob 比对
 *     git -C external/zblue/zblue diff -- kernel/system_work_q.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/zblue/system_work_q.c -- 上游 external/zblue/zblue:kernel/system_work_q.c@5cc26fd82e14 -- 禁止第二条 sysworkq 共用 BSS 栈")
const char myvendor_override_marker_zblue_system_work_q_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/zblue/system_work_q.c -- 上游 external/zblue/zblue:kernel/system_work_q.c@5cc26fd82e14 -- 禁止第二条 sysworkq 共用 BSS 栈";
#include <zephyr/init.h>

static K_KERNEL_STACK_DEFINE(sys_work_q_stack,
			     CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE);

struct k_work_q k_sys_work_q;

static int k_sys_work_q_init(void)
{
	struct k_work_queue_config cfg = {
		.name = "sysworkq",
		.no_yield = IS_ENABLED(CONFIG_SYSTEM_WORKQUEUE_NO_YIELD),
		.essential = true,
	};

	if (k_sys_work_q.thread.init_data != NULL) {
		return 0;
	}

	k_work_queue_start(&k_sys_work_q,
			    sys_work_q_stack,
			    K_KERNEL_STACK_SIZEOF(sys_work_q_stack),
			    CONFIG_SYSTEM_WORKQUEUE_PRIORITY, &cfg);
	return 0;
}

SYS_INIT(k_sys_work_q_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
