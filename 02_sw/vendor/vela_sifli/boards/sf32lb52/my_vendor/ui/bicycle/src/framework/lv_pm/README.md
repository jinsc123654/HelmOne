# lv_pm — 页面管理框架

bicycle C UI 的页面栈与转场动画层，基于 LVGL 9。应用侧通过 `lvgl_page_*` 注册页面并导航；本目录是框架本体。

**产品级 UI 设计、MTP/LiveMap 约束、主题与注意事项** 见 [`doc/ui_framework.md`](../../../doc/ui_framework.md)。  
开机 / 关机 splash 见 [`doc/splash.md`](../../../doc/splash.md)。

---

## 1. 设计目标

| 目标 | 说明 |
|------|------|
| **栈式导航** | `push` / `pop` 维护页面历史，支持 overlay（临时顶层页） |
| **生命周期** | 与 iOS / Android Activity 类似的 appear / disappear / close 回调 |
| **转场动画** | 可插拔 `lv_pm_anima_t`；bicycle 扩展 GPU transform 动画（SiFli EPIC） |
| **页面缓存** | `cache_enable` 时 pop 不销毁 `lv_obj` 与 `user_data`，适合 LiveMap 常驻 |
| **导航抢占** | busy 时 `lv_pm_nav_drain()` 结束未完成转场，再执行新 open/close |

不在本框架内：业务 DataProc（X-TRACK fork 遗留，未编入当前 C 主路径）。

---

## 2. 目录结构

```
lv_pm/
├── README.md                 ← 本文档
├── include/
│   ├── lv_pm_core.h          核心 API、页面描述符、路由
│   ├── lv_pm_port.h          生命周期注册、open_options、cache、定时器
│   ├── lv_pm_anima.h         经典动画（fade / slide / popup / none）
│   ├── lv_pm_gpu_anima.h     GPU transform 动画（zoom / depth / flip / rotate-slide）
│   ├── lv_pm_config.h        编译开关（bicycle 关闭内置状态栏/返回栏）
│   ├── lv_pm_disp.h          屏幕分辨率 helper（PAGE_HOR_RES / VER_RES）
│   ├── lv_pm_theme_def.h     主题颜色/字体/on_activate 结构
│   ├── lv_pm_theme.h         主题切换 API
│   ├── lv_pm_i18n_def.h      locale 结构、页面 i18n 回调
│   └── lv_pm_i18n.h          国际化 API
├── i18n/
│   ├── lv_pm_i18n_keys.h     字符串 key 枚举（X 宏扩展）
│   ├── lv_pm_i18n_strings.c  zh_CN / en 内置表
│   └── lv_pm_i18n_locales.h
├── themes/
│   ├── lv_pm_themes.c/h      内置主题注册表
│   ├── lv_pm_theme_classic.c   亮色
│   └── lv_pm_theme_outdoor.c   暗色
├── core/
│   ├── lv_pm_core.c          初始化、路由栈、open/close/jump
│   ├── lv_pm_port.c          port 层实现
│   ├── lv_pm_anima.c         动画调度（单通道，防重叠）
│   ├── lv_pm_theme.c
│   ├── lv_pm_theme_lvgl.c    struct → lv_style_t / lv_theme_t 绑定
│   ├── lv_pm_i18n.c
│   └── lv_pm_bar.c           可选顶部/返回栏（bicycle 未启用）
├── anima/
│   ├── lv_pm_no_anima.c
│   ├── lv_pm_fade_anima.c
│   ├── lv_pm_slide_anima.c
│   ├── lv_pm_popup_anima.c
│   └── lv_pm_gpu_anima.c
```

**跨模块事件**：页面/MTP 等广播走 `bicycle_runtime` 的 `bicycle_runtime_subj_evt()`（见 `src/bicycle_runtime.h`），由 `lv_pm_set_nav_notify_cb` 与 `bicycle_mtp_ui` 写入；原 `lv_pm_msg` 已移除。

**应用层对接**（框架外，供参考）：

