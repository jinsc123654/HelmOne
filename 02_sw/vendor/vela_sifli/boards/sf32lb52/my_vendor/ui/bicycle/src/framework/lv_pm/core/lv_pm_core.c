/**
 * @file lv_pm_core_refactored.c
 * @brief LVGL 页面管理器核心 - 重构版本
 * @author Refactored
 * @date 2024
 *
 * 重构目标：
 *   ✓ 提高代码可维护性和可读性
 *   ✓ 优化模块结构，降低耦合度
 *   ✓ 统一错误处理和日志记录
 *   ✓ 明确职责划分（初始化、管理、查询、生命周期）
 *   ✓ 保持所有公开API不变
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "../include/lv_pm_core.h"
#include "../include/lv_pm_anima.h"
#include "../include/lv_pm_theme.h"
#include "../include/lv_pm_bar.h"
#include "../include/lv_pm_overlay.h"

#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#endif

/* ======================== 私有全局变量 ======================== */

/** 页面存储 - 根据ID索引 */
static lv_pm_page_t* g_pm_pages = NULL;
static int g_pm_pages_capacity = 0;

/** 页面路由栈 - 页面打开历史 */
static lv_pm_id* g_pm_history = NULL;
static int g_pm_history_capacity = 0;
static int g_pm_history_count = 0;

/** 屏幕引用 */
static lv_obj_t* g_screen = NULL;

/** @brief 进行中的 appear/disappear 动画（pop 可能并行两个）。 */
static uint8_t g_nav_anim_ref = 0;

static lv_pm_nav_notify_cb g_nav_notify_cb;
static void * g_nav_notify_ud;

static void _nav_notify(lv_pm_page_t page, lv_pm_nav_evt_t evt)
{
    if (g_nav_notify_cb != NULL && page != NULL) {
        g_nav_notify_cb(page, evt, g_nav_notify_ud);
    }
}

/**
 * @brief lv_pm set group default。
 */
extern void lv_pm_set_group_default(lv_group_t* group);

/* msg_data: store caller pointer only — never free (supports (void*)1 tags). */
static void _pm_msg_replace(lv_pm_page_t pm_page, void * msg_data)
{
    if (pm_page == NULL) {
        return;
    }
    pm_page->msg_data = msg_data;
}

static void _pm_msg_clear(lv_pm_page_t pm_page)
{
    if (pm_page == NULL) {
        return;
    }
    pm_page->msg_data = NULL;
}

/**
 * @brief 从页面数组中获取指定ID的页面（不含日志）
 * @param id 页面ID
 * @return 页面指针或NULL
 */
static lv_pm_page_t _get_page_by_id(lv_pm_id id)
{
    if (id >= g_pm_pages_capacity) {
        return NULL;
    }
    return g_pm_pages[id];
}

/**
 * @brief 检查ID是否有效
 * @param id 页面ID
 * @return true 有效，false 无效
 */
static bool _is_valid_id(lv_pm_id id)
{
    return (id >= 0 && id < g_pm_pages_capacity);
}

/**
 * @brief 在路由栈中查找页面位置
 * @param target_id 要查找的页面ID
 * @return 页面在栈中的索引，或 -1 如果未找到
 */
