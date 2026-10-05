/*
 * Copyright (c) 2010-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board copy of zblue/port/kernel/init.c.
 *
 * Call z_sys_init() from the Bluetooth service process (bt_sal_le_init /
 * bt_sal_init), never from IDLE/board HCI register. sysworkq is a
 * pthread of the caller; OpenVela's do_in_service_loop() uses the
 * process-local uv_default_loop() (task TLS).
 *
 * BREDR + LE SAL may both call this; SYS_INIT must run only once so two
 * pthreads do not share sys_work_q_stack / bt_lw_stack_area.
 */

#include <stdbool.h>

/* ---- 抽换件标记（2026-09-18）----------------------------------------------
 * 1) 构建日志里可见：`ninja | grep "override compiled"` 能列出本次构建真的编译了
 *    哪些抽换件 —— 2026-09-18 曾有个抽换件其实根本不在编译库里（已剔除），
 *    改了半天没生效，就靠这种标记一眼看出来；
 * 2) 镜像里可查：`strings nuttx | grep vela_override/`（本符号 used，不会被
 *    --gc-sections 丢掉），不依赖任何编译选项（有些目标带 -w，会把 #warning 压掉）。
 * 见 docs/pitch/README.md。 */
/* ---------------------------------------------------------------------------
 * 抽换件说明（vela_override）
 *   替的是上游 : external/zblue/zblue / port/kernel/init.c
 *   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
 *   上游 blob  : f17a4edb88fa659b87bcae4d33eb40edd42f5da4     （git -C external/zblue/zblue rev-parse HEAD:port/kernel/init.c 应等于它）
 *   为什么抽换 : z_sys_init 幂等（SAL BREDR+LE 都可能调用）
 *   版本漂移自查:
 *     git -C external/zblue/zblue rev-parse HEAD:port/kernel/init.c   # 与上面的 blob 比对
 *     git -C external/zblue/zblue diff -- port/kernel/init.c          # 上游若已前进，先看这里再决定还要不要抽换
 *   机制：构建时按**文件名**把这个 .c 顶掉上游同名文件（见同目录 CMakeLists.txt 顶部表），
 *         上游 tree 保持干净、repo sync 收不走 —— 所以本文件不进 docs/pitch。
 * ------------------------------------------------------------------------- */
#pragma message("myvendor override compiled: vela_override/zblue/init.c -- 上游 external/zblue/zblue:port/kernel/init.c@f17a4edb88fa -- z_sys_init 幂等（SAL BREDR+LE 都可能调用）")
const char myvendor_override_marker_zblue_init_c[] __attribute__((used, section(".myvendor_marker"))) = "vela_override/zblue/init.c -- 上游 external/zblue/zblue:port/kernel/init.c@f17a4edb88fa -- z_sys_init 幂等（SAL BREDR+LE 都可能调用）";
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>

static bool g_z_sys_inited;

static int do_device_init(const struct init_entry *entry)
{
	const struct device *dev = entry->dev;
	int rc = 0;

	dev->state->init_res = 0;

	if (entry->init_fn.dev != NULL) {
		rc = entry->init_fn.dev(dev);
		if (rc != 0) {
			if (rc < 0) {
				rc = -rc;
			}
			if (rc > UINT8_MAX) {
				rc = UINT8_MAX;
			}
			dev->state->init_res = rc;
		}
	}

	dev->state->initialized = true;

	return rc;
}

void z_sys_init(void)
{
	if (g_z_sys_inited) {
		return;
	}

	g_z_sys_inited = true;

	STRUCT_SECTION_FOREACH(init_entry, entry) {
		const struct device *dev = entry->dev;
		int result;

		if (dev != NULL) {
			result = do_device_init(entry);
		} else {
			result = entry->init_fn.sys();
		}

		(void)result;
	}
}