| 路径 | 职责 |
|------|------|
| `src/lvgl_page/lvgl_page.c` | `lv_pm_init`、注册全部页面、boot/MTP 导航封装 |
| `src/lvgl_page/bicycle_page_ids.h` | 页面 ID 枚举 |
| `src/lvgl_page/*/ *_page.c` | 各页 `register` + 生命周期实现 |

---

## 3. 核心概念

### 3.1 页面 ID 与描述符

```c
typedef uint8_t lv_pm_id;

struct lv_pm_page_def {
    lv_obj_t * page;              /* 根容器，挂在 lv_scr_act() 下 */
    lv_pm_id id;
    const char * name;
    lv_group_t * group;           /* 输入焦点组 */
    lv_pm_lifecycle open_cb;      /* 见 §4 */
    lv_pm_lifecycle will_appear_cb;
    lv_pm_lifecycle dis_appear_cb;
    lv_pm_lifecycle will_disappear_cb;
    lv_pm_lifecycle dis_disappear_cb;
    lv_pm_lifecycle close_cb;
    lv_pm_open_options_t open_options;
    struct lv_pm_flag { ... } flag;
    void * msg_data;              /* open/close 传入，框架不 free */
    void * user_data;             /* 页面私有上下文，如 map_page_t * */
};
```

- 页面在 **`lv_pm_create_page(id, name)`** 时分配描述符，**不会**自动创建 UI。
- 首次 **`lv_pm_open_page_msg`** 时 `_open_page_object()` 创建 `page` lv_obj（或复用 cache 对象），再调 `open_cb`。

### 3.2 路由栈 `g_pm_history`

```
open A  →  [A]
open B  →  [A, B]     B 在顶层；A 仍 is_open，UI 可能被盖住
close   →  [A]         pop B，A 恢复
```

- 栈深上限 = `lv_pm_init(page_num)` 传入的容量（bicycle：`BICYCLE_PM_ID_COUNT`）。
- **`lv_pm_get_crr_page()`** 返回栈顶页面描述符。

### 3.3 `lv_pm_nav_busy` / 导航抢占

转场期间 `lv_pm_nav_busy()` 为 `true`（appear/disappear 段引用计数）。

**不拒绝**新的 open/close：入口调用 `lv_pm_nav_drain()`，同步结束未完成的动画段、触发对应 lifecycle 回调（含 `close_cb` / cache 隐藏 / `dis_appear`），再立即执行新导航。

| API | busy 时行为 |
|-----|-------------|
| `lv_pm_open_page_msg` | drain → push |
| `lv_pm_close_page_msg` | drain → pop |
| `lv_pm_close_to_page_msg` | drain → close_to |
| `lv_pm_jump_to_page_msg*` | drain → jump |

`lv_pm_anima_drain_all()` 对 appear/disappear 双通道各：停止 LVGL 动画 → 幂等调用完成回调 → 回收资源。

---

## 4. 生命周期回调

| 回调 | 时机 | 典型用途 |
|------|------|----------|
| `open_cb` | 首次创建或 cache 复用时，构建/绑定 UI | `map_page_create()`、`startup_ui_create()` |
| `will_appear` | 即将显示（动画开始前） | 启动 timer、刷新数据、`move_foreground` |
| `dis_appear` | 进入动画结束，页面已完全可见 | 开始轮询、注册按键 |
| `will_disappear` | 即将被盖住或 pop | pause 地图、停 timer |
| `dis_disappear` | 离开动画结束，已不可见 | 少见，深度清理前钩子 |
| `close_cb` | 页面从栈移除且非 cache | `destroy`、`lv_pm_free(user_data)` |

**推荐命名**（bicycle 惯例）：`*_on_load` → open，`*_will_appear`，`*_will_disappear`，`*_on_unload` → close。

### 4.1 `push`（open）顺序

```
lv_pm_open_page_msg(id, msg)
  ├─ 若栈非空：prev.will_disappear → prev 播放 dis_appear 动画
  ├─ _open_page_object(new) → open_cb
  ├─ new.will_appear
  ├─ new 播放 appear 动画 → dis_appear
  └─ _handle_open_target (NEW / SELF / RESET)
```

### 4.2 `pop`（close）顺序

