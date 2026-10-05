#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/keys.c
#   上游 blob: 8fafba770a46b4e9ef767e2737087f74a33c17af
#   为什么   : CONFIG_SETTINGS 关着 ⇒ 配对密钥（含 IRK/LTK）只活在 RAM：
#              重启/烧录后设备自己的身份与密钥重新生成，而手机还记着上一次的
#              ⇒ 加密握手对不上（现场 hciReason: 22，服务发现走不动）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/smp.c
#   上游 blob: 1ec3139cd678e9b3628509ff59877fedf0957f79
#   为什么   : 保存钩子要挂在"配对完成"这条总会跑的路径上 —— bt_keys_store()
#              是 settings 提交链的包装（keys.h 里退化成 `return 0` 的 inline），
#              挂它一次也不会执行
#   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
#   版本漂移自查:
#     git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/keys.c
#     git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/smp.c
#     git -C external/zblue/zblue diff -- subsys/bluetooth/host/keys.c
#   机制：CMake 在本目录下调用本脚本，生成改写后的构建副本（上游 git 不动）。
# ---------------------------------------------------------------------------
"""把 zblue 的配对密钥（含 IRK/LTK）落盘到 `/mnt/kv/bt_keys.bin`。

`CONFIG_SETTINGS` 关着 ⇒ zblue 的 `bt_keys_store()`（→ `bt_settings_store_keys()`）
是空动作（keys.h:217 是 `return 0` 的 inline），配对出来的 LTK/IRK 只活在 RAM：
**每次重启设备重新生成身份/密钥，而手机还记着上一次的** —— 两边对不上，链路
能建但不加密/服务发现走不动，Android 超时拆链（现场 `hciReason: 22`）。

做法：**只搬字节**，复用 zblue 自己的序列化尺寸 `BT_KEYS_STORAGE_LEN`
（`keys->storage_start` 起；恢复方向 = `bt_keys_get_addr()` + memcpy 回
`storage_start`，与 settings 的 `keys_set()` 恢复路径同一手法，不猜结构体）。

落点：
  · smp.c  `smp_pairing_complete()` 尾部（`bt_keys_store(...)` 之后，配对完成）= 整池落盘
  · keys.c `bt_keys_clear()` 尾部（解绑/配对失败）                = 整池落盘
  · keys.c `bt_keys_reset()`（adapter enable/disable 会清池）     = 允许下次重新恢复
  · **恢复入口由外部调用**：`myvendor_keys_kv_load_all()`（app 侧在 adapter enable
    之后、开广播之前调一次 —— 见 ble_companion.c 的 companion_start_advertising()）。
    恢复每条密钥时**顺带 `bt_id_add()` 写进控制器解析列表（RAL）**：
    上游那一步挂在 settings 提交链（`keys_commit()`）上，而本板 `CONFIG_SETTINGS`
    关着 ⇒ 不补这一句，「从 KV 恢复的密钥永远进不了 RAL」。
    **不要挂在 bt_keys_find*() 上**：那条路会被 `bt_lookup_id_addr()` 从**连接/扫描
    事件处理**里调用，而本板的事件处理跑在 BT 收包的中断上下文里（现场崩溃签名
    `arm_memfault.c:136 irq=1`）—— 在中断里做 open/read/close（还会拿 FS 的互斥、
    吃 ~1 KB 栈）必崩。查找路径里只留一个 bool 判断。

文件：`"BTKY" | ver u8 | 0 u8 | n u16 | 记录 × n | crc32`
      记录 = `dev_id u8 | id u8 | addr_type u8 | addr[6] | storage[BT_KEYS_STORAGE_LEN]`

自检：`git -C external/zblue/zblue diff -- subsys/bluetooth/host/keys.c` 应只有本补丁
的插入（构建产物在 `cmake_out/<cfg>/myvendor_zblue/myvendor_keys.c`，可直接比对）。
"""

import sys
from pathlib import Path

MARK = "MYVENDOR BUILD: patch_keys_kv.py"

