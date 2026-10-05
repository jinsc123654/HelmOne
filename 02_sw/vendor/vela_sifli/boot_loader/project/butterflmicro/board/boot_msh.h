/**
 * @file boot_msh.h
 * @brief 二级 boot 串口 msh（加载与 2s 窗口重叠）。
 *
 * Autoboot: persist.boot.target if set, else KV /fw → partition main → factory.
 * 10 consecutive B/b stay in msh (must not reset); 10 consecutive F/f jump factory.
 * Charging plug-in (no PWR): KEY1+KEY2 already down, hold 1 s → factory.
 * Release before 1 s → charge page; both keys after that never enter factory.
 * Charge page: any key held 1 s boots (KEY1 / KEY2 / PWR, including several).
 * Both keys together still do not enter factory.
 * persist.boot.pwr ("1" / "on" / "true"; nsh `ctl pwr on` writes "1"):
 * skip PWR/charge wait and autoboot.  Any other value (missing, "0", "off",
 * junk) keeps the normal key / charge-page flow; the boot banner reports
 * which of the two was read.
 * PWR at boot skips the charge page. All of PWR / charge-leave / factory
 * share the same latch + LCD/backlight + load + jump.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_MSH_H
#define BOOT_MSH_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 边加载边收热键。加载快于 2s 则补足剩余时间。
 * prepare_fn 成功则镜像已在 RAM；go_fn 跳转。
 * 连续 10 个 B/b 进 msh；连续 10 个 F/f 进工厂固件。
 */
void boot_msh_start(int (*prepare_fn)(void), void (*go_fn)(void));

/** msh `boot`/`app`：忽略 KV target，走 /fw → main → factory。 */
int boot_images_prepare_chain(void);
void boot_images_go(void);
void boot_factory_boot(void);

/** 加载循环里调用：已凑够 B/b 则返回 1。 */
int boot_msh_poll_stop(void);

/** 加载循环里调用：已凑够 F/f 则返回 1（跳过产品，进工厂）。 */
int boot_msh_want_factory(void);

/** BBB 或（正在加载产品时的）FFF：OVNX 读循环立刻返回。
 *  已经在加载 factory 时，g_want_factory 不再当 abort（combo/10x F 自己会置位）。 */
int boot_msh_abort_load(void);

/** 工厂槽正在拷贝：abort 只认 10x B。 */
void boot_msh_factory_load_begin(void);
void boot_msh_factory_load_end(void);

/** 请求进工厂。write_kv=0：充电+KEY1+KEY2，不写 persist.boot.running。 */
void boot_msh_request_factory(int write_kv);

/** 工厂加载应跳过 KV 写入。 */
int boot_msh_factory_skip_kv(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_MSH_H */