```
lv_pm_close_page_msg(msg)
  ├─ current.will_disappear → current dis_appear 动画
  ├─ 栈顶弹出，prev.will_appear
  ├─ prev 播放 appear 动画（is_back=1）→ dis_appear
  └─ current 动画完成 → dis_disappear → close_cb → delete（或 cache 隐藏）
```

---

## 5. 导航 API

| API | 作用 | 返回值 |
|-----|------|--------|
| `lv_pm_init(n)` | 分配页面池与 history | 0 |
| `lv_pm_create_page(id, name)` | 注册页面描述符 | `lv_pm_page_t` |
| `lv_pm_open_page_msg(id, msg)` | **push** 打开 | 0 成功 |
| `lv_pm_close_page_msg(msg)` | **pop** 关闭栈顶 | 0；栈≤1 失败 |
| `lv_pm_close_to_page_msg(id, msg)` | pop 到栈中已有页面 | 0 |
| `lv_pm_jump_to_page_msg(id, msg)` | 无动画跳到栈中页面 | 0 |
| `lv_pm_delete_page(id)` | 释放 lv_obj（非 cache 关闭路径） | — |
| `lv_pm_nav_busy()` | 是否在转场中 | bool |

应用封装（`lvgl_page.h`）：

```c
lvgl_page_open_home();           /* push LiveMap */
lvgl_page_push_usb_transfer();   /* push UsbTransfer */
lvgl_page_pop_usb_transfer();    /* pop UsbTransfer */
lvgl_page_open_boot_sequence();  /* 见 §7 */
```

---

## 6. `open_options` 与动画

### 6.0 成对原则（open / close 一体）

一次 `lv_pm_set_open_options(..., &lv_pm_xxx_anima, ...)` 注册的是 **一对** 动画：

| 相位 | API | 回调 | 时机 |
|------|-----|------|------|
| **ENTER** | `lv_pm_anima_play(page, LV_PM_ANIMA_ENTER, ...)` | `lv_pm_appear` | 页面进入 / 从下层恢复（`is_back=1`） |
| **EXIT** | `lv_pm_anima_play(page, LV_PM_ANIMA_EXIT, ...)` | `lv_pm_dis_appear` | 页面被盖住 / pop 关闭 |

**规则：**

- 有 ENTER 就必须有 EXIT，二者来自 **同一** `lv_pm_anima_t`，逻辑互为逆过程。
- push：`prev` EXIT + `new` ENTER；pop：`current` EXIT + `prev` ENTER（`is_back=1` 时 appear 走恢复分支）。
- 动画类型始终读 **当前页** 自己的 `open_options.lv_pm_anima_cb`；`route_opts` 参数仅传给完成回调（如 push 时被盖页的 `target` 路由）。
- 若 `appear` / `dis_appear` 任一缺失，框架回退 `lv_pm_no_anima`。

`lv_pm_anima_open_cb` / `lv_pm_anima_back_cb` 为 ENTER / EXIT 的薄封装，新代码推荐直接用 `lv_pm_anima_play`。

```c
typedef struct {
    const void * lv_pm_anima_cb;   /* const lv_pm_anima_t * */
    lv_pm_target target;           /* NEW / SELF / RESET */
    uint32_t direction;            /* 动画方向，语义因 anima 而异 */
    uint32_t time;                 /* 毫秒 */
} lv_pm_open_options_t;
```

注册：

```c
lv_pm_set_open_options(page, &lv_pm_gpu_zoom_anima,
                       LV_PM_TARGET_NEW, LV_PM_GPU_DIR_CENTER, 350);
```

### 6.1 `lv_pm_target` 路由语义

| 值 | open 时 `_handle_open_target` | 被盖住页消失动画结束时 |
|----|--------------------------------|------------------------|
| `LV_PM_TARGET_NEW` | 默认 **push**，`history_count++` | 默认：暂停 timer，**不删页**；`cache_enable` 页仅隐藏 |
| `LV_PM_TARGET_SELF` | 栈已 `++`，栈顶改为新页 ID | `close_cb` + **`delete_deep_page`**（从页面池移除） |
| `LV_PM_TARGET_RESET` | **同步**对栈内每一页 `close_cb` + **`delete_deep_page`**，再 `history = [新页]` | 被盖页走 `delete_page` |

