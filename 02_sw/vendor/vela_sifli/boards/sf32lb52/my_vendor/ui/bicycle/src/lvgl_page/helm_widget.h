/**
 * @file helm_widget.h
 * @brief 线稿共用控件：mhead / mlist / mitem / kv / sheet / card / dock。
 */

#ifndef HELM_WIDGET_H
#define HELM_WIDGET_H

#include "helm_icon.h"
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HELM_ITEM_GO = 0,
    HELM_ITEM_SW,
    HELM_ITEM_VAL,
    HELM_ITEM_SLIM
};

typedef struct {
    helm_ico_id_t ico;
    const char * title;
    const char * sub;
    const char * value;
    const lv_font_t * title_font; /**< NULL = MiSans 4bpp；通知/GPX 文件名用系统 TTF。 */
    uint32_t progress_color;      /**< 进度条填充色；0 使用 HELM_COLOR_CAD。 */
    uint8_t kind;
    uint8_t progress;             /**< 0..100，仅 progress_on=true 时显示。 */
    bool on;
    bool sel;
    bool progress_on;
    /** @brief 整行底色覆盖：配色试验页要展示"选中时整行是什么样"。
     *  @note 必须由 helm_row_set 每行清零 —— items 数组跨页复用，残留会漏到别的页。 */
    bool bg_set;
    uint32_t bg_color;
    /** @brief 这行的底色偏深 ⇒ 次级文字（副标题/右值）用浅色而不是 MUTE。
     *  @details 选中块也是一块深底，两者共用同一个开关：选中时由 helm_widget.c 按 mix 置位。 */
    bool bg_dark;
    bool live;                    /**< 只改文案/进度条，不滚动、不重光标。 */
} helm_item_t;

lv_obj_t * helm_label(lv_obj_t * parent, const lv_font_t * font, uint32_t color,
                      const char * txt);
/** @brief 打桩：绘制任务回调（统计 fill 方角/圆角、label、img 等）。
 *  @note DRAW_TASK_ADDED 只发给**正在绘制的那个对象**且它得带
 *        LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS ⇒ 想让分类数字真实，必须逐对象挂。 */
void helm_ui_task_cb(lv_event_t * e);
/** @brief 整页行建完之后调一次：滚动到选中行 + 落光标（见 helm_mitem_create 注释）。 */
void helm_mitem_list_built(lv_obj_t * list, int sel);
/** @brief 记录数缓存失效（开机/进页面/骑行结束后各一次，见 helm_menu.c）。 */
void helm_gpx_count_invalidate(void);
void helm_style_paper(lv_obj_t * obj);
void helm_style_scr(lv_obj_t * obj);
/** @brief 暖纸卡片：场地底上的数字面 / 列表行衬底。 */
void helm_style_card(lv_obj_t * obj);

lv_obj_t * helm_mhead_create(lv_obj_t * parent, const char * title);
void helm_mhead_set(lv_obj_t * head, const char * title);
/** @brief 换主题后重设页头样式（颜色是建页时烘进样式的，`helm_mhead_set` 只改文案）。 */
void helm_mhead_reapply(lv_obj_t * head);
/** @brief 顶栏标题字体。GPX 文件名用 TTF，其它页回到点阵。 */
void helm_mhead_set_font(lv_obj_t * head, const lv_font_t * font);

lv_obj_t * helm_mlist_create(lv_obj_t * parent);
/** @brief 关滑动光标并擦掉脏区。关页/淡出前调用，避免局部刷新把橙色条留在下一页。 */
void helm_mlist_cursor_off(lv_obj_t * list);
/** @brief 光标立刻落到 idx（进页/重建列表，不做滑动）。 */
void helm_mlist_sel_snap(lv_obj_t * list, uint8_t idx);
lv_obj_t * helm_mitem_create(lv_obj_t * list, const helm_item_t * spec);
/** @brief 行结构不变时就地改文案/图标/开关/选中。失败则调用方应重建该行。 */
bool helm_mitem_refresh(lv_obj_t * row, const helm_item_t * spec);
/** @brief 光标从 from 滑到 to（OLED_UI 非线性），必要时 scroll。 */
void helm_mlist_move_sel(lv_obj_t * list, uint8_t from, uint8_t to);
/** @brief 子页进入。dir>0 从右，dir<0 从左。 */
void helm_obj_enter(lv_obj_t * obj, int dir);
/** @brief 按键确认：行下压回弹（弹窗/dock）。 */
void helm_obj_press(lv_obj_t * obj);
/** @brief 列表确认：光标左右挤一下，不带动整表。 */
void helm_mlist_press_sel(lv_obj_t * list, uint8_t idx);
/** @brief 列表外框压扁后弹回（短按确认时用，不依赖按下事件）。 */
void helm_obj_squash_pulse(lv_obj_t * obj);
void helm_obj_squash(lv_obj_t * obj, bool down);
void helm_obj_squash_set(lv_obj_t * obj, bool down);
/** @brief 进页时子组件错落入场（只动 Y，不叠一层透明度）。 */
void helm_obj_stagger_in(lv_obj_t * parent);
void helm_obj_stagger_clear(lv_obj_t * parent);

/**
 * LVGL 9: after remove_style_all, size is LV_DPI_DEF (130). A flex-grow
 * child must start at 0. Never combine flex_grow with margin (clips the
 * border); pad the parent instead.
 */
static inline void helm_grow_y(lv_obj_t * obj)
{
    lv_obj_set_width(obj, lv_pct(100));
    lv_obj_set_height(obj, 0);
    lv_obj_set_style_min_height(obj, 0, 0);
    lv_obj_set_flex_grow(obj, 1);
}

static inline void helm_grow_x(lv_obj_t * obj)
{
    lv_obj_set_width(obj, 0);
    lv_obj_set_style_min_width(obj, 0, 0);
    lv_obj_set_flex_grow(obj, 1);
}

lv_obj_t * helm_softkeys_create(lv_obj_t * parent, uint32_t color);

lv_obj_t * helm_sum_cell(lv_obj_t * parent, helm_ico_id_t ico, uint32_t ico_color,
                         const char * lab, lv_obj_t ** val);
lv_obj_t * helm_sum_grid(lv_obj_t * parent);

lv_obj_t * helm_sheet_hero(lv_obj_t * parent, helm_ico_id_t ico, bool compact);
lv_obj_t * helm_kvbox_create(lv_obj_t * parent);
lv_obj_t * helm_kv_add(lv_obj_t * box, helm_ico_id_t ico, const char * k,
                       const char * v);
void helm_kvbox_seal(lv_obj_t * box);

lv_obj_t * helm_empty_create(lv_obj_t * parent, helm_ico_id_t ico,
                             const char * title, const char * sub);
/** @brief 空页已在则就地改图标/文案，否则重建。 */
void helm_empty_set(lv_obj_t * parent, helm_ico_id_t ico, const char * title,
                     const char * sub);

lv_obj_t * helm_pbar_create(lv_obj_t * parent);
void helm_pbar_set(lv_obj_t * bar, uint8_t pct);

lv_obj_t * helm_mask_create(lv_obj_t * parent);
lv_obj_t * helm_card_create(lv_obj_t * mask, helm_ico_id_t ico, const char * title,
                            bool alert);
static inline lv_obj_t * helm_card_body(lv_obj_t * card)
{
    return card ? (lv_obj_t *)lv_obj_get_user_data(card) : NULL;
}

lv_obj_t * helm_dock_create(lv_obj_t * parent, bool ok);

#ifdef __cplusplus
}
#endif

#endif /* HELM_WIDGET_H */
