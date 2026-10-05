# NuttX OVNX 镜像说明

本文档描述 `wrap_nuttx_image.py` 产物的 **bin 分布**、**PC 端打包算法**，以及 **二级 bootloader 如何识别与校验**。

| 文件 | 作用 |
|------|------|
| `CONFIG_MYVENDOR_PRODUCT_VERSION` | 版本源（Kconfig / nsh defconfig） |
| `app_version.txt` | 仅当 defconfig 未写版本时的回退 |
| `ovnx_version.py` | 1 KiB 文件头描述符 |
| **`wrap_nuttx_image.sh`** | **CLion / 命令行打包入口**（调用下方 `.py`） |
| `wrap_nuttx_image.py` | 生成 / 校验 `nuttx.flash.bin` |
| `vela_my_vendor_tools.py` | `build` / `flash` 时自动 wrap；`pack-fw` 生成 OTA 文件名 |

---

## 1. 镜像布局

**元数据在 1 KiB 文件头，尾部仅 4 字节 CRC。**

```
nuttx.flash.bin
┌────────────────────────────────────────┐
│ [0 .. 1023]   1 KiB OVNX file header   │  ← 构建时间、版本、整段 image_len
├────────────────────────────────────────┤
│ [1024 ..]     payload (= nuttx.bin)    │  ← boot 拷到 PSRAM dest+0
├────────────────────────────────────────┤
│ [末尾 4B]     CRC32(payload)           │  ← 仅此，无其它 trailer 字段
└────────────────────────────────────────┘

image_len = 1024 + payload_len + 4
```

Boot **固定跳过 1024 字节** 再加载 payload；PSRAM 中向量表仍在 `dest+0`。

---

## 2. 文件头 `struct boot_ovnx_file_hdr`（296 B，小端 packed）

二级 boot 与 PC 端共用同一布局。C 定义见 `boot_loader/project/butterflmicro/board/boot_ovnx.h`：

```c
struct boot_ovnx_file_hdr
{
    char     magic[8];        /* "OVNXAPP\0" */
    char     version[256];    /* PRODUCT_VERSION[+gREV]，不足填 '\0' */
    uint32_t build_unix;      /* UTC unix 时间戳 */
    char     build_date[12];  /* "YYYYMMDDhhmm" */
    uint32_t image_len;       /* len(nuttx.flash.bin) */
    uint32_t payload_len;     /* len(nuttx.bin) */
    uint32_t hdr_size;        /* 固定 1024 */
    uint32_t flags;           /* 保留，当前 0 */
} __attribute__((packed));
```

| 偏移 | 大小 | 字段 | 含义 |
|------|------|------|------|
| 0 | 8 | `magic` | `"OVNXAPP\0"` |
| 8 | 256 | `version` | 构建版本字符串 |
| 264 | 4 | `build_unix` | UTC 时间戳 |
| 268 | 12 | `build_date` | `YYYYMMDDhhmm` |
| 280 | 4 | `image_len` | **整段** `nuttx.flash.bin` 字节数 |
| 284 | 4 | `payload_len` | `len(nuttx.bin)` |
| 288 | 4 | `hdr_size` | 固定 `1024` |
| 292 | 4 | `flags` | 保留 |

`sizeof(struct boot_ovnx_file_hdr) == 296`。`[296 .. 1023]` 填 0。

Python 打包（与结构体一致）：

```python
struct.pack("<8s256sI12sIIII", magic, version, build_unix, build_date,
            image_len, payload_len, 1024, 0).ljust(1024, b"\0")
```

Boot 解析：`g_flash_read(src, &fhdr, sizeof(fhdr))` → `boot_ovnx_parse_file_hdr(&fhdr, &info)`。

---

## 3. 尾部 CRC

- 仅 **4 字节** `uint32_t` CRC32/IEEE，覆盖 **payload**（与 `nuttx.bin` 一致）。
- 位置：`file[offset - 4 .. offset - 1]`。

---

## 4. PC 端 wrap

```text
payload     = read(nuttx.bin)
version     = resolve_app_version()  # Kconfig PRODUCT_VERSION, optional -gREV
build_unix, build_date = build_timestamp()
image_len   = 1024 + len(payload) + 4
file_hdr    = make_file_header(version, build_unix, build_date, len(payload), image_len)
tail_crc    = pack("<I", CRC32(payload))
output      = file_hdr + payload + tail_crc
```

环境变量：`SOURCE_DATE_EPOCH`（可复现时间）、`OVNX_VERSION_NO_GIT=1`（不追加 git 后缀）。

分区：`len(nuttx.bin) + 1028 ≤ ptab main max_size`。

---