与 C++ PageManager 近似对应：`push` ≈ **NEW**；整栈 **replace** ≈ 新页带 **RESET** 去 `open`；只换栈顶 ≈ **SELF**（被盖页 deep 删除）。

**注意：** `RESET` **不**尊重 `cache_enable`——`_handle_target_reset` 会 `delete_deep_page`，LiveMap 画布/tile cache 会被拆掉。**禁止**对常驻 LiveMap 使用 RESET/replace。

### 6.2 动画由谁播放？

**规则（实现于 `lv_pm_anima.c::lv_pm_anima_play`）：**

| 场景 | ENTER (appear) | EXIT (dis_appear) |
|------|----------------|-------------------|
| push B | **B** 的 `lv_pm_anima_t` | **A** 的 `lv_pm_anima_t` |
| pop B | **A** 的 `lv_pm_anima_t`（`is_back=1`） | **B** 的 `lv_pm_anima_t` |

因此：**下层页被盖住时**，播的是**下层页自己**注册的 EXIT；**上层页**播 ENTER。  
MTP 示例：LiveMap 配 `gpu_depth`，UsbTransfer 配 `gpu_zoom` → 进 MTP 时地图 depth EXIT + 传输页 zoom ENTER；退出时 zoom EXIT + depth ENTER（恢复）。

### 6.3 内置动画

| 符号 | 效果 |
|------|------|
| `lv_pm_no_anima` | 无动画（MTP 调试、boot 预挂载 LiveMap 壳） |
| `lv_pm_fade_anima` | 透明度 |
| `lv_pm_slide_anima` | 位移；direction 0–3 表上下左右 |
| `lv_pm_popup_anima` | 半屏弹入 |
| `lv_pm_gpu_zoom_anima` | 缩放 + 淡入淡出（EPIC layer） |
| `lv_pm_gpu_depth_anima` | 景深 parallax 缩放 |
| `lv_pm_gpu_flip_anima` | scale_x 翻转 |
| `lv_pm_gpu_rotate_slide_anima` | 侧滑 + 微旋转 |

GPU 方向常量见 `lv_pm_gpu_anima.h`（`LV_PM_GPU_DIR_*`）。

### 6.4 Boot / RESET / replace 规范（bicycle）

**正常开机（当前实现，`lvgl_page_open_boot_sequence`）：**

```
push LiveMap   TARGET_NEW + bicycle_page_anima_apply_silent()（无动画空壳，cache）
push Startup   TARGET_NEW
Startup 结束   lv_pm_close_page_msg()（pop Startup）
→ 同一 LiveMap 实例 resume，vmap 在 Logo 后加载
```

- LiveMap **必须先**进栈；Startup 只做临时顶层。
- 结束用 **pop**，不用 `open LiveMap + RESET`，不用 replace。
- `bicycle_page_anima.c` 默认三页均为 **TARGET_NEW**；boot 特例只在 `lvgl_page.c` 里对 LiveMap 调 `apply_silent()`，**不改 target**。

**何时才考虑 RESET / replace：**

| 场景 | 用 RESET？ | 说明 |
|------|-----------|------|
| 正常 Splash → 地图 | **否** | 会破坏应常驻的 LiveMap |
| MTP（LiveMap 在栈底） | **否** | `push/pop UsbTransfer` |
| 栈里仅 Startup、尚无 LiveMap（极早插 USB 等） | 优先 **push 静默 LiveMap** | 勿对已 cache 的 LiveMap 做 RESET |
| 错误页 / 单页冷启动（目标页不可 cache） | **可以** | 整栈清空且栈内无 cache 常驻页 |

**若将来单次 open 需要 replace 整栈：** 不要写进 `bicycle_page_anima.c` 默认表；在 `open` 前临时设 `open_options.target = LV_PM_TARGET_RESET`，并确认目标页 `cache_enable = false`、栈内无 LiveMap。