static int _find_page_in_history(lv_pm_id target_id)
{
    for (int i = 0; i < g_pm_history_count; i++) {
        if (g_pm_history[i] == target_id) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 获取路由栈中的当前页面ID
 * @return 当前页面ID，或 -1 如果栈为空
 */
static lv_pm_id _get_current_page_id(void)
{
    if (g_pm_history_count <= 0) {
        return -1;
    }
    return g_pm_history[g_pm_history_count - 1];
}

/**
 * @brief 获取路由栈中的前一页面ID
 * @return 前一页面ID，或 -1 如果不存在
 */
static lv_pm_id _get_previous_page_id(void)
{
    if (g_pm_history_count <= 1) {
        return -1;
    }
    return g_pm_history[g_pm_history_count - 2];
}


/* ======================== 屏幕初始化 ======================== */

/**
 * @brief 初始化屏幕配置
 */
static void _init_screen_config(void)
{
    lv_obj_set_style_outline_width(g_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(lv_layer_top(), LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(lv_layer_top(), LV_DIR_VER);
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * @brief 创建并初始化顶部栏
 */
static void _init_top_bars(void)
{
#if LV_PM_USE_STA_BAR || LV_PM_USE_BACK_BAR
    lv_pm_bars_init(lv_layer_top());
#endif
#if LV_PM_USE_OVERLAY
    lv_pm_overlay_init(lv_layer_top());
#endif
}


/* ======================== 页面组管理 ======================== */

/**
 * @brief 创建或重用设备组
 * @param existing_group 现有分组指针，为NULL则创建新的
 * @return 分组指针
 */
static lv_group_t* _create_or_reuse_group(lv_group_t* existing_group)
{
    if (existing_group != NULL) {
        lv_group_remove_all_objs(existing_group);
        return existing_group;
    }
    return lv_group_create();
}

/**
 * @brief 设置输入设备组（支持模拟器和真实设备）- 私有版本
 * @param group 要设置的分组
 */
static void _set_input_group(lv_group_t* group)
{
    lv_pm_set_group_default(group);
}


/* ======================== 动画回调 ======================== */

/**
 * @brief 页面打开动画完成回调
 * @param pm_page 页面指针
 * @param open_options 打开选项（可能为NULL）
 */
static void _on_open_animation_complete(lv_pm_page_t pm_page, lv_pm_open_options_t* open_options)
{
    if (pm_page->dis_appear_cb) {
        pm_page->dis_appear_cb(pm_page);
    }
    lv_obj_add_flag(pm_page->page, LV_OBJ_FLAG_CLICKABLE);

    if (lv_pm_theme_get() != LV_PM_THEME_ID_INVALID) {
        lv_pm_theme_apply(pm_page);
    }
#if LV_PM_USE_STA_BAR || LV_PM_USE_BACK_BAR
    lv_pm_bar_apply_for_page(pm_page);
#endif
    _nav_notify(pm_page, LV_PM_NAV_EVT_APPEAR_DONE);
}

/**
 * @brief 页面返回打开动画完成回调
 * @param pm_page 页面指针
 * @param open_options 打开选项（可能为NULL）
 */
static void _on_back_open_animation_complete(lv_pm_page_t pm_page, lv_pm_open_options_t* open_options)
{
    if (pm_page->dis_appear_cb) {
        pm_page->dis_appear_cb(pm_page);
    }
    lv_obj_add_flag(pm_page->page, LV_OBJ_FLAG_CLICKABLE);

    if (lv_pm_theme_get() != LV_PM_THEME_ID_INVALID) {
        lv_pm_theme_apply(pm_page);
    }
#if LV_PM_USE_STA_BAR || LV_PM_USE_BACK_BAR
    lv_pm_bar_apply_for_page(pm_page);
#endif
    _nav_notify(pm_page, LV_PM_NAV_EVT_APPEAR_DONE);
}

/**
 * @brief 页面关闭动画完成回调
 * @param pm_page 页面指针
 * @param open_options 打开选项
 */
static void _on_close_animation_complete(lv_pm_page_t pm_page, lv_pm_open_options_t* open_options)
{
    if (pm_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 关闭回调: 空页面指针");
        return;
    }

    if (pm_page->dis_disappear_cb) {
        pm_page->dis_disappear_cb(pm_page);
    }

    if (open_options == NULL) {
        _nav_notify(pm_page, LV_PM_NAV_EVT_DISAPPEAR_DONE);
        return;
    }

    _nav_notify(pm_page, LV_PM_NAV_EVT_DISAPPEAR_DONE);

    switch (open_options->target) {
    case LV_PM_TARGET_SELF:
        if (pm_page->close_cb) {
            pm_page->close_cb(pm_page);
        }
        _pm_msg_clear(pm_page);
        lv_pm_delete_deep_page(pm_page->id);
        break;

    case LV_PM_TARGET_RESET:
        if (pm_page->close_cb) {
            pm_page->close_cb(pm_page);
        }
        _pm_msg_clear(pm_page);
        lv_pm_delete_page(pm_page->id);
        break;

    default:
        if (pm_page->page_timer != NULL && pm_page->flag.timer_backend_run == 0) {
            lv_timer_pause(pm_page->page_timer);
        }
        break;
    }
}

/**
 * @brief 页面返回关闭动画完成回调
 * @param pm_page 页面指针
 * @param open_options 打开选项（未使用）
 */
static void _on_back_close_animation_complete(lv_pm_page_t pm_page, lv_pm_open_options_t* open_options)
{
    if (pm_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 返回关闭回调: 空页面指针");
        return;
    }

    if (pm_page->page != NULL) {
        lv_obj_add_flag(pm_page->page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pm_page->page, LV_OBJ_FLAG_CLICKABLE);
    }

    if (pm_page->dis_disappear_cb) {
        pm_page->dis_disappear_cb(pm_page);
    }
    if (pm_page->close_cb) {
        pm_page->close_cb(pm_page);
    }

    lv_pm_delete_page(pm_page->id);
}


/* ======================== 改进的动画完成回调 ======================== */

/**
 * @brief 普通关闭的动画完成回调
 *
 * 执行顺序：
 *   1. dis_disappear_cb - 页面隐藏动画完成，页面已不可见
 *   2. 暂停定时器
 *   3. close_cb - 清理资源
 *   4. 删除页面
 *
 * 对应的打开流程：
 *   lv_pm_open_page_msg() →
 *     _open_page_object()
 *     → open_cb
 *     → will_appear_cb
 *     → 播放打开动画
 *     → _on_open_animation_complete()
 *       → dis_appear_cb
 */
static void _on_normal_close_animation_complete(
    lv_pm_page_t pm_page,
    lv_pm_open_options_t* open_options)
{
    if (pm_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 关闭回调: 空页面指针");
        return;
    }

    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_ANIMA] 关闭动画完成: ID=%d, Name=%s",
           pm_page->id, pm_page->name);

    // Step 1: dis_disappear - 页面已隐藏，动画完成
    if (pm_page->dis_disappear_cb) {
        LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_ANIMA] 执行 dis_disappear_cb: ID=%d",
               pm_page->id);
        pm_page->dis_disappear_cb(pm_page);
    }

    // Step 2: 暂停定时器（如果不在后台运行）
    if (pm_page->page_timer != NULL && pm_page->flag.timer_backend_run == 0) {
        lv_timer_pause(pm_page->page_timer);
        LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_ANIMA] 定时器已暂停: ID=%d", pm_page->id);
    }

    // Step 3: close - 清理资源
    if (pm_page->close_cb) {
        LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_ANIMA] 执行 close_cb: ID=%d", pm_page->id);
        pm_page->close_cb(pm_page);
    }

    _pm_msg_clear(pm_page);
    _nav_notify(pm_page, LV_PM_NAV_EVT_DISAPPEAR_DONE);

    // Step 4: 删除页面对象
    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_ANIMA] 删除页面: ID=%d", pm_page->id);
    lv_pm_delete_page(pm_page->id);
}

/**
 * @brief 批量关闭的动画完成回调
 *
 * 用于 lv_pm_close_to_page_msg() 的中间页面关闭
 */
static void _on_batch_close_animation_complete(
    lv_pm_page_t pm_page,
    lv_pm_open_options_t* open_options)
{
    if (pm_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] [BATCH_CLOSE] 空页面指针");
        return;
    }

    LV_LOG_INFO("[PAGE_MANAGER] [BATCH_CLOSE] 关闭动画完成: ID=%d, Name=%s",
           pm_page->id, pm_page->name);

    // Step 1: dis_disappear
    if (pm_page->dis_disappear_cb) {
        pm_page->dis_disappear_cb(pm_page);
    }

    // Step 2: 暂停定时器
    if (pm_page->page_timer != NULL && pm_page->flag.timer_backend_run == 0) {
        lv_timer_pause(pm_page->page_timer);
    }

    // Step 3: close
    if (pm_page->close_cb) {
        pm_page->close_cb(pm_page);
    }

    _pm_msg_clear(pm_page);

    // Step 4: 删除页面
    lv_pm_delete_page(pm_page->id);

    LV_LOG_INFO("[PAGE_MANAGER] [BATCH_CLOSE] 页面关闭完成: ID=%d", pm_page->id);
}

