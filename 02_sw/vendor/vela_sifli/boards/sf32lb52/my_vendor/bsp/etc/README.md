# etc ROMFS 资源说明

本目录下的文件会在编译时打包进固件，开机后挂载为只读文件系统 **`/etc`**。

需开启 `CONFIG_ETC_ROMFS=y`（已在 `configs/nsh/defconfig` 中启用）。

## 登记位置

在 **`../CMakeLists.txt`** 的 `nuttx_add_romfs()` 中登记，路径相对 **`bsp/`** 目录：

```cmake
nuttx_add_romfs(
  NAME etc
  MOUNTPOINT etc
  RCSRCS
  etc/init.d/rc.sysinit    # 需预处理的脚本
  RCRAWS
  etc/init.d/rcS           # 原样复制
  etc/group
  etc/1.txt
  etc/img                  # 整目录打包（目录内新增文件无需再改 CMake）
  PATH ${CMAKE_CURRENT_BINARY_DIR}/etc)
```

## RCSRCS 与 RCRAWS

| 参数 | 用途 | 典型文件 |
|------|------|----------|
| **RCSRCS** | 编译前预处理（如展开 `#include`） | `etc/init.d/rc.sysinit` |
| **RCRAWS** | 原样复制进 ROMFS | 脚本、配置、图片、整目录 |

`RCRAWS` 支持**单个文件**或**目录**：

- 单文件：`etc/group` → 只打包该文件
- 目录：`etc/img` → 打包目录下全部文件（推荐用于图片等资源）

## 当前已打包内容

| 源码（本目录） | CMake 登记 | 设备路径 |
|----------------|------------|----------|
| `group` | `RCRAWS etc/group` | `/etc/group` |
| `1.txt` | `RCRAWS etc/1.txt` | `/etc/1.txt` |
| `init.d/rcS` | `RCRAWS etc/init.d/rcS` | `/etc/init.d/rcS` |
| `init.d/rc.sysinit` | `RCSRCS etc/init.d/rc.sysinit` | `/etc/init.d/rc.sysinit` |
| `img/`（整目录） | `RCRAWS etc/img` | `/etc/img/` |

`img/` 下新增 PNG 等资源时，**只需放入目录并重新 build**，无需再改 `CMakeLists.txt`。

当前文件：

```
img/
└── usb.png    →  /etc/img/usb.png
```

## 路径对应关系

| 源码（本目录） | 设备运行时路径 |
|----------------|----------------|
| `etc/group` | `/etc/group` |
| `etc/init.d/rcS` | `/etc/init.d/rcS` |
| `etc/img/usb.png` | `/etc/img/usb.png` |

代码中直接使用 `/etc/...` 路径即可，例如：

```c
lv_img_set_src(icon, "/etc/img/usb.png");
```

## 添加新资源的步骤

1. 将文件放到 `bsp/etc/` 下（可新建子目录，如 `img/`、`config/`）。
2. 编辑 `bsp/CMakeLists.txt`，在 `RCRAWS` 或 `RCSRCS` 中增加对应路径。
   - 单文件：写 `etc/子目录/文件名`
   - 整个子目录：写 `etc/子目录`
3. 重新编译：

```bash
cd ~/SDK/vela/openvela
./vela_my_vendor_tools.py build
```

## 注意事项

- 本板在 `bsp/CMakeLists.txt` 中直接调用 `nuttx_add_romfs()`；**不要**依赖 `add_board_rcraws()`（该方式在本板不会生效）。
- 修改 `etc/` 内文件后必须重新 build，ROMFS 内容才会更新。
- ROMFS 为只读；运行时持久化数据请写入 LittleFS 等其他分区，不要写 `/etc`。