**一句话：** Boot 与 MTP 均 **LiveMap 在栈底 + 上层 push/pop（NEW）**；RESET/replace 仅用于无 cache 常驻页的整栈清空。

### 6.5 `msg_data` 所有权与防野指针

| 规则 | 说明 |
|------|------|
| **框架不释放** | `_pm_msg_replace` 仅赋值，无 `lv_pm_free(msg_data)` |
| **生命周期** | 自本次 `open/close/jump` 写入起，至该页 `close_cb` 返回止 |
| **自动失效** | `close_cb` 之后框架调用 `_pm_msg_clear`；`lv_pm_delete_*` 再次清空 |
| **读取方式** | 生命周期回调内：`lv_pm_page_get_msg(pm_page)`，勿缓存到静态/定时器 |
| **无参数** | 使用 `LV_PM_MSG_NONE`（即 NULL） |

```c
static void my_will_appear(void * pm_page)
{
    my_param_t * p = (my_param_t *)lv_pm_page_get_msg(pm_page);
    if (p == NULL) {
        return;
    }
    /* 仅在本回调同步使用 p；close 后指针失效 */
}
```

bicycle 当前各页均传 `LV_PM_MSG_NONE`，未使用导航载荷。

---

## 7. 页面缓存 `cache_enable`

```c
lv_pm_set_cache_enable(page, true);
```

- **pop 时**：`close_cb` 仍调用，但若 `cache_enable` 且 `close_cb` 内判断 flag，则**保留** `page` lv_obj 与 `user_data`，仅 `HIDDEN`。
- **再次 open**：`_open_page_object` 复用已有 `page`，`open_cb` 里可检测 `user_data != NULL` 跳过重建。

**bicycle 约定**

| 页面 | cache | 说明 |
|------|-------|------|
| LiveMap | **true** | 主页常驻；MTP 只 pause，不 destroy |
| Startup | false | 一次性 splash |
| UsbTransfer | false | 临时 overlay，pop 销毁 |

LiveMap `close_cb` 示例模式：

```c
if (page->flag.cache_enable) return;
map_page_destroy(...);
```

---

## 8. bicycle 页面一览（当前约定）

ID 定义：`src/lvgl_page/bicycle_page_ids.h`  
**转场 / cache 配置表**：`src/lvgl_page/bicycle_page_anima.c`（各 `*_page_register` 调用 `bicycle_page_anima_apply()`）

| ID | 名称 | 文件 | 动画 | cache | 备注 |
|----|------|------|------|-------|------|
| 0 | LiveMap | `live_map/map_page.c` | `gpu_depth` 350ms | yes | 主页；pm 下拉状态栏 + boot 静默壳 |
| 1 | UsbTransfer | `usb_transfer/usb_transfer_page.c` | `gpu_zoom` 350ms | no | MTP overlay |
| 2 | Startup | `startup/startup_page.c` | `gpu_zoom` 400ms | no | 开机 splash |

### 8.1 开机流程（已确定）

```
lvgl_page_open_boot_sequence()
  1. map_page_set_boot_deferred(true)
  2. push LiveMap（no_anima，空壳，黑屏，不创建 vmap）
  3. push Startup（与 2SFBL 同一套 logo）
  4. Startup：黑→白后事项滚入（骑行记录 / 系统字体 / 地图加载）
  5. fade → pop Startup
  6. 同一 LiveMap 实例 resume；默认 Helm 待机，地图区仍隐藏
```

事项行、关机对称动画、首帧画布约定见
[`doc/splash.md`](../../../doc/splash.md)。
字体在事项里分块加载；vmap 在「地图加载」里创建，splash 盖着时只 warmup。

### 8.2 MTP 流程（已确定）

```
LiveMap (cached, 运行中)
  → push UsbTransfer     LiveMap: depth dis_appear + pause
  → pop UsbTransfer      UsbTransfer: zoom dis_appear；LiveMap: depth appear + resume
```

实现入口：`src/bicycle_mtp_ui.c` + `lvgl_page_push/pop_usb_transfer()`。  
框架会在 busy 时自动 **drain** 未完成转场，业务层可直接调用 push/pop。

---

