/**
 * @file lv_pm_gpu_anima.c
 * @brief lv_pm 页面管理 — gpu_anima。
 */

/**
 * Page transitions using LVGL transform layers (SiFli EPIC GPU accelerated).
 */
#include "../include/lv_pm_gpu_anima.h"
#include "../include/lv_pm_disp.h"

#define GPU_SCALE_FULL   LV_SCALE_NONE  /* 256 = 100% */
#define GPU_SCALE_ZOOM   205            /* ~80% */
#define GPU_SCALE_DEPTH  225            /* ~88% */
#define GPU_SCALE_FLIP   32             /* ~12% */
#define GPU_SCALE_LEAVE  235            /* ~92% */
#define GPU_ROT_SLIDE    60             /* 6.0° */

typedef struct {
    int32_t x0;
    int32_t x1;
    int32_t rot0;
    int32_t rot1;
} gpu_rotate_slide_cfg_t;

static gpu_rotate_slide_cfg_t s_rs_cfg;

static void gpu_set_pivot(lv_obj_t * page, uint32_t dir)
{
    switch (dir) {
    case LV_PM_GPU_DIR_FROM_TOP:
        lv_obj_set_style_transform_pivot_x(page, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(page, 0, 0);
        break;
    case LV_PM_GPU_DIR_FROM_RIGHT:
        lv_obj_set_style_transform_pivot_x(page, lv_pct(100), 0);
        lv_obj_set_style_transform_pivot_y(page, lv_pct(50), 0);
        break;
    case LV_PM_GPU_DIR_FROM_LEFT:
        lv_obj_set_style_transform_pivot_x(page, 0, 0);
        lv_obj_set_style_transform_pivot_y(page, lv_pct(50), 0);
        break;
    default:
        lv_obj_set_style_transform_pivot_x(page, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(page, lv_pct(50), 0);
        break;
    }
}

/**
 * @brief lv_pm gpu page reset transform。
 */
void lv_pm_gpu_page_reset_transform(lv_obj_t * page)
{
    if (!page) {
        return;
    }

    lv_obj_set_style_transform_scale(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_transform_scale_x(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_transform_scale_y(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_transform_rotation(page, 0, 0);
    lv_obj_set_x(page, 0);
    lv_obj_set_y(page, 0);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
}

static void gpu_prepare_page(lv_pm_anima_data * data)
{
    lv_obj_t * page = data->pm_page->page;

    lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(page);
    gpu_set_pivot(page, data->pm_page->open_options.direction);
}

static void gpu_start_anim(lv_anim_t * anim, lv_pm_anima_data * data, lv_obj_t * page,
                           int32_t start, int32_t end, uint32_t time,
                           lv_anim_exec_xcb_t exec_cb, lv_anim_path_cb_t path_cb)
{
    lv_anim_init(anim);
    lv_anim_set_user_data(anim, data);
    lv_anim_set_var(anim, page);
    lv_anim_set_values(anim, start, end);
    lv_anim_set_time(anim, time);
    lv_anim_set_repeat_count(anim, 1);
    lv_anim_set_exec_cb(anim, exec_cb);
    lv_anim_set_ready_cb(anim, get_lv_pm_deleted_cb());
    lv_anim_set_path_cb(anim, path_cb ? path_cb : lv_anim_path_ease_out);
    lv_anim_start(anim);
}

static int32_t gpu_lerp(int32_t a, int32_t b, int32_t p)
{
    return a + (b - a) * p / 255;
}

/* ---------- zoom: uniform scale + opacity ---------- */

static void gpu_zoom_exec(void * var, int32_t progress)
{
    lv_obj_t * page = (lv_obj_t *)var;
    int32_t scale = gpu_lerp(GPU_SCALE_ZOOM, GPU_SCALE_FULL, progress);

    lv_obj_set_style_transform_scale(page, scale, 0);
    lv_obj_set_style_opa(page, (lv_opa_t)progress, 0);
}

static void gpu_zoom_out_exec(void * var, int32_t progress)
{
    lv_obj_t * page = (lv_obj_t *)var;
    int32_t inv = 255 - progress;
    int32_t scale = gpu_lerp(GPU_SCALE_ZOOM, GPU_SCALE_FULL, inv);

    lv_obj_set_style_transform_scale(page, scale, 0);
    lv_obj_set_style_opa(page, (lv_opa_t)inv, 0);
}

static void _pm_gpu_zoom_appear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);

    if (!data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
        lv_obj_set_style_transform_scale(page, GPU_SCALE_ZOOM, 0);
        lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
        gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_zoom_exec, lv_anim_path_ease_out);
    } else {
        /* Resume from overlay: inverse of dis_appear (zoom + fade in). */
        lv_obj_set_style_transform_scale(page, GPU_SCALE_ZOOM, 0);
        lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
        gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_zoom_exec, lv_anim_path_ease_out);
    }
}

static void _pm_gpu_zoom_disappear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);

    if (data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
    }

    lv_obj_set_style_transform_scale(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
    gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_zoom_out_exec, lv_anim_path_ease_in);
}