/**
 * @brief 无动画关闭的安全实现
 *
 * 使用场景：
 *   - 需要快速返回，不需要关闭动画
 *   - 某些页面不支持动画
 *   - 用户按下返回键快速返回
 *
 * 保证的安全顺序（与打开过程对称）：
 *   1. will_disappear_cb - 页面即将隐藏
 *   2. 隐藏 UI（无动画）
 *   3. 暂停定时器
 *   4. dis_disappear_cb - 页面已隐藏
 *   5. close_cb - 清理资源
 *   6. 删除页面对象
 *
 * ✓ 避免直接调用回调的危险
 * ✓ 保证执行顺序的正确性
 * ✓ 即使没有动画也是安全的
 */
static void _close_page_without_animation(lv_pm_page_t page)
{
    if (page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] [NO_ANIMA] 尝试关闭空页面");
        return;
    }

    LV_LOG_INFO("[PAGE_MANAGER] [NO_ANIMA] 开始关闭页面（无动画）: ID=%d, Name=%s",
           page->id, page->name);

    page->flag.is_back = 1;

    // Step 1: will_disappear - 在隐藏前通知
    if (page->will_disappear_cb) {
        LV_LOG_INFO("[PAGE_MANAGER] [NO_ANIMA] 执行 will_disappear_cb: ID=%d",
               page->id);
        page->will_disappear_cb(page);
    }

    // Step 2: 隐藏 UI（无动画）
    if (page->page != NULL) {
        lv_obj_add_flag(page->page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(page->page, LV_OBJ_FLAG_CLICKABLE);
        LV_LOG_INFO("[PAGE_MANAGER] [NO_ANIMA] UI 已隐藏: ID=%d", page->id);
    }

    // Step 3: 暂停定时器
    if (page->page_timer != NULL && page->flag.timer_backend_run == 0) {
        lv_timer_pause(page->page_timer);
        LV_LOG_INFO("[PAGE_MANAGER] [NO_ANIMA] 定时器已暂停: ID=%d", page->id);
    }

    // Step 4: dis_disappear - 页面已隐藏（即使没有动画也视为完成）
    if (page->dis_disappear_cb) {
        LV_LOG_INFO("[PAGE_MANAGER] [NO_ANIMA] 执行 dis_disappear_cb: ID=%d",
               page->id);
        page->dis_disappear_cb(page);
    }

    // Step 5: close - 清理资源（最关键的步骤，必须在删除前调用）
    if (page->close_cb) {
        LV_LOG_INFO("[PAGE_MANAGER] [NO_ANIMA] 执行 close_cb: ID=%d", page->id);
        page->close_cb(page);
    }

    _pm_msg_clear(page);

    LV_LOG_INFO("[PAGE_MANAGER] [NO_ANIMA] 页面关闭完成（无动画）: ID=%d", page->id);
}



/* ======================== 页面打开目标处理 ======================== */

/**
 * @brief 处理 LV_PM_TARGET_SELF 模式
 * @param open_page 要打开的页面
 */
static void _handle_target_self(lv_pm_page_t open_page)
{
    if (g_pm_history_count == 0) {
        g_pm_history_count++;
    } else {
        g_pm_history[g_pm_history_count - 1] = open_page->id;
    }
    LV_LOG_INFO("[OPEN_TARGET] SELF 模式: 路由表大小 = %d", g_pm_history_count);
}

/**
 * @brief 处理 LV_PM_TARGET_RESET 模式
 * @param open_page 要打开的页面
 */
static void _handle_target_reset(lv_pm_page_t open_page)
{
    LV_LOG_INFO("[OPEN_TARGET] RESET 模式开始");
    LV_LOG_INFO("[OPEN_TARGET] 当前路由表大小: %d", g_pm_history_count);

    int pages_to_delete = g_pm_history_count;

    /* 逆序删除所有页面 */
    for (int i = pages_to_delete - 1; i >= 0; i--) {
        lv_pm_id delete_id = g_pm_history[i];
        lv_pm_page_t delete_page = _get_page_by_id(delete_id);

        if (delete_page != NULL) {
            LV_LOG_INFO("[OPEN_TARGET] 删除页面: ID=%d, Name=%s", delete_page->id, delete_page->name);

            if (delete_page->dis_disappear_cb) {
                delete_page->dis_disappear_cb(delete_page);
            }
            if (delete_page->close_cb) {
                delete_page->close_cb(delete_page);
            }

            _pm_msg_clear(delete_page);
            lv_pm_delete_deep_page(delete_id);
        }

        g_pm_history_count--;
        g_pm_history[g_pm_history_count] = 0;
    }

    /* 添加新页面 */
    g_pm_history[0] = open_page->id;
    g_pm_history_count = 1;

    LV_LOG_INFO("[OPEN_TARGET] RESET 完成: ID=%d, Name=%s, 路由表大小=%d",
           open_page->id, open_page->name, g_pm_history_count);
}

/**
 * @brief 处理 LV_PM_TARGET_NEW 模式
 * @param open_page 要打开的页面
 */
static void _handle_target_new(lv_pm_page_t open_page)
{
    g_pm_history_count++;
    LV_LOG_INFO("[OPEN_TARGET] NEW 模式: 路由表大小 = %d", g_pm_history_count);
}

/**
 * @brief 根据打开模式更新路由表
 * @param open_page 要打开的页面
 */
static void _handle_open_target(lv_pm_page_t open_page)
{
    switch (open_page->open_options.target) {
    case LV_PM_TARGET_SELF:
        _handle_target_self(open_page);
        break;
    case LV_PM_TARGET_RESET:
        _handle_target_reset(open_page);
        break;
    case LV_PM_TARGET_NEW:
    default:
        _handle_target_new(open_page);
        break;
    }
}


/* ======================== 页面UI初始化 ======================== */

/**
 * @brief 配置页面LVGL对象
 * @param page 页面对象
 * @param pm_page 页面管理结构
 */
static void _configure_page_object(lv_obj_t* page, lv_pm_page_t pm_page)
{
    lv_obj_t* scr = lv_obj_get_screen(page);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(page, lv_obj_get_width(scr), lv_obj_get_height(scr));
    lv_obj_align(page, LV_ALIGN_TOP_MID, 0, 0);
}

/**
 * @brief 配置屏幕滚动属性
 */
static void _configure_screen_scrolling(void)
{
    lv_obj_clear_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_0, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
}

/**
 * @brief 配置顶部栏
 * @param pm_page 页面指针
 */
