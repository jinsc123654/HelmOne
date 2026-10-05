# My Vendor — SF32LB52

与 `vendor/sifli/` 同级的独立 vendor。**板级目录**与 sifli 同构；**NAND 启动**相关配置/工程集中在 `boot_loader/`。

## 快速开始

```bash
# openvela 根目录
python3 vendor/my_vendor/build_board.py build
python3 vendor/my_vendor/build_board.py build-boot   # ftab + bootloader
python3 vendor/my_vendor/build_board.py flash
```

## 目录结构

```
vendor/my_vendor/
├── build_board.py              # 构建入口（同 sifli/build_board.py）
├── boards/sf32lb52/
│   ├── drivers/                # LCD / 触摸
│   └── my_vendor/              # 板级（与 sf32lb52_devkit_lcd 同构）
│       ├── configs/nsh/        # defconfig
│       ├── scripts/            # ld.script, Make.defs
│       ├── include/            # 板级公共头
│       ├── vela_override/      # Vela 抽换
│       ├── bsp/                # BSP + 启动编排
│       ├── ctl/                # 系统控制
│       ├── services/           # BLE / MTP / 传输
│       ├── domain/             # GPX / 字体
│       ├── ui/                 # 自行车 UI
│       └── nsh/                # NSH / 探针
├── boot_loader/                # NAND 启动链（my_vendor 扩展，sifli 无）
│   ├── build.sh
│   ├── bin/                    # ftab.bin, bootloader.bin（构建产物）
│   ├── config/nsh/             # ptab.json, sftool_param.json, boot.json
│   ├── include/                # ptab.h, custom_mem_map.h（分区宏）
│   ├── scripts/                # gen_ftab.py, load_boot_config.py
│   └── project/                # vendored SiFli bootloader (scons)
├── chips/sf32lb52/
├── middleware/
└── docs/
    ├── build_guide.md
    └── tools/vela_my_vendor_tools.py   # build_board.py 实际实现
```

## 配置职责划分

| 位置 | 内容 | 类比 sifli |
|------|------|------------|
| `boards/.../configs/nsh/defconfig` | NuttX 内核/驱动 Kconfig | 相同 |
| `boards/.../scripts/` | 链接脚本、Make.defs | 相同 |
| `boot_loader/config/nsh/ptab.json` | Flash 分区表 | sifli 无（NOR XIP 不需） |
| `boot_loader/config/nsh/sftool_param.json` | sftool 烧录清单 | sifli 无 |
| `boot_loader/include/` | 分区地址 C 宏（`ptab.h`） | sifli 无 |
| `cmake_out/my_vendor_nsh/nuttx.bin` | 主固件 | `cmake_out/..._nsh/nuttx.bin` |

## defconfig 路径

| 配置项 | 指向 |
|--------|------|
| `ARCH_BOARD_CUSTOM_DIR` | `vendor/my_vendor/boards/sf32lb52/my_vendor` |
| `ARCH_CHIP_CUSTOM_DIR` | `vendor/my_vendor/chips/sf32lb52` |

## 文档

- [板卡制作指南](docs/board_guide.md)
- [编译与烧录](docs/build_guide.md)
- [Helm One 参赛作品页](docs/product.md)（卖点、受众、openvela 叙事、演示建议）
- [系统状态与口径](docs/system_states.md)（状态/按键矩阵/阈值/术语，演示与界面文案以它为准）
- [二级 Boot（2SFBL）](docs/boot_2sfbl.md)（msh、KV LittleFS、sftool / USB-CDC）
- [SD 分区布局](docs/sd_partition.md)
- [干净树必改点](docs/required_patches.md)（`repo sync` 后 zblue `id.c` + 官方 LVGL 构建脚本）
- [BLE 方案设计](docs/ble/README.md)（实施权威 / 配对模型与射频节奏 / 传感器 Central / Bring-up 手记）
- [上游必改件 pitch](docs/pitch/README.md)（替换文件、补丁、改法）
- [vela_my_vendor_tools 命令说明](docs/tools/vela_my_vendor_tools.md)（含串口 hub 卡死与自愈）

## 参考

- 官方只读：`vendor/sifli/`
- sifli 工具：`vendor/sifli/build_board.py`