HEADER = r'''
/* ---- my_vendor: 配对密钥落盘（vela_override/zblue/patch_keys_kv.py）-----------
 * settings 关着 ⇒ bt_keys_store() 是空动作；这里把同一份 BT_KEYS_STORAGE_LEN
 * 字节存到 KV，开机再塞回 key_pool。只搬字节，不猜结构体。
 *
 * 输出走 port 的 printk（`zephyr/sys/printk.h` → syslog(LOG_INFO)），**不走
 * LOG_WRN**：本板 .config 里没有 BT_KEYS_LOG_LEVEL，`LOG_LEVEL` 展开成 0，
 * keys.c 里所有 LOG_* 都被编译掉了（实测 `strings nuttx | grep "Invalid key
 * length"` 为空）—— 用 LOG_WRN 记日志等于没有。 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <zephyr/sys/printk.h>

#define MYVENDOR_KEYS_KV_PATH     "/mnt/kv/bt_keys.bin"
#define MYVENDOR_KEYS_KV_MAGIC    0x594b5442u   /* "BTKY" */
#define MYVENDOR_KEYS_KV_TRIES    32u           /* /mnt/kv 可能比 BT 起得晚 */
#define MYVENDOR_KEYS_KV_EMPTY    0xffffffffu   /* 文件合法但没有记录 */

struct myvendor_keys_rec_s
{
	uint8_t dev_id;
	uint8_t id;
	uint8_t addr_type;
	uint8_t addr[6];
	uint8_t storage[BT_KEYS_STORAGE_LEN];
};

static bool     myvendor_keys_loaded;
static uint8_t  myvendor_keys_tries;

static uint32_t myvendor_keys_crc(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t c = 0x811c9dc5u;

	while (n-- > 0u) {
		c ^= *b++;
		c *= 16777619u;
	}

	return c;
}

/* 读文件：>=1 = 记录数；0 = 没文件/坏了（调用方可重试）；EMPTY = 合法但没有记录。 */
static unsigned myvendor_keys_read(struct myvendor_keys_rec_s *out, unsigned max)
{
	uint8_t  hdr[8];
	uint32_t crc_file, crc_calc;
	uint16_t n;
	int      fd;
	size_t   want;

	fd = open(MYVENDOR_KEYS_KV_PATH, O_RDONLY);
	if (fd < 0) {
		return 0;
	}
	if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
		close(fd);
		return 0;
	}
	if (memcmp(hdr, "BTKY", 4) != 0) {
		printk("bt_keys kv: bad magic\n");
		close(fd);
		return 0;
	}
	memcpy(&n, hdr + 6, sizeof(n));
	if (n == 0u) {
		/* 合法但空（解绑/配对失败时 `save_pool` 会写成 0 条）—— 别当坏的，
		 * 更别让调用方以为"没有文件"而一直重试。 */
		close(fd);
		return MYVENDOR_KEYS_KV_EMPTY;
	}
	if (n > (uint16_t)max) {
		printk("bt_keys kv: bad count %u\n", (unsigned)n);
		close(fd);
		return 0;
	}
	want = (size_t)n * sizeof(out[0]);
	if (read(fd, out, want) != (ssize_t)want ||
	    read(fd, &crc_file, sizeof(crc_file)) != (ssize_t)sizeof(crc_file)) {
		printk("bt_keys kv: short read\n");
		close(fd);
		return 0;
	}
	close(fd);
	crc_calc = myvendor_keys_crc(out, want);
	if (crc_calc != crc_file) {
		printk("bt_keys kv: crc mismatch %08x != %08x\n", (unsigned)crc_calc,
		       (unsigned)crc_file);
		return 0;
	}

	return n;
}

/* 整池重写（0 条 = 文件里只剩头；解绑后就该这样）。 */
static void myvendor_keys_write(const struct myvendor_keys_rec_s *rec, unsigned n)
{
	uint8_t  hdr[8];
	uint32_t crc;
	uint16_t nn = (uint16_t)n;
	int      fd;

	memcpy(hdr, "BTKY", 4);
	hdr[4] = 1;      /* ver */
	hdr[5] = 0;
	memcpy(hdr + 6, &nn, sizeof(nn));

	crc = myvendor_keys_crc(rec, (size_t)n * sizeof(rec[0]));

	fd = open(MYVENDOR_KEYS_KV_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printk("bt_keys kv: open for write failed\n");
		return;
	}
	(void)write(fd, hdr, sizeof(hdr));
	if (n > 0u) {
		(void)write(fd, rec, (size_t)n * sizeof(rec[0]));
	}
	(void)write(fd, &crc, sizeof(crc));
	close(fd);
}

/* 把**当前密钥池**整体落盘（配对完成 / 解绑时调；≤ CONFIG_BT_MAX_PAIRED 条）。
 *
 * 空槽判定用 host 自己的约定 `bt_addr_le_eq(&keys->addr, BT_ADDR_LE_ANY)`
 * （bt_keys_get_addr() 找空槽用的就是它）。半成品槽（地址填了、密钥还没协商完）
 * 存下去也无害：那份字节里的 `keys` 位图是 0，恢复后不会被任何
 * bt_keys_find(type) 命中。 */
void myvendor_keys_kv_save_pool(void)
{
	struct myvendor_keys_rec_s rec[CONFIG_BT_MAX_PAIRED];
	unsigned n = 0;
	unsigned i, j;

	for (i = 0; i < ARRAY_SIZE(key_pool); i++) {
		for (j = 0; j < ARRAY_SIZE(key_pool[i].key_pool); j++) {
			struct bt_keys *k = &key_pool[i].key_pool[j];
			struct myvendor_keys_rec_s *r;

			if (bt_addr_le_eq(&k->addr, BT_ADDR_LE_ANY) || n >= ARRAY_SIZE(rec)) {
				continue;
			}
			r = &rec[n++];
			r->dev_id    = (uint8_t)i;
			r->id        = k->id;
			r->addr_type = k->addr.type;
			memcpy(r->addr, k->addr.a.val, sizeof(r->addr));
			memcpy(r->storage, k->storage_start, BT_KEYS_STORAGE_LEN);
		}
	}

	myvendor_keys_write(rec, n);
	printk("bt_keys kv: saved %u record(s)\n", n);
}

/* 恢复（幂等；失败按 TRIES 限次重试 —— 万一调得早、`/mnt/kv` 还没挂上）。
 *
 * **只在普通线程里调**（ble_companion 开广播之前，见 patch 头注释）：不要放进
 * bt_keys_find*() 那种会被连接/扫描事件处理（本板是中断上下文）走到的路径。 */
void myvendor_keys_kv_load_all(void)
{
	struct myvendor_keys_rec_s rec[CONFIG_BT_MAX_PAIRED];
	unsigned n;
	unsigned applied = 0;
	unsigned i;

	if (myvendor_keys_loaded) {
		return;
	}
	if (myvendor_keys_tries >= MYVENDOR_KEYS_KV_TRIES) {
		return;
	}
	myvendor_keys_tries++;

	n = myvendor_keys_read(rec, ARRAY_SIZE(rec));
	if (n == 0u) {
		return;
	}
	if (n == MYVENDOR_KEYS_KV_EMPTY) {
		/* 文件在、但一条密钥都没有（配过又解绑了）：算"恢复完成"，不重试。 */
		myvendor_keys_loaded = true;
		return;
	}

	for (i = 0; i < n; i++) {
		struct bt_dev *hdev = bt_dev_get(rec[i].dev_id);
		bt_addr_le_t   addr;
		struct bt_keys *keys;

		if (hdev == NULL || hdev->keys == NULL) {
			continue;
		}
		addr.type = rec[i].addr_type;
		memcpy(addr.a.val, rec[i].addr, sizeof(rec[i].addr));

		keys = bt_keys_get_addr(hdev, rec[i].id, &addr);
		if (keys == NULL) {
			printk("bt_keys kv: no slot for %s\n", bt_addr_le_str(&addr));
			continue;
		}
		memcpy(keys->storage_start, rec[i].storage, BT_KEYS_STORAGE_LEN);

		/* **把这条 IRK 写进控制器的解析列表（RAL）**——主机侧的
		 * `bt_lookup_id_addr()` 只能把"已收到的包"翻译回身份地址，控制器
		 * 侧没有任何解析能力时，手机轮换 RPA 连上来报的还是随机地址。
		 *
		 * 上游把这一步挂在 settings 提交链上（`keys_commit()` →
		 * `bt_id_add()`），而本板 `CONFIG_SETTINGS` 是关的 ⇒ 那条路从不执行，
		 * 于是"从 KV 恢复的密钥永远进不了 RAL"。配对当场那条（smp.c 里
		 * `bt_id_add()`）是另一条路，所以现场表现成"刚配完能认、重启后认不出"。
		 *
		 * 这里补上，位置与上游语义一致（恢复完即入表）。控制器不支持时
		 * `bt_id_add()` 只打一行 error 就返回，不影响本函数的其余部分。 */
		bt_id_add(hdev, keys);

		/* **把结果打出来**——这是"控制器 RAL 到底有没有生效"的**唯一可见证据**：
		 * id.c 里 `Failed to add IRK to controller` 那几条是 LOG_ERR，而本板
		 * BT 日志等级是 0（见本文件头注释：keys.c 的 LOG_* 全被编译掉），
		 * 出错了也一声不吭。判读：
		 *   · `rl_size=0` ⇒ 板级 HCI 楔子仍报"控制器不会解析"（该报 3，见
		 *     `chips/sf32lb52/sf32lb52_bth4.c` 的 SF32LB52_HCI_RL_SIZE）；
		 *   · `rl_size=3 entries=1/2/3` ⇒ 逐条进了 RAL，控制器会解析 RPA；
		 *   · `rl_size=3` 但 `entries` 不涨 ⇒ LCPU 拒了 Add Device To
		 *     Resolving List，退回主机侧解析（`patch_keys_kv.py` 的 IRK
		 *     落盘仍是那条路的保障）。
		 */
		printk("bt_keys kv: %s -> RAL rl_size=%u entries=%u\n",
		       bt_addr_le_str(&addr), hdev->le.rl_size, hdev->le.rl_entries);

		applied++;
	}

	/* 一条都没塞进去（adapter 还没起来）就别置位，留给下一次调用重试。 */
	if (applied > 0u) {
		myvendor_keys_loaded = true;
	}
	printk("bt_keys kv: restored %u/%u record(s)\n", applied, n);
}
'''