static void _configure_top_bars(lv_pm_page_t pm_page)
{
#if LV_PM_USE_BACK_BAR == 1
    if (pm_page->flag.back_bar_en == 1) {
        lv_obj_t * back = lv_pm_back_bar_main();

        if (back != NULL) {
            lv_group_add_obj(pm_page->group, back);
        }
    }
#endif

#if LV_PM_USE_STA_BAR == 1
    if (pm_page->flag.top_bar_en == 1) {
        lv_pm_page_layout_below_bars(pm_page);
    }
#endif
}

/**
 * @brief 配置页面焦点
 * @param pm_page 页面指针
 */
static void _configure_page_focus(lv_pm_page_t pm_page)
{
    if (pm_page->focus_obj) {
        lv_group_focus_obj(pm_page->focus_obj);
        if (pm_page->focus_obj == pm_page->page) {
            lv_group_set_editing(pm_page->group, true);
        }
    } else {
        lv_obj_t* first_child = lv_obj_get_child(pm_page->page, 0);
        if (first_child != NULL) {
            lv_group_focus_obj(first_child);
        }
    }
}

/**
 * @brief 打开页面的LVGL对象
 * @param pm_page 页面指针
 */
static void _open_page_object(lv_pm_page_t pm_page)
{
    /* 创建或重用设备组 */
    if (pm_page->group == NULL) {
        pm_page->group = _create_or_reuse_group(pm_page->group);
        _set_input_group(pm_page->group);
    }

    /* 创建页面对象 */
    pm_page->page = lv_obj_create(g_screen);

    /* 应用主题 */
    lv_pm_page_theme_init(pm_page);

    /* 调用用户打开回调 */
    if (pm_page->open_cb) {
        pm_page->open_cb(pm_page);
    }

    /* 重启定时器 */
    if (pm_page->page_timer != NULL) {
        lv_timer_resume(pm_page->page_timer);
    }

    /* 配置UI */
    _configure_page_object(pm_page->page, pm_page);
    _configure_screen_scrolling();
    _configure_top_bars(pm_page);
    _configure_page_focus(pm_page);

    lv_obj_clear_flag(pm_page->page, LV_OBJ_FLAG_HIDDEN);
}


/* ======================== 跳转辅助函数 ======================== */

/**
 * @brief 快速关闭从当前页面到目标页面之间的所有页面
 * @param target_index 目标页面在路由栈中的位置
 */