/* ---------- rotate + slide ---------- */
/* Do not use lv_pm_gpu_rotate_slide_anima on this product: rotate+slide
 * of a full page is too expensive (jank over LiveMap). Kept for reference. */

static void gpu_rotate_slide_exec(void * var, int32_t p)
{
    lv_obj_t * page = (lv_obj_t *)var;

    lv_obj_set_x(page, gpu_lerp(s_rs_cfg.x0, s_rs_cfg.x1, p));
    lv_obj_set_style_transform_rotation(page, gpu_lerp(s_rs_cfg.rot0, s_rs_cfg.rot1, p), 0);
    lv_obj_set_style_opa(page, (lv_opa_t)p, 0);
}

static void gpu_rotate_slide_out_exec(void * var, int32_t p)
{
    lv_obj_t * page = (lv_obj_t *)var;
    int32_t inv = 255 - p;

    lv_obj_set_x(page, gpu_lerp(s_rs_cfg.x0, s_rs_cfg.x1, p));
    lv_obj_set_style_transform_rotation(page, gpu_lerp(s_rs_cfg.rot0, s_rs_cfg.rot1, p), 0);
    lv_obj_set_style_opa(page, (lv_opa_t)inv, 0);
}

static void gpu_rotate_slide_setup(lv_pm_anima_data * data, bool leaving)
{
    lv_pm_open_options_t * opt = &data->pm_page->open_options;
    bool from_right = (opt->direction != LV_PM_GPU_DIR_FROM_LEFT);

    s_rs_cfg.x1 = 0;
    s_rs_cfg.rot1 = 0;

    if (from_right) {
        s_rs_cfg.x0 = leaving ? lv_pm_hor_res() : lv_pm_hor_res();
        s_rs_cfg.rot0 = leaving ? GPU_ROT_SLIDE : GPU_ROT_SLIDE;
    } else {
        s_rs_cfg.x0 = leaving ? -lv_pm_hor_res() : -lv_pm_hor_res();
        s_rs_cfg.rot0 = leaving ? -GPU_ROT_SLIDE : -GPU_ROT_SLIDE;
    }

    if (leaving) {
        s_rs_cfg.x0 = 0;
        s_rs_cfg.rot0 = 0;
        s_rs_cfg.x1 = from_right ? lv_pm_hor_res() : -lv_pm_hor_res();
        s_rs_cfg.rot1 = from_right ? GPU_ROT_SLIDE : -GPU_ROT_SLIDE;
    } else if (data->pm_page->flag.is_back) {
        s_rs_cfg.x0 = from_right ? -lv_pm_hor_res() / 4 : lv_pm_hor_res() / 4;
        s_rs_cfg.rot0 = from_right ? -GPU_ROT_SLIDE / 2 : GPU_ROT_SLIDE / 2;
    }
}

static void _pm_gpu_rotate_slide_appear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);
    lv_obj_set_style_transform_scale(page, GPU_SCALE_FULL, 0);
    gpu_rotate_slide_setup(data, false);

    if (!data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
        lv_obj_set_x(page, s_rs_cfg.x0);
        lv_obj_set_style_transform_rotation(page, s_rs_cfg.rot0, 0);
        lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
    }

    gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_rotate_slide_exec,
                   lv_anim_path_ease_in_out);
}

static void _pm_gpu_rotate_slide_disappear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);
    gpu_rotate_slide_setup(data, true);

    if (data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
    }

    lv_obj_set_x(page, 0);
    lv_obj_set_style_transform_rotation(page, 0, 0);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
    gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_rotate_slide_out_exec,
                   lv_anim_path_ease_in_out);
}

/* ---------- flip: scale_x + opacity ---------- */

