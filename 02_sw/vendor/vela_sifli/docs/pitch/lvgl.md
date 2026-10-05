# LVGL：官方 apps 构建脚本避让 + SiFli EPIC 头

**上游文件：**

- `apps/graphics/lvgl/CMakeLists.txt`
- `apps/graphics/lvgl/Makefile`
- `apps/graphics/lvgl/lvgl/Kconfig` ← **在另一个仓库里**（嵌套 project `apps_graphics_lvgl`）

**替换件：** [replace/apps/graphics/lvgl/](replace/apps/graphics/lvgl/)  
**补丁：** [patches/apps-lvgl-myvendor-stack.patch](patches/apps-lvgl-myvendor-stack.patch)（前两个文件）+
[patches/apps-lvgl-kconfig.patch](patches/apps-lvgl-kconfig.patch)（Kconfig，**在 `apps/graphics/lvgl/lvgl` 里打**）  
**对照 HEAD：** `apps` @ `017abdbef`；`apps/graphics/lvgl/lvgl` @ `0f6336dab`

## 组件 id（换版本前先对这张表）

| 项 | 值 |
|---|---|
| 仓库 | `apps` |
| 写入时 HEAD | `017abdbef40f8c6b52ccb56c50fdfc7cb5fbb375` |
| 文件① | `graphics/lvgl/CMakeLists.txt`，HEAD blob `7bc18e19bb77162474e836d5f042209f723b03c0` |
| 文件② | `graphics/lvgl/Makefile`，HEAD blob `c2f34d983dd0cfb5b0f354655e64b5744bb582f6` |
| 仓库②| `apps/graphics/lvgl/lvgl`（manifest 里叫 `apps_graphics_lvgl`，**独立 git 仓库**） |
| 写入时 HEAD | `0f6336dab851a132b2794c251d9d03f32892953e` |
| 文件③ | `Kconfig`，HEAD blob `0188e9d4cd537a80cbf3205fbbc29df396840ba1` → 打完后 `12e479a8caeea45fec700dbeb220bea5cc901446` |

```bash
git -C apps rev-parse HEAD:graphics/lvgl/CMakeLists.txt HEAD:graphics/lvgl/Makefile      # 应等于上表两个 blob
git -C apps apply --check ../vendor/my_vendor/docs/pitch/patches/apps-lvgl-myvendor-stack.patch
git -C apps/graphics/lvgl/lvgl rev-parse HEAD:Kconfig                                    # 应为 0188e9d4…
git -C apps/graphics/lvgl/lvgl apply --check ../vendor/my_vendor/docs/pitch/patches/apps-lvgl-kconfig.patch
```

### 文件③ `Kconfig` 为什么必须打（2026-09-19 实测踩过）

本板的 `CONFIG_LV_*` 全部来自 `apps/graphics/lvgl/Kconfig` 里那行
`osource "$APPSDIR/graphics/lvgl/lvgl/Kconfig"` —— 也就是说 **config 生成读的是官方（上游）LVGL 树的 Kconfig**，
哪怕真正参与编译的源码在 `vendor/my_vendor/apps/graphics/lvgl/`。上游那版是旧 LVGL 的，缺
`LV_SUNDAY_STR`/`LV_MONDAY_STR` 等符号，于是 config 里没有它们，而 vendor 的 9.5 源码在
`widgets/calendar/lv_calendar.c` 里要用：

```
lv_conf_kconfig.h:317:52: error: 'CONFIG_LV_SUNDAY_STR' undeclared here (not in a function)
```

⇒ 官方树的 `.c/.h` 可以保持上游原样（反正不编），**唯独 `Kconfig` 必须保留 9.5 那版**。
打完这份补丁后，如果构建目录里已有旧 `.config`，要删掉让它重新推导一次
（`rm cmake_out/<cfg>/.config`），否则缺的符号不会自己回来。

HEAD 动了但 blob 没动 ⇒ 两个脚本没被改过，照下面的前后对照改；blob 也变了 ⇒ 先看上游怎么改的再重做。
**这两个文件是"整文件替换件"**（`replace/apps/graphics/lvgl/…` 里就是"改后"全文），
所以这里给的是**完整改动块**（上游原文 ↔ 替换件原文），不是片段。

本板走 CMake（`build_board.py`）。`CMakeLists.txt` 是必打；`Makefile` 给 make 构建和「不用 my_vendor 栈、仍编官方 LVGL + EPIC」留后路。

---

## 原因

本板 `CONFIG_MYVENDOR_LVGL_STACK=y`。真正的 LVGL 9.5 + NuttX 移植 + EPIC 在：

```
vendor/my_vendor/apps/graphics/lvgl/
```