# 查找/遍历入口**不再挂钩子**（见头注释：那条路会从中断上下文走到，不能做文件 IO）。
RESET_SIG = "void bt_keys_reset(struct bt_dev *hdev)"
RESET_ANCHOR = "memset(&hdev->keys->key_pool, 0, sizeof(hdev->keys->key_pool));"

CLEAR_SIG = "void bt_keys_clear(struct bt_dev *hdev, struct bt_keys *keys)"
CLEAR_ANCHOR = "(void)memset(keys, 0, sizeof(*keys));"

# smp.c：配对完成（LE）—— `bt_keys_store()` 在这里只是个空动作，紧跟其后才是真落盘。
SMP_ANCHOR = "bt_keys_store(conn->hdev->dev_id, conn->le.keys);"
# 声明放 include 区（文件作用域、无条件编译）；别放 `smp_pairing_complete` 前面 ——
# 那里是 `#if defined(CONFIG_BT_PRIVACY) || …` 里的前置声明，条件一变就会变成隐式声明。
SMP_DECL_ANCHOR = '#include "smp.h"'

STMPS = {
    "keys.c": ("zblue/patch_keys_kv.py",
               "external/zblue/zblue / subsys/bluetooth/host/keys.c",
               "8fafba770a46",
               "配对密钥(含 IRK)落 /mnt/kv/bt_keys.bin（settings 关着）"),
    "smp.c": ("zblue/patch_keys_kv.py",
              "external/zblue/zblue / subsys/bluetooth/host/smp.c",
              "1ec3139cd678",
              "配对完成即落盘 key_pool（钩子不能挂 bt_keys_store）"),
}


