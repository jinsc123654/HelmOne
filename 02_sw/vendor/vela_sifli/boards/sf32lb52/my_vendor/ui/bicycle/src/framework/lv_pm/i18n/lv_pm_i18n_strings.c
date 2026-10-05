/**
 * @file lv_pm_i18n_strings.c
 * @brief lv_pm 页面管理 — i18n_strings。
 */

#include "lv_pm_i18n_keys.h"
#include "lv_pm_i18n_locales.h"

static const char * const s_strings_zh_cn[LV_PM_I18N_KEY_COUNT] = {
    [LV_PM_I18N_KEY_USB_TITLE] = "文件传输模式",
    [LV_PM_I18N_KEY_MTP_OP_UPLOAD] = "上传",
    [LV_PM_I18N_KEY_MTP_OP_DOWNLOAD] = "下载",
    [LV_PM_I18N_KEY_MTP_OP_MKDIR] = "新建目录",
    [LV_PM_I18N_KEY_MTP_OP_DELETE] = "删除",
    [LV_PM_I18N_KEY_MTP_OP_RENAME] = "重命名",
    [LV_PM_I18N_KEY_MTP_OP_MOVE] = "移动",
    [LV_PM_I18N_KEY_MTP_OP_COPY] = "复制",
    [LV_PM_I18N_KEY_STARTUP_LOADING] = "加载地图资源…",
    [LV_PM_I18N_KEY_STARTUP_READY] = "就绪",
    [LV_PM_I18N_KEY_STARTUP_LOAD_DONE] = "加载完成",
};

static const char * const s_strings_en[LV_PM_I18N_KEY_COUNT] = {
    [LV_PM_I18N_KEY_USB_TITLE] = "File Transfer",
    [LV_PM_I18N_KEY_MTP_OP_UPLOAD] = "Upload",
    [LV_PM_I18N_KEY_MTP_OP_DOWNLOAD] = "Download",
    [LV_PM_I18N_KEY_MTP_OP_MKDIR] = "New Folder",
    [LV_PM_I18N_KEY_MTP_OP_DELETE] = "Delete",
    [LV_PM_I18N_KEY_MTP_OP_RENAME] = "Rename",
    [LV_PM_I18N_KEY_MTP_OP_MOVE] = "Move",
    [LV_PM_I18N_KEY_MTP_OP_COPY] = "Copy",
    [LV_PM_I18N_KEY_STARTUP_LOADING] = "Loading map…",
    [LV_PM_I18N_KEY_STARTUP_READY] = "Ready",
    [LV_PM_I18N_KEY_STARTUP_LOAD_DONE] = "Load complete",
};

const lv_pm_locale_def_t lv_pm_locale_zh_cn = {
    .name = "zh_CN",
    .strings = s_strings_zh_cn,
};

const lv_pm_locale_def_t lv_pm_locale_en = {
    .name = "en",
    .strings = s_strings_en,
};

const lv_pm_locale_def_t * const lv_pm_builtin_locales[] = {
    &lv_pm_locale_zh_cn,
    &lv_pm_locale_en,
};

const unsigned lv_pm_builtin_locale_count =
    (unsigned)(sizeof(lv_pm_builtin_locales) / sizeof(lv_pm_builtin_locales[0]));