## 5. 二级 Boot 流程

```text
1. 读 NAND src 前 `sizeof(struct boot_ovnx_file_hdr)` 到 `fhdr`，校验 magic / hdr_size / 长度关系
2. UART 打印：version、build_date、image_len        ← 校验前
3. 按 2KiB 分块：NAND → SRAM 暂存 → `memcpy` 到 PSRAM（只读一遍 payload）
4. 全部载入 PSRAM 后，再对 payload 做一次 CRC32，并与文件末尾 4B 比较
5. UART 打印：crc expect / crc calc / PASS 或 FAIL   ← 无论成败都打印
6. CRC 失败 → OVNXFAIL；成功 → run_img(dest)
```

### UART 示例

```text
OVNX app image:
  version     1.0.0-g1a2b3c4
  build_date  202606031200 (unix 1780465051)
  image_len   992900 0x000F2504
  crc expect  0x0A1487C5
  crc calc    0x0A1487C5
  crc result  PASS
```

失败时 `crc result  FAIL`，仍会有 expect/calc 两行，随后 `OVNXFAIL`。

---

## 6. CLion 构建后打包（Post-build）

CLion 编 NuttX 只会产出 **`cmake_out/<board>_<config>/nuttx.bin`**，不会自动生成 OVNX 烧录镜像。  
在 **构建完成后** 需要再跑 **打包脚本**（不改 `nuttx.bin`，生成同目录下的 `nuttx.flash.bin`）。

### 应使用的脚本

| 用途 | 脚本（相对 openvela 根目录） |
|------|------------------------------|
| **CLion Post-build 推荐** | `vendor/my_vendor/scripts/wrap_nuttx_image.sh` |
| 实际逻辑（一般勿直接调） | `vendor/my_vendor/scripts/wrap_nuttx_image.py` |

本仓库默认 CMake 输出目录：`cmake_out/my_vendor_nsh/`（板 `my_vendor` + 配置 `nsh`）。

### 命令行示例

```bash
# 在 openvela 根目录
vendor/my_vendor/scripts/wrap_nuttx_image.sh cmake_out/my_vendor_nsh
```

产物：

- 输入：`cmake_out/my_vendor_nsh/nuttx.bin`（CLion 构建，保持不变）
- 输出：`cmake_out/my_vendor_nsh/nuttx.flash.bin`（sftool 烧录用）
- OTA：`cmake_out/my_vendor_nsh/fw/1.0.0-Helm-One.bin`（拷到 `/mnt/kv/fw/`）
- 附带：`app_version.generated.txt`、`fw/1.0.0-Helm-One.txt`

校验已有镜像：

```bash
vendor/my_vendor/scripts/wrap_nuttx_image.sh --verify cmake_out/my_vendor_nsh/nuttx.flash.bin
```

### CLion External Tools 配置

**Settings → Tools → External Tools → +**

| 字段 | 值 |
|------|-----|
| Name | `OVNX wrap nuttx` |
| Program | `$ProjectFileDir$/vendor/my_vendor/scripts/wrap_nuttx_image.sh` |
| Arguments | `$ProjectFileDir$/cmake_out/my_vendor_nsh` |
| Working directory | `$ProjectFileDir$` |

构建 NuttX 成功后手动运行该 External Tool，或在 CLion 的 **Run Configuration → Before/After launch**（若使用自定义构建目标）里挂上同一条命令。

若 CMake 输出目录不是 `my_vendor_nsh`，将 Arguments 改为实际的 `cmake_out/<board>_<config>`。

### 与 `vela_my_vendor_tools.py` 的关系

```bash
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py wrap
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py pack-fw   # OTA：fw/1.0.0-Helm-One.bin
```

效果与 `wrap_nuttx_image.sh cmake_out/my_vendor_nsh` 相同（依赖 `vela_my_vendor_tools.py` 顶部 `BOARD_CONFIG`）。  
**CLion 只编 app、不走该工具链时，直接用 `wrap_nuttx_image.sh` 并传入 cmake 输出目录即可。**

注意：`build-boot` 用于编译 **bootloader + ftab**，不是 app 的 OVNX 打包。

---

## 7. 日常工作流

```bash
# 改过二级 boot / ptab 时
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py build-boot

# 命令行完整构建（含自动 wrap）
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py build

# CLion 仅 build 后：手动或 Post-build 执行
vendor/my_vendor/scripts/wrap_nuttx_image.sh cmake_out/my_vendor_nsh

# 烧录（优先 nuttx.flash.bin）
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py flash
```

烧录 NAND 时请使用 **`nuttx.flash.bin`**，不要使用裸 `nuttx.bin`。