def safe_path(raw: str) -> Path:
    """构建期调用，仍按不可信输入处理：拒绝 '..'，只接受文件路径。"""
    if ".." in Path(raw).parts:
        raise SystemExit("refusing path containing '..': %s" % raw)
    p = Path(raw).resolve()
    if p.is_dir():
        raise SystemExit("expected a file path, got a directory: %s" % p)
    return p


def insert_after_signature(src: str, sig: str, text: str) -> str:
    """在函数体第一行之后插入（sig 是函数定义，先找到 '{' 再找换行）。"""
    i = src.find(sig)
    if i < 0:
        return src
    j = src.find("\n{", i)
    if j < 0:
        return src
    j = src.find("\n", j + 1)
    if j < 0:
        return src
    return src[:j + 1] + text + src[j + 1:]


def insert_after_anchor(src: str, sig: str, anchor: str, text: str) -> str:
    """只在 sig 这个函数的函数体里找 anchor（同名锚点别处也有，见 keys_set）。"""
    i = src.find(sig)
    if i < 0:
        return src
    j = src.find("\n}", i)          # 函数尾（keys.c 里这几个函数都不含嵌套 '}' 在行首）
    if j < 0:
        return src
    k = src.find(anchor, i, j)
    if k < 0:
        return src
    e = src.find("\n", k)

    return src[:e + 1] + text + src[e + 1:]


