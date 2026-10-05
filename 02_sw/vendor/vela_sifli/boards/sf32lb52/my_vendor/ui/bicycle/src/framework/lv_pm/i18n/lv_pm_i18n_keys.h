/**
 * @file lv_pm_i18n_keys.h
 * @brief lv_pm 页面管理 — i18n_keys。
 */

#ifndef LV_PM_I18N_KEYS_H
#define LV_PM_I18N_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** @brief 添加 UI 字符串时用 LV_PM_I18N_KEY_LIST(X) 扩展。 */
#define LV_PM_I18N_KEY_LIST(X)          \
    X(USB_TITLE)                        \
    X(MTP_OP_UPLOAD)                    \
    X(MTP_OP_DOWNLOAD)                  \
    X(MTP_OP_MKDIR)                     \
    X(MTP_OP_DELETE)                    \
    X(MTP_OP_RENAME)                    \
    X(MTP_OP_MOVE)                      \
    X(MTP_OP_COPY)                      \
    X(STARTUP_LOADING)                  \
    X(STARTUP_READY)                    \
    X(STARTUP_LOAD_DONE)

typedef enum {
#define LV_PM_I18N_KEY_ENUM(name) LV_PM_I18N_KEY_##name,
    LV_PM_I18N_KEY_LIST(LV_PM_I18N_KEY_ENUM)
#undef LV_PM_I18N_KEY_ENUM
    LV_PM_I18N_KEY_COUNT
} lv_pm_i18n_key_t;

#ifdef __cplusplus
}
#endif

#endif /* LV_PM_I18N_KEYS_H */