## 9. 新增页面 checklist

1. 在 `bicycle_page_ids.h` 增加 ID，`BICYCLE_PM_ID_COUNT++`。
2. 新建 `src/lvgl_page/<name>/<name>_page.c`，实现 `*_page_register()`。
3. 在 `lvgl_page_init()` 里调用 register。
4. `lv_pm_create_page` + 绑定六个生命周期（可空）+ `bicycle_page_anima_apply(page, id)`。
5. 在 `open_cb` 里创建 UI；私有状态放 `page->user_data`。
6. CMakeLists 增加源文件与 include 路径。
7. 更新本文档 §8 表格。

---

## 10. 配置项 `lv_pm_config.h`

| 宏 | bicycle 值 | 含义 |
|----|------------|------|
| `LV_PM_USE_STA_BAR` | 1 | 框架下拉状态栏（LiveMap；收起条恒黑，展开随主题） |
| `LV_PM_USE_BACK_BAR` | 0 | 框架内置返回栏（物理键返回） |
| `LV_PM_USE_ANMI_STA` | 0 | 状态栏显隐 Y 轴动画 |
| `LV_PM_USE_ANMI_BACK` | 0 | 返回栏显隐 Y 轴动画 |
| `LV_PM_MAX_PAGES` | 8 | 页面池上限 |

### 10.1 状态栏 / 返回栏（`lv_pm_bar.h`）

| API | 作用 |
|-----|------|
| `lv_pm_bars_init(parent)` | `lv_pm_init` 内调用，创建 overlay 栏 |
| `lv_pm_bar_apply_for_page(page)` | 页面 appear 完成后按 `top_bar_en` / `back_bar_en` 显隐 |
| `lv_pm_set_top_bar(page, en)` | 注册页是否显示下拉状态栏 |
| `lv_pm_set_back_bar(page, en)` | 注册页是否显示上拉返回栏 |
| `lv_pm_status_bar_cont()` | 状态栏内容容器（应用可放 label/icon） |
| `lv_pm_back_bar_handle()` | 返回条手柄 |

### 10.3 国际化（`lv_pm_i18n.h` + `i18n/`）

与主题对称：内置 locale 注册表 + 页面 `i18n_changed_cb`。

| API | 作用 |
|-----|------|
| `lv_pm_tr(key)` / `lv_pm_i18n_get_text(key)` | 取当前 locale 字符串 |
| `lv_pm_label_set_tr(label, key)` | 设置 label 文案 |
| `lv_pm_i18n_set(id)` / `set_by_name` / `set_next` | 切换语言 → `lv_pm_i18n_apply_all()` |
| `lv_pm_set_i18n_changed(page, cb, ud)` | 页面刷新 hook |

新增字符串：在 `i18n/lv_pm_i18n_keys.h` 的 `LV_PM_I18N_KEY_LIST` 增加 key，并在 `lv_pm_i18n_strings.c` 各 locale 表填翻译。

NSH：`bicycle_nsh lang en`（经 ctl 在 LVGL 线程切换）。

### 10.4 页面主题（`lv_pm_theme.h` + `themes/`）

内置主题在 `framework/lv_pm/themes/`，**每个 `.c` 文件一个主题**，定义：

- `colors`：页面背景、文字、accent、状态栏/面板、toast、聚焦色等
- `fonts`：页面 shell 默认字体（可选）
- `on_activate`：切换时回调（bicycle：`bicycle_pm_theme_on_activate` → `vmap_style_set_id`）

`lv_pm_theme_set` 会同时：

1. 从 struct 更新共享 `lv_style_t`，`apply_cb` 在 **对象创建时** `add_style` 叠加（同 EPD `lv_port_theme.c`，**不** `remove_style_all`）
2. 切换主题时只改 style 内容 + `lv_obj_report_style_change`，已有控件自动刷新
3. `on_activate` + 各页 `theme_changed_cb`（LiveMap 等特殊 chrome / vmap）

注册表：`lv_pm_themes.c` 的 `lv_pm_builtin_themes[]`。