static void gpu_flip_exec(void * var, int32_t progress)
{
    lv_obj_t * page = (lv_obj_t *)var;
    int32_t scale_x = gpu_lerp(GPU_SCALE_FLIP, GPU_SCALE_FULL, progress);

    lv_obj_set_style_transform_scale_x(page, scale_x, 0);
    lv_obj_set_style_transform_scale_y(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_opa(page, (lv_opa_t)progress, 0);
}

static void gpu_flip_out_exec(void * var, int32_t progress)
{
    lv_obj_t * page = (lv_obj_t *)var;
    int32_t inv = 255 - progress;
    int32_t scale_x = gpu_lerp(GPU_SCALE_FLIP, GPU_SCALE_FULL, inv);

    lv_obj_set_style_transform_scale_x(page, scale_x, 0);
    lv_obj_set_style_transform_scale_y(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_opa(page, (lv_opa_t)inv, 0);
}

static void _pm_gpu_flip_appear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);
    lv_obj_set_style_transform_pivot_x(page, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(page, lv_pct(50), 0);

    if (!data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
        lv_obj_set_style_transform_scale_x(page, GPU_SCALE_FLIP, 0);
        lv_obj_set_style_transform_scale_y(page, GPU_SCALE_FULL, 0);
        lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
        gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_flip_exec, lv_anim_path_ease_out);
    } else {
        /* Resume from overlay: inverse of dis_appear (flip + fade in). */
        lv_obj_set_style_transform_scale_x(page, GPU_SCALE_FLIP, 0);
        lv_obj_set_style_transform_scale_y(page, GPU_SCALE_FULL, 0);
        lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
        gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_flip_exec, lv_anim_path_ease_out);
    }
}

static void _pm_gpu_flip_disappear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);
    lv_obj_set_style_transform_pivot_x(page, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(page, lv_pct(50), 0);

    if (data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
    }

    lv_obj_set_style_transform_scale_x(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
    gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_flip_out_exec, lv_anim_path_ease_in);
}

/* ---------- depth: parallax scale + opacity ---------- */

static void gpu_depth_exec(void * var, int32_t progress)
{
    lv_obj_t * page = (lv_obj_t *)var;
    int32_t scale = gpu_lerp(GPU_SCALE_DEPTH, GPU_SCALE_FULL, progress);

    lv_obj_set_style_transform_scale(page, scale, 0);
    lv_obj_set_style_opa(page, (lv_opa_t)progress, 0);
}

static void gpu_depth_out_exec(void * var, int32_t progress)
{
    lv_obj_t * page = (lv_obj_t *)var;
    int32_t inv = 255 - progress;
    int32_t scale = gpu_lerp(GPU_SCALE_FULL, GPU_SCALE_LEAVE, progress);

    lv_obj_set_style_transform_scale(page, scale, 0);
    lv_obj_set_style_opa(page, (lv_opa_t)inv, 0);
}

static void _pm_gpu_depth_appear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);

    if (!data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
        lv_obj_set_style_transform_scale(page, GPU_SCALE_DEPTH, 0);
        lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
        gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_depth_exec, lv_anim_path_ease_out);
    } else {
        /* Resume from overlay: subtle zoom-in */
        lv_obj_set_style_transform_scale(page, GPU_SCALE_LEAVE, 0);
        lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
        gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_depth_exec, lv_anim_path_ease_out);
    }
}

static void _pm_gpu_depth_disappear(lv_pm_anima_data * data, lv_anim_t * anim)
{
    lv_obj_t * page = data->pm_page->page;
    lv_pm_open_options_t * opt = &data->pm_page->open_options;

    gpu_prepare_page(data);

    if (data->pm_page->flag.is_back) {
        lv_obj_move_foreground(page);
    }

    lv_obj_set_style_transform_scale(page, GPU_SCALE_FULL, 0);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
    gpu_start_anim(anim, data, page, 0, 255, opt->time, gpu_depth_out_exec, lv_anim_path_ease_in);
}

const lv_pm_anima_t lv_pm_gpu_zoom_anima = {
    _pm_gpu_zoom_appear,
    _pm_gpu_zoom_disappear,
};

const lv_pm_anima_t lv_pm_gpu_rotate_slide_anima = { /* do not use */
    _pm_gpu_rotate_slide_appear,
    _pm_gpu_rotate_slide_disappear,
};

const lv_pm_anima_t lv_pm_gpu_flip_anima = {
    _pm_gpu_flip_appear,
    _pm_gpu_flip_disappear,
};

const lv_pm_anima_t lv_pm_gpu_depth_anima = {
    _pm_gpu_depth_appear,
    _pm_gpu_depth_disappear,
};
