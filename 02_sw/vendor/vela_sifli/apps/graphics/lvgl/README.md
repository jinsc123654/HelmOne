# my_vendor LVGL (vendor-owned fork)

Full copy of `openvela/apps/graphics/lvgl`, maintained under my_vendor.

## Build model (hybrid)

| Layer | Role |
|-------|------|
| **Kconfig** | `MYVENDOR_LVGL_STACK` → `imply GRAPHICS_LVGL` → `CONFIG_LV_*` from apps Kconfig |
| **CMake** | `CONFIG_MYVENDOR_LVGL_STACK` → compile **this tree** as `liblvgl` |
| **apps CMake** | Skips when `CONFIG_MYVENDOR_LVGL_STACK` (`apps/graphics/lvgl/CMakeLists.txt`) |

Vendor patches in this tree:

- `lvgl/src/stdlib/clib/lv_mem_core_clib.c` — BoardPSRAM malloc
- `lvgl/src/osal/lv_pthread.c` — draw thread names
- `myvendor_system_font.c` — system TTF preload

## After Kconfig / defconfig changes

If cmake fails with `CMAKE_SYSTEM_PROCESSOR not set`, delete stale output and reconfigure:

```bash
rm -rf cmake_out/my_vendor_nsh
python3 vela_my_vendor_tools.py build
```