| API | 作用 |
|-----|------|
| `lv_pm_theme_set(id)` / `set_by_name` / `set_next` | 切换主题 → `on_activate` → shell 换色 → 各页 `theme_changed_cb` |
| `lv_pm_theme_current()` | 当前预设（颜色/字体） |
| `lv_pm_set_theme_changed(page, cb, ud)` | 页面级回调：LiveMap vmap 重绘等 |
| `lv_pm_theme_apply_page_shell(page)` | 仅应用 pm 页 shell（bg / font / focus） |

新增主题：复制 `lv_pm_theme_classic.c`，改 `name`/`colors`，加入 `lv_pm_builtin_themes[]`。

bicycle：`bicycle_nsh style nav` → ctl → `lv_pm_theme_set_by_name`；LiveMap 注册 `map_page_on_pm_theme` 刷新 chrome + `vmap_view_render`。

分辨率：`PAGE_HOR_RES` / `PAGE_VER_RES` 由 bicycle CMake 注入，`lv_pm_disp.h` 读取。

---

## 11. 已知限制与注意事项

1. **单动画通道**：全局仅一组 `appear_anima` / `dis_appear_anima`；重叠时会强制结束上一段（见 log WARN）。
2. **`msg_data`**（§6.5）：框架**只存指针、绝不 free**；`close_cb` 执行后立即 `_pm_msg_clear`，页面删除时再次清空。生命周期内用 `lv_pm_page_get_msg(pm_page)` 读取；勿传栈地址；无参数传 `LV_PM_MSG_NONE`。
3. **栈顶才能 pop**：`close_page_msg` 要求 `history_count > 1`。
4. **重复 open**：同一 ID 在 `is_open==1` 时再次 open 会失败（cache 页需先 pop 再 open，或依赖复用逻辑）。
5. **GPU 动画**：依赖 `LV_USE_SIFLI_EPIC` + transform layer；异常时可回退 `lv_pm_no_anima`。
6. **LiveMap ↔ UsbTransfer**：已启用 GPU 动画；若遇 LVGL 崩溃，优先怀疑 transform 与地图大 canvas 并发，可临时改 `no_anima` 定位。

---

## 12. 待讨论 / 可选演进

- [x] ~~`lv_pm_msg` 是否与 `bicycle_runtime` subject 合并~~ → 已合并至 `bicycle_runtime_subj_evt()`  
- [x] ~~C++ `PageManager`~~ → 已删除；页面栈统一 `lv_pm`（`src/lvgl_page/`）  
- [x] ~~`LV_PM_TARGET_RESET` 与 `replace` 语义在 boot 场景的规范用法~~ → 见 §6.4  
- [x] ~~页面转场配置表集中化~~ → `src/lvgl_page/bicycle_page_anima.c`  

---

## 13. 快速参考

```c
/* 注册 */
lv_pm_init(BICYCLE_PM_ID_COUNT);
lv_pm_page_t p = lv_pm_create_page(BICYCLE_PM_ID_MAP, "LiveMap");
lv_pm_set_open(p, map_on_load);
lv_pm_set_will_appear(p, map_will_appear);
lv_pm_set_will_disappear(p, map_will_disappear);
lv_pm_set_close(p, map_on_unload);
lv_pm_set_open_options(p, &lv_pm_gpu_depth_anima, LV_PM_TARGET_NEW, 0, 350);
lv_pm_set_cache_enable(p, true);
/* 或统一：bicycle_page_anima_apply(p, BICYCLE_PM_ID_MAP); */

/* 导航（busy 时框架会先 drain 未完成转场） */
lv_pm_open_page_msg(BICYCLE_PM_ID_USB_TRANSFER, NULL);

/* 事件总线（bicycle_runtime，替代 lv_pm_msg） */
bicycle_runtime_evt_observe(BICYCLE_SUBJ_EVT_PAGE, on_page_evt, obj, NULL);
bicycle_runtime_evt_observe(BICYCLE_SUBJ_EVT_MTP, on_mtp_evt, obj, NULL);
```

---

*文档版本：与 bicycle C UI（Startup / LiveMap / UsbTransfer + GPU anima）同步。修改框架行为时请更新本文 §6–§8。*
