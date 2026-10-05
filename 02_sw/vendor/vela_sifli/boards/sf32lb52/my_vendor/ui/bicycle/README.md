# bicycle

goldfish 模拟器板级自行车码表，fork 自 [X-TRACK](https://github.com/FASTSHIFT/X-TRACK)（`apps/packages/demos/x_track`）。源码与资源均在 **本目录** 维护，后续只改 `bicycle/`。

| 项目 | 说明 |
|------|------|
| NSH 命令 | `bicycle` |
| 入口 | `src/bicycle_c_main.c` |
| 源码 | `src/` |
| 资源 | `../mkfs/`（打包进 LittleFS，挂载为 `/mnt/lfs/...`） |
| 总开关 | `CONFIG_VELA_BICYCLE`（`boards/vela/Kconfig` + defconfig） |
| 本地参数 | `CMakeLists.txt` 顶部 `BICYCLE_*` |

```bash
# openvela 根目录
python3 vela_emulator_tools.py build
python3 vela_emulator_tools.py run
```

详细说明（由来、配置、ROMFS、FAQ）见 **[doc/bicycle_guide.md](doc/bicycle_guide.md)**。

**UI 设计与注意事项（LiveMap 主页 / MTP / 主题 / 动画）** 见 **[doc/ui_framework.md](doc/ui_framework.md)**。  
开机 / 关机 splash（事项行、首帧画布）见 **[doc/splash.md](doc/splash.md)**。  
框架 API 细节见 **[src/framework/lv_pm/README.md](src/framework/lv_pm/README.md)**。  
矢量地图见 **[doc/utils_components.md § VectorMap](doc/utils_components.md)**。