def patch_keys_c(src: str) -> tuple[str, int]:
    anchor = "static struct bt_keys_pool key_pool[CONFIG_BT_NUM_CTLRS];"
    if anchor not in src:
        raise SystemExit("keys.c anchor (key_pool) not found")
    src = src.replace(anchor, anchor + "\n" + HEADER, 1)

    out = insert_after_anchor(src, RESET_SIG, RESET_ANCHOR,
                              "\tmyvendor_keys_loaded = false;   "
                              "/* adapter enable/disable 会清池，留着下次再恢复 */\n"
                              "\tmyvendor_keys_tries  = 0;      "
                              "/* 预算按「池世代」重置 —— 不清零的话，本开机内累计\n"
                              "\t                                * 32 次尝试用完就再也灌不回来，\n"
                              "\t                                * 手机 IRK 丢失 ⇒ RPA 解析不出发\n"
                              "\t                                * 身份地址 ⇒ 每次连上看到的 MAC 都不一样、\n"
                              "\t                                * 记录/准入认不出（2026-09-23 现场）。 */\n")
    resets = 1 if out != src else 0
    src = out

    out = insert_after_anchor(src, CLEAR_SIG, CLEAR_ANCHOR,
                              "\tmyvendor_keys_kv_save_pool();   "
                              "/* 解绑/配对失败：把这条从文件里也去掉 */\n")
    clears = 1 if out != src else 0
    src = out

    if resets == 0 or clears == 0:
        raise SystemExit("keys.c hooks incomplete: reset=%d clear=%d"
                         % (resets, clears))

    return src, 2


def patch_smp_c(src: str) -> tuple[str, int]:
    if src.count(SMP_ANCHOR) != 1:
        raise SystemExit("smp.c pairing-complete anchor is not unique")

    decl = ("\n/* my_vendor: 配对密钥落盘（keys.c 里实现，见 patch_keys_kv.py） */\n"
            "extern void myvendor_keys_kv_save_pool(void);\n")
    i = src.find(SMP_DECL_ANCHOR + "\n")
    if i < 0:
        raise SystemExit("smp.c keys.h include not found")
    src = src[:i + len(SMP_DECL_ANCHOR) + 1] + decl + src[i + len(SMP_DECL_ANCHOR) + 1:]

    call = ("\t\t\t/* my_vendor: settings 关着，上面那句是空动作，这里才是真落盘 */\n"
            "\t\t\tmyvendor_keys_kv_save_pool();\n")
    src = src.replace(SMP_ANCHOR + "\n", SMP_ANCHOR + "\n" + call, 1)

    return src, 1


def stamp(text: str, name: str) -> str:
    script, upstream, blob, why = STMPS[name]
    sym = "patch_keys_kv_" + name.replace(".", "_")
    tag = "vela_override/%s -- 上游 %s@%s -- %s" % (script, upstream, blob, why)

    return text + (
        "\n/* myvendor override (build-time patch): " + script + "\n"
        " *   上游 " + upstream + " @ " + blob + "\n"
        " *   原因 " + why + " */\n"
        '#pragma message("myvendor override compiled(patch): ' + tag + ' ")\n'
        "const char myvendor_override_patch_marker_" + sym + "[]\n"
        '    __attribute__((used, section(".myvendor_marker"))) = "' + tag + '";\n')


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_keys_kv.py SRC DST", file=sys.stderr)
        return 2

    src_path = safe_path(sys.argv[1])
    dst_path = safe_path(sys.argv[2])
    src = src_path.read_text(encoding="utf-8")

    name = src_path.name
    if name not in STMPS:
        print("patch_keys_kv.py: unexpected source %s" % name, file=sys.stderr)
        return 2

    if MARK in src:
        dst_path.write_text(src, encoding="utf-8")
        print("my_vendor: %s already patched" % name)
        return 0

    if name == "keys.c":
        out, _ = patch_keys_c(src)
        detail = "save=clear+reset(%d hooks) load=external" % 2
    else:
        out, hooks = patch_smp_c(src)
        detail = "save=1"

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(stamp(out, name), encoding="utf-8")
    print("my_vendor: override patch patch_keys_kv.py -> subsys/bluetooth/host/%s "
          "-- 配对密钥(含 IRK)落 /mnt/kv/bt_keys.bin | %s" % (name, detail))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