由板级 CMake 拉进构建：

```cmake
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../apps/graphics/lvgl
               myvendor_lvgl)
```

（`boards/sf32lb52/my_vendor/CMakeLists.txt`）

与此同时，OpenVela 仍会处理 **官方** `apps/graphics/lvgl/CMakeLists.txt`。那边在 `CONFIG_GRAPHICS_LVGL` 下会 `nuttx_add_library(lvgl)`。两个脚本都会定义同一个 `lvgl` target → CMake 配置失败。

所以官方入口必须在 `CONFIG_MYVENDOR_LVGL_STACK` 时 **立刻 `return()`**，把编译权交给 vendor 树。

第二段是 `CONFIG_LV_USE_SIFLI_EPIC`：SiFli 绘制后端要芯片驱动和 CMSIS 头，以及 `SOC_BF0_HCPU` / `SF32LB52X`。vendor 自己的 `CMakeLists.txt` 已经写了；官方脚本也要写，否则有人关掉 my_vendor 栈、仍用官方 LVGL + EPIC 时编不过。

**不要**把官方 `apps/graphics/lvgl/lvgl/` 工作区整棵换成 9.5。那是覆盖脏树，不是本补丁。本板源码在 vendor 那份 9.5 里。

---

## 改法

### 1. `CMakeLists.txt`：my_vendor 栈则退出

加在文件头、`if(CONFIG_GRAPHICS_LVGL)` **之前**：

```cmake
# my_vendor 占用 LVGL：由 board add_subdirectory(vendor/.../lvgl myvendor_lvgl)
# 编译。此处跳过，避免与 Vela 自带树重复定义 lvgl target。
if(CONFIG_MYVENDOR_LVGL_STACK)
  return()
endif()
```

没有这一段，`CONFIG_MYVENDOR_LVGL_STACK=y` 的配置过不了 CMake。

### 2. `CMakeLists.txt`：官方 LVGL + EPIC 头

放在 `CONFIG_LV_OPTLEVEL` 处理之后、`LV_ATTRIBUTE_MEM_ALIGN` 之前：

```cmake
  if(CONFIG_LV_USE_SIFLI_EPIC)
    get_filename_component(_sifli_drv "${NUTTX_CHIP_ABS_DIR}/../drivers" ABSOLUTE)
    get_filename_component(_sifli_ext "${NUTTX_CHIP_ABS_DIR}/../external/CMSIS/Include"
                         ABSOLUTE)
    target_include_directories(
      lvgl
      PRIVATE ${_sifli_drv}/Include ${_sifli_drv}/cmsis/Include
              ${_sifli_drv}/cmsis/sf32lb52x ${_sifli_ext})
    target_compile_definitions(lvgl PRIVATE SOC_BF0_HCPU SF32LB52X)
  endif()
```

`NUTTX_CHIP_ABS_DIR` 在本板是 `vendor/my_vendor/chips/sf32lb52`，`../drivers` 即 chip 驱动树。

### 3. `Makefile`：同样的 EPIC 头

`CONFIG_LV_USE_DRAW_VG_LITE` 之后：

```makefile
ifneq ($(CONFIG_LV_USE_SIFLI_EPIC),)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_ARCH_CHIP_CUSTOM_DIR)/../drivers/Include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_ARCH_CHIP_CUSTOM_DIR)/../drivers/cmsis/Include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_ARCH_CHIP_CUSTOM_DIR)/../drivers/cmsis/sf32lb52x
CFLAGS += -DSOC_BF0_HCPU -DSF32LB52X
endif
```

官方 Makefile **没有** `MYVENDOR_LVGL_STACK` 早退。本板不用这份 Makefile 编主固件。

---

## 改动前 / 改动后（完整 diff，取自 `git -C apps diff`）