static void _close_pages_between(int target_index)
{
    int pages_to_remove = g_pm_history_count - target_index - 1;
    LV_LOG_INFO("[PAGE_MANAGER] [JUMP] 需要删除 %d 个中间页面", pages_to_remove);

    for (int i = g_pm_history_count - 1; i > target_index; i--) {
        lv_pm_id delete_id = g_pm_history[i];
        lv_pm_page_t delete_page = _get_page_by_id(delete_id);

        if (delete_page != NULL) {
            LV_LOG_INFO("[PAGE_MANAGER] [JUMP] 删除页面[%d/%d]: ID=%d, Name=%s",
                   g_pm_history_count - i, pages_to_remove,
                   delete_page->id, delete_page->name);

            // ✅ 先禁用交互
            if (delete_page->page != NULL) {
                lv_obj_add_flag(delete_page->page, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(delete_page->page, LV_OBJ_FLAG_CLICKABLE);
            }

            // ✅ 调用回调
            if (delete_page->dis_disappear_cb) {
                delete_page->dis_disappear_cb(delete_page);
            }
            if (delete_page->close_cb) {
                delete_page->close_cb(delete_page);
            }

            // ✅ 释放资源
            lv_pm_delete_page(delete_id);

            LV_LOG_INFO("[PAGE_MANAGER] [JUMP] 页面已删除: ID=%d", delete_id);
        } else {
            LV_LOG_WARN("[PAGE_MANAGER] [JUMP] 页面未找到: ID=%d", delete_id);
        }

        // ✅ 更新路由表计数
        g_pm_history_count--;
        g_pm_history[g_pm_history_count] = 0;
    }
}

/**
 * @brief 显示目标页面
 * @param target_page 目标页面
 * @param msg_data 消息数据
 */
static void _show_target_page(lv_pm_page_t target_page, void* msg_data)
{
    if (target_page == NULL || target_page->page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] [JUMP] 目标页面无效");
        return;
    }

    target_page->flag.is_back = 1;

    _set_input_group(target_page->group);

    _pm_msg_replace(target_page, msg_data);

    if (target_page->will_appear_cb) {
        LV_LOG_INFO("[PAGE_MANAGER] [JUMP] 执行目标页面 will_appear_cb");
        target_page->will_appear_cb(target_page);
    }

    if (target_page->page_timer != NULL) {
        lv_timer_resume(target_page->page_timer);
    }

    // ✅ 确保页面可见和可交互
    lv_obj_clear_flag(target_page->page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(target_page->page, LV_OBJ_FLAG_CLICKABLE);

    // ✅ 将页面移到最前面
    lv_obj_move_to_index(target_page->page, -1);

    if (target_page->dis_appear_cb) {
        LV_LOG_INFO("[PAGE_MANAGER] [JUMP] 执行目标页面 dis_appear_cb");
        target_page->dis_appear_cb(target_page);
    }

#if LV_PM_USE_STA_BAR || LV_PM_USE_BACK_BAR
    lv_pm_bar_apply_for_page(target_page);
#endif

    LV_LOG_INFO("[PAGE_MANAGER] [JUMP] 页面显示完成: ID=%d, 页面对象=%p, 隐藏标志=%d",
           target_page->id, target_page->page,
           lv_obj_has_flag(target_page->page, LV_OBJ_FLAG_HIDDEN));
}


/* ======================== 页面资源释放 ======================== */

/**
 * @brief 释放页面的所有组件 - 增强防护版本
 * @param pm_page 页面指针
 */
static void _release_page_components(lv_pm_page_t pm_page)
{
    if (pm_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 尝试释放空页面指针");
        return;
    }

    // 释放组
    if (pm_page->group != NULL) {
        lv_group_del(pm_page->group);
        pm_page->group = NULL;
        LV_LOG_INFO("[PAGE_MANAGER] 页面组已删除");
    }

    // 释放定时器
    if (pm_page->page_timer != NULL) {
        // lv_timer_del(pm_page->page_timer);
        lv_timer_pause(pm_page->page_timer);
        // pm_page->page_timer = NULL;
        LV_LOG_INFO("[PAGE_MANAGER] 页面定时器已暂停");
    }

    // 释放用户数据
    if (pm_page->user_data != NULL) {
        lv_pm_free(pm_page->user_data);
        pm_page->user_data = NULL;
    }

    _pm_msg_clear(pm_page);

    // 释放 LVGL 对象 - 最后释放
    if (pm_page->page != NULL) {
        lv_obj_clean(pm_page->page);
        lv_obj_del(pm_page->page);
        pm_page->page = NULL;
        LV_LOG_INFO("[PAGE_MANAGER] 页面LVGL对象已删除");
    }
}


/* ======================== 公开API - 初始化 ======================== */

uint8_t lv_pm_init(int page_num)
{
    g_pm_pages_capacity = page_num;

    g_pm_pages = (lv_pm_page_t*)lv_pm_malloc(sizeof(lv_pm_page_t) * page_num);
    lv_memset(g_pm_pages, 0, g_pm_pages_capacity * sizeof(lv_pm_page_t));

    g_pm_history = (lv_pm_id*)lv_pm_malloc(sizeof(lv_pm_id) * page_num);
    lv_memset(g_pm_history, 0, g_pm_pages_capacity * sizeof(lv_pm_id));

    g_screen = lv_scr_act();

    _init_screen_config();
    _init_top_bars();

    LV_LOG_INFO("[PAGE_MANAGER] 初始化完成，最大页面数: %d", page_num);

    return 0;
}

/**
 * @brief lv_pm re set page num。
 */
uint8_t lv_pm_re_set_page_num(int page_num)
{
    g_pm_pages_capacity = page_num;

    lv_pm_page_t* new_pages = (lv_pm_page_t*)lv_pm_malloc(sizeof(lv_pm_page_t) * page_num);
    lv_memset(new_pages, 0, g_pm_pages_capacity * sizeof(lv_pm_page_t));
    lv_pm_free(g_pm_pages);
    g_pm_pages = new_pages;

    lv_pm_id* new_history = (lv_pm_id*)lv_pm_malloc(sizeof(lv_pm_id) * page_num);
    lv_memset(new_history, 0, g_pm_pages_capacity * sizeof(lv_pm_id));
    lv_pm_free(g_pm_history);
    g_pm_history = new_history;

    LV_LOG_INFO("[PAGE_MANAGER] 页面数量已重设为: %d", page_num);

    return 0;
}


/* ======================== 公开API - 查询 ======================== */

lv_pm_page_t lv_pm_get_pm_page(void* lv_pm_page)
{
    return (lv_pm_page_t)(lv_pm_page);
}

/**
 * @brief lv_pm get crr page。
 */
lv_pm_page_t lv_pm_get_crr_page(void)
{
    if (g_pm_history_count == 0) {
        LV_LOG_ERROR("[PAGE_MANAGER] 路由表为空");
        return NULL;
    }
    return _get_page_by_id(g_pm_history[g_pm_history_count - 1]);
}

/**
 * @brief lv_pm foreach registered page。
 */
void lv_pm_foreach_registered_page(void (*fn)(lv_pm_page_t page, void * user_data),
                                   void * user_data)
{
    if (fn == NULL) {
        return;
    }

    for (int i = 0; i < g_pm_pages_capacity; i++) {
        if (g_pm_pages[i] != NULL) {
            fn(g_pm_pages[i], user_data);
        }
    }
}


/* ======================== 公开API - 创建与删除 ======================== */

lv_pm_page_t lv_pm_create_page(lv_pm_id id, const char* name)
{
    if (!_is_valid_id(id)) {
        LV_LOG_ERROR("[PAGE_MANAGER] 无效的ID: %d", id);
        return NULL;
    }

    lv_pm_page_t existing_page = _get_page_by_id(id);
    if (existing_page != NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 重复创建ID -> %d, %s", id, name);
        return (lv_pm_page_t)-1;
    }

    lv_pm_page_t new_page = (lv_pm_page_t)lv_pm_malloc(sizeof(struct lv_pm_page_def));
    if (new_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 创建页面申请内存失败: ID=%d", id);
        return NULL;
    }

    lv_memset(new_page, 0, sizeof(struct lv_pm_page_def));
    new_page->id = id;
    new_page->name = (char*)name;
    g_pm_pages[id] = new_page;

    LV_LOG_INFO("[PAGE_MANAGER] 页面创建成功: ID=%d, Name=%s", id, name);

    return g_pm_pages[id];
}

/**
 * @brief lv_pm delete page。
 */
void lv_pm_delete_page(lv_pm_id id)
{
    // 防护：检查 ID 的有效范围
    if (!_is_valid_id(id)) {
        LV_LOG_ERROR("[PAGE_MANAGER] 无效的ID: %d (有效范围: 0-%d)",
                     id, g_pm_pages_capacity - 1);
        return;
    }

    lv_pm_page_t pm_page = _get_page_by_id(id);
    if (pm_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 页面未创建: ID=%d", id);
        return;
    }

    // 防护：检查页面是否已经被删除
    if (pm_page->flag.is_open == 0 && pm_page->page == NULL) {
        LV_LOG_WARN("[PAGE_MANAGER] 页面已删除，重复删除: ID=%d", id);
        return;
    }

    pm_page->flag.is_open = 0;
    pm_page->focus_obj = NULL;

    _release_page_components(pm_page);

    LV_LOG_INFO("[PAGE_MANAGER] 资源回收完成: ID -> %d", id);
}

/**
 * @brief lv_pm delete deep page。
 */
void lv_pm_delete_deep_page(lv_pm_id id)
{
    lv_pm_page_t pm_page = _get_page_by_id(id);
    if (pm_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 未查询到ID -> %d", id);
        return;
    }

    lv_pm_delete_page(id);

    lv_pm_free(pm_page);
    g_pm_pages[id] = NULL;

    LV_LOG_INFO("[PAGE_MANAGER] 页面完全释放: ID -> %d", id);
}


/* ======================== 公开API - 组管理 ======================== */

/**
 * @brief 设置默认的输入设备组（支持模拟器和真实设备）
 * @param set_group 要设置的分组
 *
 * 这个函数在模拟器和真实设备上有不同的行为：
 * - 模拟器：设置指针、按键、编码器设备的组
 * - 真实设备：仅设置为默认组
 */
#ifdef user_lv_simulator
extern lv_indev_t* lv_win32_pointer_device_object;
extern lv_indev_t* lv_win32_keypad_device_object;
extern lv_indev_t* lv_win32_encoder_device_object;

/**
 * @brief lv_pm set group default。
 */
void lv_pm_set_group_default(lv_group_t* set_group)
{
    lv_group_set_default(set_group);
    lv_indev_set_group(lv_win32_pointer_device_object, set_group);
    lv_indev_set_group(lv_win32_keypad_device_object, set_group);
    lv_indev_set_group(lv_win32_encoder_device_object, set_group);
}
#else
/**
 * @brief lv_pm set group default。
 */
void lv_pm_set_group_default(lv_group_t* set_group)
{
    lv_group_set_default(set_group);
}
#endif

/**
 * @brief 创建一个新的输入设备组
 * @return 新创建的分组指针
 *
 * 这是一个公开的辅助函数，用户可以调用它来创建新的设备组
 */
lv_group_t* lv_pm_create_group(void)
{
    return lv_group_create();
}



/**
 * @brief lv_pm nav busy。
 */
bool lv_pm_nav_busy(void)
{
    return g_nav_anim_ref > 0u;
}

/**
 * @brief lv_pm nav anim begin。
 */
void lv_pm_nav_anim_begin(void)
{
    if (g_nav_anim_ref < 255u) {
        g_nav_anim_ref++;
    }
}

/**
 * @brief lv_pm nav anim end。
 */
void lv_pm_nav_anim_end(void)
{
    if (g_nav_anim_ref > 0u) {
        g_nav_anim_ref--;
    }
}

/**
 * @brief lv_pm nav set busy。
 */
void lv_pm_nav_set_busy(bool busy)
{
    if (busy) {
        lv_pm_nav_anim_begin();
    }
    else {
        g_nav_anim_ref = 0u;
    }
}

/**
 * @brief lv_pm nav drain。
 */
void lv_pm_nav_drain(void)
{
    if (g_nav_anim_ref > 0u) {
        LV_LOG_WARN("[PAGE_MANAGER] 导航抢占：结束未完成转场 (ref=%u)",
                    (unsigned)g_nav_anim_ref);
    }

    lv_pm_anima_drain_all();
    g_nav_anim_ref = 0u;
}

static void _nav_preempt_if_busy(void)
{
    if (lv_pm_nav_busy()) {
        lv_pm_nav_drain();
    }
}

/**
 * @brief lv_pm set nav notify cb。
 */
void lv_pm_set_nav_notify_cb(lv_pm_nav_notify_cb cb, void * user_data)
{
    g_nav_notify_cb = cb;
    g_nav_notify_ud = user_data;
}

/**
 * @brief lv_pm page get msg。
 */
void * lv_pm_page_get_msg(void * pm_page)
{
    lv_pm_page_t page = lv_pm_get_pm_page(pm_page);

    if (page == NULL) {
        return NULL;
    }
    return page->msg_data;
}

/* ======================== 公开API - 页面生命周期 ======================== */

int lv_pm_open_page_msg(lv_pm_id id, void* msg_data)
{
    _nav_preempt_if_busy();

    if (g_pm_pages[id] == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 不存在页面ID -> %d", id);
        return -1;
    }

    if (g_pm_history_count == g_pm_pages_capacity) {
        LV_LOG_ERROR("[PAGE_MANAGER] 路由表用尽 -> %d", id);
        return -2;
    }

    lv_pm_page_t open_page = _get_page_by_id(id);
    if (open_page->flag.is_open == 1) {
        LV_LOG_ERROR("[PAGE_MANAGER] 重复打开ID -> %d", id);
        return -3;
    }

    /* 添加到路由表 */
    g_pm_history[g_pm_history_count] = id;
    open_page->flag.is_open = 1;
    open_page->flag.is_back = 0;

    /* 处理前一个页面 */
    if (g_pm_history_count != 0) {
        lv_pm_id prev_id = g_pm_history[g_pm_history_count - 1];
        lv_pm_page_t prev_page = _get_page_by_id(prev_id);

        if (prev_page != NULL) {
            prev_page->flag.is_back = false;

            if (prev_page->page != NULL) {
                lv_obj_clear_flag(prev_page->page, LV_OBJ_FLAG_CLICKABLE);
            }

            if (prev_page->will_disappear_cb) {
                prev_page->will_disappear_cb(prev_page);
            }

            /* Covered page: EXIT (dis_appear) of its own lv_pm_anima_t pair. */
            lv_pm_anima_play(prev_page, LV_PM_ANIMA_EXIT, &open_page->open_options,
                             _on_close_animation_complete);
        } else {
            LV_LOG_ERROR("[PAGE_MANAGER] 前一页面无效: ID=%d", prev_id);
        }
    }

    _pm_msg_replace(open_page, msg_data);

    /* 打开页面UI */
    _open_page_object(open_page);

    /* 调用生命周期回调 */
    if (open_page->will_appear_cb) {
        open_page->will_appear_cb(open_page);
    }

    /* New page: ENTER (appear) of its own lv_pm_anima_t pair. */
    lv_pm_anima_play(open_page, LV_PM_ANIMA_ENTER, NULL, _on_open_animation_complete);

    _handle_open_target(open_page);

    LV_LOG_INFO("[PAGE_MANAGER] 页面打开成功: ID=%d, Name=%s", open_page->id, open_page->name);

    return 0;
}

/**
 * @brief 关闭当前页面并返回前一页面（推荐版本）
 *
 * 执行流程：
 *   1. 当前页面: will_disappear_cb → 关闭动画 → dis_disappear_cb → close_cb → 删除
 *   2. 前一页面: 显示 → will_appear_cb → 打开动画 → dis_appear_cb
 *
 * ✓ 使用动画完成回调确保回调顺序
 * ✓ 所有回调都在适当的时机触发
 * ✓ 资源释放安全
 * ✓ 与 lv_pm_open_page_msg() 流程对称
 */
int lv_pm_close_page_msg(void* msg_data)
{
    _nav_preempt_if_busy();

    if (g_pm_history_count <= 1) {
        LV_LOG_ERROR("[PAGE_MANAGER] 无法返回（路由表大小 <= 1）");
        return -1;
    }

    /* 获取当前页面 */
    lv_pm_id current_id = g_pm_history[g_pm_history_count - 1];
    lv_pm_page_t current_page = _get_page_by_id(current_id);

    if (current_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 当前页面无效: ID=%d", current_id);
        return -1;
    }

    current_page->flag.is_back = 1;

    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE] 关闭页面: ID=%d, Name=%s",
           current_page->id, current_page->name);

    /* Step 1: will_disappear - 页面即将隐藏 */
    if (current_page->will_disappear_cb) {
        current_page->will_disappear_cb(current_page);
    }

    /* Current page: EXIT (dis_appear). */
    lv_pm_anima_play(current_page, LV_PM_ANIMA_EXIT, NULL, _on_normal_close_animation_complete);

    /* ==================== 处理前一页面 ==================== */

    g_pm_history_count--;
    lv_pm_id prev_id = g_pm_history[g_pm_history_count - 1];
    lv_pm_page_t prev_page = _get_page_by_id(prev_id);

    if (prev_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] 前一页面无效: ID=%d", prev_id);
        return -1;
    }

    prev_page->flag.is_back = 1;

    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE] 显示前一页面: ID=%d, Name=%s",
           prev_page->id, prev_page->name);

    _set_input_group(prev_page->group);

    _pm_msg_replace(prev_page, msg_data);

    /* will_appear */
    if (prev_page->will_appear_cb) {
        prev_page->will_appear_cb(prev_page);
    }

    /* 恢复定时器 */
    if (prev_page->page_timer != NULL) {
        lv_timer_resume(prev_page->page_timer);
    }

    /* 显示页面 */
    if (prev_page->page != NULL) {
        lv_obj_clear_flag(prev_page->page, LV_OBJ_FLAG_HIDDEN);
    }

    /* Restored page: ENTER (appear, is_back=1) — inverse of its earlier EXIT. */
    lv_pm_anima_play(prev_page, LV_PM_ANIMA_ENTER, NULL, _on_back_open_animation_complete);

    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE] 页面关闭成功，已返回: ID=%d, Name=%s",
           prev_page->id, prev_page->name);

    return 0;
}


