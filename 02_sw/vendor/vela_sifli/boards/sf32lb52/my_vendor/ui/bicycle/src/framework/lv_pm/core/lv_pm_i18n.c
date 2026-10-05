/**
 * @file lv_pm_i18n.c
 * @brief lv_pm 页面管理 — i18n。
 */

#include <string.h>

#include "../include/lv_pm_i18n.h"
#include "../include/lv_pm_core.h"
#include "../i18n/lv_pm_i18n_locales.h"

static lv_pm_locale_id_t g_locale_id = LV_PM_I18N_LOCALE_INVALID;
static lv_pm_i18n_notify_cb g_i18n_notify;
static void * g_i18n_notify_ud;

static const lv_pm_locale_def_t * locale_def_for_id(lv_pm_locale_id_t id)
{
    if ((unsigned)id >= lv_pm_builtin_locale_count) {
        return NULL;
    }

    return lv_pm_builtin_locales[id];
}

/**
 * @brief lv_pm i18n locale count。
 * @return 请求的值。
 */
unsigned lv_pm_i18n_locale_count(void)
{
    return lv_pm_builtin_locale_count;
}

/**
 * @brief lv_pm i18n locale name。
 */
const char * lv_pm_i18n_locale_name(lv_pm_locale_id_t id)
{
    const lv_pm_locale_def_t * def = locale_def_for_id(id);

    if (def == NULL || def->name == NULL) {
        return "";
    }

    return def->name;
}

/**
 * @brief lv_pm i18n locale find。
 */
lv_pm_locale_id_t lv_pm_i18n_locale_find(const char * name)
{
    if (name == NULL || name[0] == '\0') {
        return LV_PM_I18N_LOCALE_INVALID;
    }

    for (unsigned i = 0; i < lv_pm_builtin_locale_count; i++) {
        if (lv_pm_builtin_locales[i]->name != NULL &&
            strcmp(lv_pm_builtin_locales[i]->name, name) == 0) {
            return (lv_pm_locale_id_t)i;
        }
    }

    return LV_PM_I18N_LOCALE_INVALID;
}

/**
 * @brief lv_pm i18n get。
 * @return 请求的值。
 */
lv_pm_locale_id_t lv_pm_i18n_get(void)
{
    return g_locale_id;
}

static const char * locale_string(const lv_pm_locale_def_t * def, lv_pm_i18n_key_t key)
{
    if (def == NULL || def->strings == NULL || (unsigned)key >= LV_PM_I18N_KEY_COUNT) {
        return NULL;
    }

    return def->strings[key];
}

/**
 * @brief lv_pm i18n get text。
 */
const char * lv_pm_i18n_get_text(lv_pm_i18n_key_t key)
{
    const char * text;

    if ((unsigned)key >= LV_PM_I18N_KEY_COUNT) {
        return "";
    }

    text = locale_string(locale_def_for_id(g_locale_id), key);
    if (text != NULL && text[0] != '\0') {
        return text;
    }

    text = locale_string(lv_pm_builtin_locales[0], key);
    if (text != NULL) {
        return text;
    }

    return "";
}

static void i18n_invoke_page(lv_pm_page_t page, void * user_data)
{
    lv_pm_locale_id_t id = (lv_pm_locale_id_t)(uintptr_t)user_data;

    if (page == NULL || page->i18n_changed_cb == NULL) {
        return;
    }

    page->i18n_changed_cb(page, id, page->i18n_changed_ud);
}

/**
 * @brief lv_pm i18n apply all。
 */
void lv_pm_i18n_apply_all(void)
{
    if (g_locale_id == LV_PM_I18N_LOCALE_INVALID) {
        return;
    }

    if (g_i18n_notify != NULL) {
        g_i18n_notify(g_locale_id, g_i18n_notify_ud);
    }

    lv_pm_foreach_registered_page(i18n_invoke_page, (void *)(uintptr_t)g_locale_id);
}

/**
 * @brief lv_pm i18n set。
 */
bool lv_pm_i18n_set(lv_pm_locale_id_t id)
{
    if (locale_def_for_id(id) == NULL) {
        return false;
    }

    if (id == g_locale_id) {
        lv_pm_i18n_apply_all();
        return true;
    }

    g_locale_id = id;
    lv_pm_i18n_apply_all();
    return true;
}

/**
 * @brief lv_pm i18n set by name。
 */
bool lv_pm_i18n_set_by_name(const char * name)
{
    lv_pm_locale_id_t id = lv_pm_i18n_locale_find(name);

    if (id == LV_PM_I18N_LOCALE_INVALID) {
        return false;
    }

    return lv_pm_i18n_set(id);
}

/**
 * @brief lv_pm i18n set next。
 */
bool lv_pm_i18n_set_next(void)
{
    lv_pm_locale_id_t next;

    if (lv_pm_builtin_locale_count == 0u) {
        return false;
    }

    if (g_locale_id == LV_PM_I18N_LOCALE_INVALID) {
        return lv_pm_i18n_set(0);
    }

    next = (lv_pm_locale_id_t)(((unsigned)g_locale_id + 1u) % lv_pm_builtin_locale_count);
    return lv_pm_i18n_set(next);
}

/**
 * @brief lv_pm set i18n changed。
 */
int8_t lv_pm_set_i18n_changed(void * page, lv_pm_page_i18n_cb cb, void * user_data)
{
    lv_pm_page_t pm_page = lv_pm_get_pm_page(page);

    if (pm_page == NULL) {
        return -1;
    }

    pm_page->i18n_changed_cb = cb;
    pm_page->i18n_changed_ud = user_data;
    return 0;
}

/**
 * @brief lv_pm set i18n notify cb。
 */
void lv_pm_set_i18n_notify_cb(lv_pm_i18n_notify_cb cb, void * user_data)
{
    g_i18n_notify = cb;
    g_i18n_notify_ud = user_data;
}