```diff
diff --git a/graphics/lvgl/CMakeLists.txt b/graphics/lvgl/CMakeLists.txt
index 7bc18e19b..8b66cbe4e 100644
--- a/graphics/lvgl/CMakeLists.txt
+++ b/graphics/lvgl/CMakeLists.txt
@@ -18,6 +18,12 @@
 #
 # ##############################################################################
 
+# my_vendor 占用 LVGL：由 board add_subdirectory(vendor/.../lvgl myvendor_lvgl)
+# 编译。此处跳过，避免与 Vela 自带树重复定义 lvgl target。
+if(CONFIG_MYVENDOR_LVGL_STACK)
+  return()
+endif()
+
 if(CONFIG_GRAPHICS_LVGL)
 
   # ############################################################################
@@ -154,6 +160,17 @@ if(CONFIG_GRAPHICS_LVGL)
     target_compile_options(lvgl PRIVATE ${CONFIG_LV_OPTLEVEL})
   endif()
 
+  if(CONFIG_LV_USE_SIFLI_EPIC)
+    get_filename_component(_sifli_drv "${NUTTX_CHIP_ABS_DIR}/../drivers" ABSOLUTE)
+    get_filename_component(_sifli_ext "${NUTTX_CHIP_ABS_DIR}/../external/CMSIS/Include"
+                         ABSOLUTE)
+    target_include_directories(
+      lvgl
+      PRIVATE ${_sifli_drv}/Include ${_sifli_drv}/cmsis/Include
+              ${_sifli_drv}/cmsis/sf32lb52x ${_sifli_ext})
+    target_compile_definitions(lvgl PRIVATE SOC_BF0_HCPU SF32LB52X)
+  endif()
+
   # this macro should be visiable to LVGL and all libraries that use LVGL
   target_compile_options(
     lvgl PUBLIC "-DLV_ATTRIBUTE_MEM_ALIGN=aligned_data(LV_DRAW_BUF_ALIGN)")
diff --git a/graphics/lvgl/Makefile b/graphics/lvgl/Makefile
index c2f34d983..338ff02fe 100644
--- a/graphics/lvgl/Makefile
+++ b/graphics/lvgl/Makefile
@@ -62,6 +62,13 @@ ifeq ($(CONFIG_LV_USE_DRAW_VG_LITE),y)
 CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_LV_DRAW_VG_LITE_INCLUDE)
 endif
 
+ifneq ($(CONFIG_LV_USE_SIFLI_EPIC),)
+CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_ARCH_CHIP_CUSTOM_DIR)/../drivers/Include
+CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_ARCH_CHIP_CUSTOM_DIR)/../drivers/cmsis/Include
+CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_ARCH_CHIP_CUSTOM_DIR)/../drivers/cmsis/sf32lb52x
+CFLAGS += -DSOC_BF0_HCPU -DSF32LB52X
+endif
+
 ifneq ($(CONFIG_LV_ASSERT_HANDLER_INCLUDE), "")
 CFLAGS += "-DLV_ASSERT_HANDLER=ASSERT(0);"
 endif
```

- 文件级"改后"全文见 `replace/apps/graphics/lvgl/CMakeLists.txt` 与 `…/Makefile`（与工作区逐字一致）。
- 上游原文可用 `git -C apps show HEAD:<path>` 取回。

---

## 怎么打

OpenVela 根目录。

**覆盖：**

```bash
cp vendor/my_vendor/docs/pitch/replace/apps/graphics/lvgl/CMakeLists.txt \
   apps/graphics/lvgl/CMakeLists.txt
cp vendor/my_vendor/docs/pitch/replace/apps/graphics/lvgl/Makefile \
   apps/graphics/lvgl/Makefile
```

**打补丁：**

```bash
git -C apps apply \
  ../vendor/my_vendor/docs/pitch/patches/apps-lvgl-myvendor-stack.patch
```

补丁相对 `apps/` 仓库根，路径是 `graphics/lvgl/...`。

---

## 为什么必须改上游脚本

`vela_override/` 只替换已进入某个 library target 的 **同名 `.c`**。官方 `CMakeLists.txt` / `Makefile` 是 **生成那个 target 之前** 的入口，抽换层到不了。

board 的 `add_subdirectory(... myvendor_lvgl)` 只能多编一份 vendor LVGL，不能阻止 apps 再跑官方脚本。因此 `return()` 只能写在官方 `CMakeLists.txt` 里。

---

## 验证

```bash
git -C apps diff -- graphics/lvgl/CMakeLists.txt graphics/lvgl/Makefile
```

CMake 配置日志里不应再出现重复 target `lvgl`。应能看到 vendor 自己的 LVGL 被编进 `myvendor_lvgl`。

`repo sync` 之后若 CMake 报 `add_library cannot create target "lvgl"`，就是这份 `CMakeLists.txt` 被冲掉了。

---

## 不要做的

| 动作 | 原因 |
|------|------|
| 把官方 `apps/graphics/lvgl/lvgl/` 整树换成 vendor 的 9.5 | 脏工作区，官方脚本已 `return()`，编译用不到 |
| 改官方 `apps/graphics/lvgl/Kconfig` 关掉 GRAPHICS_LVGL | 本板仍可能依赖部分 LV 符号；用 `MYVENDOR_LVGL_STACK` 门控即可 |
| 把 EPIC 头写进 `nuttx` 或 chip 以外的公共树 | 头文件已在 `vendor/my_vendor/chips/sf32lb52/../drivers` |