/**
 * @brief 快速关闭到指定页面（支持动画）
 *
 * 执行流程：
 *   1. 所有中间页面: will_disappear_cb → 关闭动画 → dis_disappear_cb → close_cb
 *   2. 目标页面: will_appear_cb → 打开动画 → dis_appear_cb
 *
 * 示例：
 *   当前页面栈: [Page1, Page2, Page3, Page4, Page5]
 *   调用: lv_pm_close_to_page_msg(LV_PM_ID_MAIN, NULL)
 *   结果: [Page1, Page1正在打开动画中]
 *   Page5, Page4, Page3, Page2 分别播放关闭动画后删除
 *
 * ✓ 每个页面都有完整的生命周期回调
 * ✓ 所有关闭页面都有关闭动画
 * ✓ 目标页面有打开动画
 * ✓ 资源释放安全
 */
int lv_pm_close_to_page_msg(lv_pm_id target_id, void* msg_data)
{
    _nav_preempt_if_busy();

    if (g_pm_history_count <= 1) {
        LV_LOG_ERROR("[PAGE_MANAGER] 无法返回（路由表大小 <= 1）");
        return -1;
    }

    /* 查找目标页面在历史栈中的位置 */
    int target_index = -1;
    for (int i = 0; i < g_pm_history_count; i++) {
        if (g_pm_history[i] == target_id) {
            target_index = i;
            break;
        }
    }

    if (target_index < 0) {
        LV_LOG_ERROR("[PAGE_MANAGER] [CLOSE_TO] 目标页面不在路由表中: ID=%d",
                     target_id);
        return -3;
    }

    if (target_index == g_pm_history_count - 1) {
        LV_LOG_ERROR("[PAGE_MANAGER] [CLOSE_TO] 当前页面就是目标页面");
        return -1;
    }

    /* ================== 关闭所有中间页面 ================== */
    int pages_to_close = g_pm_history_count - target_index - 1;
    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_TO] 需要关闭 %d 个中间页面", pages_to_close);

    for (int i = g_pm_history_count - 1; i > target_index; i--) {
        lv_pm_id cur_id = g_pm_history[i];
        lv_pm_page_t page = _get_page_by_id(cur_id);

        if (page == NULL) {
            LV_LOG_ERROR("[PAGE_MANAGER] [CLOSE_TO] 页面无效: ID=%d", cur_id);
            g_pm_history_count--;
            continue;
        }

        page->flag.is_back = 1;

        LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_TO] 关闭页面: ID=%d, Name=%s",
               page->id, page->name);

        /* Step 1: will_disappear - 在隐藏前通知 */
        if (page->will_disappear_cb) {
            page->will_disappear_cb(page);
        }

        /* Step 2: 启动关闭动画 */
        lv_pm_anima_play(page, LV_PM_ANIMA_EXIT, NULL, _on_batch_close_animation_complete);
    }

    /* ================== 更新路由栈 ================== */
    g_pm_history_count = target_index + 1;

    /* ================== 显示目标页面 ================== */
    lv_pm_page_t target_page = _get_page_by_id(target_id);
    if (target_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] [CLOSE_TO] 目标页面无效: ID=%d",
                     target_id);
        return -2;
    }

    target_page->flag.is_back = 1;

    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_TO] 显示目标页面: ID=%d, Name=%s",
           target_page->id, target_page->name);

    _set_input_group(target_page->group);

    _pm_msg_replace(target_page, msg_data);

    /* will_appear */
    if (target_page->will_appear_cb) {
        target_page->will_appear_cb(target_page);
    }

    /* 恢复定时器 */
    if (target_page->page_timer != NULL) {
        lv_timer_resume(target_page->page_timer);
    }

    /* 显示页面 */
    if (target_page->page != NULL) {
        lv_obj_clear_flag(target_page->page, LV_OBJ_FLAG_HIDDEN);
    }

    lv_pm_anima_play(target_page, LV_PM_ANIMA_ENTER, NULL, _on_back_open_animation_complete);

    LV_LOG_INFO("[PAGE_MANAGER] [CLOSE_TO] 已返回目标页面: ID=%d, Name=%s",
           target_page->id, target_page->name);

    return 0;
}


/**
 * @brief lv_pm jump to page msg。
 */
int lv_pm_jump_to_page_msg(lv_pm_id target_id, void * msg_data)
{
    _nav_preempt_if_busy();
    return lv_pm_close_to_page_msg(target_id, msg_data);
}

/**
 * @brief 无动画快速返回到指定页面（最快，但需要谨慎）
 *
 * 这是一个新增的 API，用于确实不需要动画的场景
 *
 * 特点：
 *   - 没有关闭和打开动画
 *   - 但完全保证生命周期回调的安全执行顺序
 *   - 比 lv_pm_close_to_page_msg() 快，但没有动画效果
 *
 * 使用场景：
 *   - 用户按下硬件返回键，需要最快的响应
 *   - 某些页面禁用动画
 *   - 需要立即跳转到特定页面
 *
 * 注意：这个函数直接调用回调，但顺序是安全的
 */
int lv_pm_jump_to_page_msg_no_animation(lv_pm_id target_id, void* msg_data)
{
    _nav_preempt_if_busy();

    if (g_pm_history_count <= 1) {
        LV_LOG_ERROR("[PAGE_MANAGER] 无法返回（路由表大小 <= 1）");
        return -1;
    }

    /* 查找目标页面在历史栈中的位置 */
    int target_index = -1;
    for (int i = 0; i < g_pm_history_count; i++) {
        if (g_pm_history[i] == target_id) {
            target_index = i;
            break;
        }
    }

    if (target_index < 0) {
        LV_LOG_ERROR("[PAGE_MANAGER] [JUMP_NO_ANIMA] 目标页面不在路由表中: ID=%d",
                     target_id);
        return -3;
    }

    if (target_index == g_pm_history_count - 1) {
        LV_LOG_ERROR("[PAGE_MANAGER] [JUMP_NO_ANIMA] 当前页面就是目标页面");
        return -1;
    }

    /* ================== 关闭所有中间页面（无动画） ================== */
    int pages_to_close = g_pm_history_count - target_index - 1;
    LV_LOG_INFO("[PAGE_MANAGER] [JUMP_NO_ANIMA] 需要关闭 %d 个中间页面（无动画）",
           pages_to_close);

    for (int i = g_pm_history_count - 1; i > target_index; i--) {
        lv_pm_id cur_id = g_pm_history[i];
        lv_pm_page_t page = _get_page_by_id(cur_id);

        if (page == NULL) {
            LV_LOG_ERROR("[PAGE_MANAGER] [JUMP_NO_ANIMA] 页面无效: ID=%d",
                         cur_id);
            continue;
        }

        LV_LOG_INFO("[PAGE_MANAGER] [JUMP_NO_ANIMA] 关闭页面: ID=%d, Name=%s",
               page->id, page->name);

        /* 使用安全的无动画关闭函数 */
        _close_page_without_animation(page);

        /* 删除页面对象 */
        lv_pm_delete_page(page->id);
    }

    /* ================== 更新路由栈 ================== */
    g_pm_history_count = target_index + 1;

    /* ================== 显示目标页面（无动画） ================== */
    lv_pm_page_t target_page = _get_page_by_id(target_id);
    if (target_page == NULL) {
        LV_LOG_ERROR("[PAGE_MANAGER] [JUMP_NO_ANIMA] 目标页面无效: ID=%d",
                     target_id);
        return -2;
    }

    target_page->flag.is_back = 1;

    LV_LOG_INFO("[PAGE_MANAGER] [JUMP_NO_ANIMA] 显示目标页面（无动画）: ID=%d, Name=%s",
           target_page->id, target_page->name);

    _set_input_group(target_page->group);

    _pm_msg_replace(target_page, msg_data);

    /* will_appear */
    if (target_page->will_appear_cb) {
        target_page->will_appear_cb(target_page);
    }

    /* 恢复定时器 */
    if (target_page->page_timer != NULL) {
        lv_timer_resume(target_page->page_timer);
    }

    /* 显示页面（无动画） */
    if (target_page->page != NULL) {
        lv_obj_clear_flag(target_page->page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(target_page->page, LV_OBJ_FLAG_CLICKABLE);
    }

    /* dis_appear（因为没有动画，直接认为展示完成） */
    if (target_page->dis_appear_cb) {
        target_page->dis_appear_cb(target_page);
    }

    if (lv_pm_theme_get() != LV_PM_THEME_ID_INVALID) {
        lv_pm_theme_apply(target_page);
    }
#if LV_PM_USE_STA_BAR || LV_PM_USE_BACK_BAR
    lv_pm_bar_apply_for_page(target_page);
#endif
    _nav_notify(target_page, LV_PM_NAV_EVT_APPEAR_DONE);

    LV_LOG_INFO("[PAGE_MANAGER] [JUMP_NO_ANIMA] 已跳转到目标页面: ID=%d, Name=%s",
           target_page->id, target_page->name);

    return 0;
}
/* ======================== 调试函数 ======================== */

void printf_all_page(void)
{
    LV_LOG_INFO("\n========== 页面管理器状态 ==========");
    LV_LOG_INFO("页面数组大小: %d", g_pm_pages_capacity);
    LV_LOG_INFO("路由表大小: %d\n", g_pm_history_count);

    LV_LOG_INFO("所有已创建的页面:");
    for (int i = 0; i < g_pm_pages_capacity; i++) {
        lv_pm_page_t pm_page = _get_page_by_id(i);
        if (pm_page != NULL) {
            LV_LOG_INFO("[%d/%d] 页面指针: %p, ID: %d, 名称: %s, 状态: %s",
                   i + 1, g_pm_pages_capacity,
                   pm_page, pm_page->id, pm_page->name,
                   pm_page->flag.is_open ? "打开" : "关闭");
        }
    }

    LV_LOG_INFO("\n当前路由表 (页面堆栈):");
    for (int i = 0; i < g_pm_history_count; i++) {
        lv_pm_page_t pm_page = _get_page_by_id(g_pm_history[i]);
        if (pm_page != NULL) {
            LV_LOG_INFO("[%d/%d] 指针: %p, ID: %d, 名称: %s",
                   i + 1, g_pm_history_count,
                   pm_page, pm_page->id, pm_page->name);
        }
    }
    LV_LOG_INFO("=====================================\n");
}