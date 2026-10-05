# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Bicycle for SF32LB52 + /dev/lcd0 — pure C UI (lv_pm + vmap + MTP overlay).
# ##############################################################################

if(CONFIG_MYVENDOR_BICYCLE)
  set(BICYCLE_PROGNAME bicycle)
  set(BICYCLE_STACKSIZE ${CONFIG_MYVENDOR_BICYCLE_STACKSIZE})
  set(BICYCLE_PRIORITY ${CONFIG_MYVENDOR_BICYCLE_PRIORITY})
  set(BICYCLE_RESOURCE_DIR "/mnt/lfs")
  set(BICYCLE_PAGE_HOR_RES 240)
  set(BICYCLE_PAGE_VER_RES 320)

  set(BICYCLE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(INCDIR)

  add_compile_definitions(
    PAGE_HOR_RES=${BICYCLE_PAGE_HOR_RES}
    PAGE_VER_RES=${BICYCLE_PAGE_VER_RES})
  add_compile_definitions(
    "CONFIG_RESOURCE_DIR_PATH=\"${BICYCLE_RESOURCE_DIR}\"")
  add_compile_definitions(CONFIG_RESOURCE_FS_LETTER="")
  add_compile_definitions(MYVENDOR_BICYCLE_NATIVE_FONT=1)
  add_compile_definitions(BICYCLE_STABILITY_PROBE=0 BICYCLE_TS_OVERLAY=0)
  add_compile_definitions(
    "CONFIG_VMAP_DIR_PATH=\"/mnt/fat/map\"")
  add_compile_definitions(
    CONFIG_MYVENDOR_VMAP_CANVAS_MARGIN=${CONFIG_MYVENDOR_VMAP_CANVAS_MARGIN})
  add_compile_definitions(CONFIG_VMAP_ROUTE_WORKER_STACK=65536)

  list(APPEND INCDIR ${BICYCLE_DIR}/src)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/App)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/lvgl_page)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/lvgl_page/live_map)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/lvgl_page/usb_transfer)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/lvgl_page/startup)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/lvgl_page/factory)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/framework/lv_pm/include)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/framework/lv_pm/themes)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/framework/lv_pm/i18n)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/framework/lv_pm/font)
  list(APPEND INCDIR ${BICYCLE_DIR}/src/framework/lv_pm/img)
  list(APPEND INCDIR ${NUTTX_BOARD_ABS_DIR}/include)
  list(APPEND INCDIR ${NUTTX_BOARD_ABS_DIR}/ctl)
  list(APPEND INCDIR ${NUTTX_BOARD_ABS_DIR}/domain/gpx)
  list(APPEND INCDIR ${NUTTX_BOARD_ABS_DIR}/services/mtp_simple)

  set(LV_PM_DIR ${BICYCLE_DIR}/src/framework/lv_pm)
  set(GPX_DIR ${NUTTX_BOARD_ABS_DIR}/domain/gpx)

  set(MAINSRC ${BICYCLE_DIR}/src/bicycle_c_main.c)
  set(CSRCS
    ${BICYCLE_DIR}/src/lvgl_page/lvgl_page.c
    ${BICYCLE_DIR}/src/lvgl_page/bicycle_page_anima.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_shell.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_menu.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_pwr.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_splash.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_idle.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_toolbox.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_font.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_palette.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_font_lab.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_icon.c
    ${BICYCLE_DIR}/src/lvgl_page/helm_widget.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_ui_12.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_ui_15.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_ui_22.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_mism4_12.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_mism4_15.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_mism4_22.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_ui_64.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_num_28.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_num_32.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_num_40.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_num_56.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_num_64.c
    ${BICYCLE_DIR}/src/lvgl_page/fonts/helm_fa_16.c
    ${BICYCLE_DIR}/src/lvgl_page/live_map/map_page.c
    ${BICYCLE_DIR}/src/lvgl_page/usb_transfer/usb_transfer_page.c
    ${BICYCLE_DIR}/src/lvgl_page/startup/startup_page.c
    ${BICYCLE_DIR}/src/lvgl_page/startup/img_src_boot_logo.c
    ${BICYCLE_DIR}/src/bicycle_status_bar.c
    ${BICYCLE_DIR}/src/bicycle_mtp_ui.c
    ${BICYCLE_DIR}/src/bicycle_config.c
    ${BICYCLE_DIR}/src/bicycle_pm_theme.c
    ${BICYCLE_DIR}/src/bicycle_ui_ctl.c
    ${BICYCLE_DIR}/src/nav_test.c   # `ctl nav`：脱离 App 对比直规 / rolling
    ${BICYCLE_DIR}/src/bicycle_runtime.c
    ${BICYCLE_DIR}/src/bicycle_ride_gpx.c
    ${BICYCLE_DIR}/src/bicycle_env.c
    ${BICYCLE_DIR}/src/bicycle_demo.c   # 演示模式：四步循环（导航 / 跳点 / 换主题）
    ${BICYCLE_DIR}/src/bicycle_gpx_sim.c
    ${BICYCLE_DIR}/src/vmap/vmap_alloc.c
    ${BICYCLE_DIR}/src/vmap/vmap_vpk_heap.c
    ${BICYCLE_DIR}/src/vmap/vmap_cache_arena.c
    ${BICYCLE_DIR}/src/vmap/vmap_label.c
    ${BICYCLE_DIR}/src/vmap/vmap_track_arena.c
    ${BICYCLE_DIR}/src/vmap/vmap_track.c
    ${BICYCLE_DIR}/src/vmap/vmap_blit.c
    ${BICYCLE_DIR}/src/vmap/vmap_view.c
    ${BICYCLE_DIR}/src/vmap/vmap_render.c
    ${BICYCLE_DIR}/src/vmap/vmap_style.c
    ${BICYCLE_DIR}/src/vmap/vmap_tile.c
    ${BICYCLE_DIR}/src/vmap/vmap_tile_cache.c
    ${BICYCLE_DIR}/src/vmap/vmap_map_catalog.c
    ${BICYCLE_DIR}/src/vmap/vmap_grid.c
    ${BICYCLE_DIR}/src/vmap/vmap_tile_index.c
    ${BICYCLE_DIR}/src/vmap/vmap_tile_system.c
    ${BICYCLE_DIR}/src/vmap/vmap_geo.c
    ${BICYCLE_DIR}/src/vmap/vmap_route_graph.c
    ${BICYCLE_DIR}/src/vmap/vmap_route.c
    ${BICYCLE_DIR}/src/vmap/vmap_route_corridor.c
    ${BICYCLE_DIR}/src/vmap/vmap_route_portal.c
    ${BICYCLE_DIR}/src/vmap/vmap_route_trip.c
    ${BICYCLE_DIR}/src/vmap/vmap_route_nav.c
    ${BICYCLE_DIR}/src/vmap/vmap_route_worker.c
    ${LV_PM_DIR}/core/lv_pm_core.c
    ${LV_PM_DIR}/core/lv_pm_port.c
    ${LV_PM_DIR}/core/lv_pm_anima.c
    ${LV_PM_DIR}/core/lv_pm_i18n.c
    ${LV_PM_DIR}/i18n/lv_pm_i18n_strings.c
    ${LV_PM_DIR}/core/lv_pm_theme.c
    ${LV_PM_DIR}/core/lv_pm_theme_lvgl.c
    ${LV_PM_DIR}/core/lv_pm_bar.c
    ${LV_PM_DIR}/core/lv_pm_overlay.c
    ${LV_PM_DIR}/font/font_awesome_15.c
    ${LV_PM_DIR}/img/NaviArrow.c
    ${LV_PM_DIR}/img/img_src_navi_arrow_light.c
    ${LV_PM_DIR}/img/img_src_navi_arrow_dark.c
    ${LV_PM_DIR}/themes/lv_pm_themes.c
    ${LV_PM_DIR}/themes/lv_pm_theme_classic.c
    ${LV_PM_DIR}/themes/lv_pm_theme_outdoor.c
    ${LV_PM_DIR}/anima/lv_pm_no_anima.c
    ${LV_PM_DIR}/anima/lv_pm_slide_anima.c
    ${LV_PM_DIR}/anima/lv_pm_popup_anima.c
    ${LV_PM_DIR}/anima/lv_pm_fade_anima.c
    ${LV_PM_DIR}/anima/lv_pm_gpu_anima.c
    ${BICYCLE_DIR}/src/Vendor/Board/lv_port/lv_port_buttons.c
    ${GPX_DIR}/gpx_port.c
    ${GPX_DIR}/gpx_ext.c
    ${GPX_DIR}/gpx_ext_sensor.c
    ${GPX_DIR}/gpx_writer.c
    ${GPX_DIR}/gpx_record.c
    ${GPX_DIR}/gpx_reader.c
    ${GPX_DIR}/gpx_decode.c)
  file(GLOB FTEST_SRCS CONFIGURE_DEPENDS
    ${BICYCLE_DIR}/src/lvgl_page/fonts/ftest/ftest_*.c)
  list(APPEND CSRCS ${FTEST_SRCS})

  if(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE)
    list(APPEND CSRCS ${BICYCLE_DIR}/src/bicycle_inval_probe.c)
  endif()
  if(CONFIG_MYVENDOR_BICYCLE_HANDLER_STAT)
    list(APPEND CSRCS ${BICYCLE_DIR}/src/bicycle_handler_stat.c)
  endif()
  if(CONFIG_MYVENDOR_BICYCLE_C_DEBUG)
    list(APPEND CSRCS ${BICYCLE_DIR}/src/bicycle_c_debug.c)
  endif()
  if(CONFIG_MYVENDOR_FACTORY_MODE)
    list(APPEND CSRCS
      ${BICYCLE_DIR}/src/lvgl_page/factory/factory_page.c
      ${BICYCLE_DIR}/src/lvgl_page/factory/factory_board_test.c)
  else()
    list(APPEND CSRCS
      ${BICYCLE_DIR}/src/App/UI/Resource/FontVectorLfs.c)
  endif()

  if(NOT CONFIG_MYVENDOR_SYSTEM_VECTOR_FONT OR CONFIG_MYVENDOR_FACTORY_MODE)
    list(APPEND CSRCS
      ${NUTTX_BOARD_ABS_DIR}/domain/lvgl_font/myvendor_system_font_stub.c)
  endif()

  nuttx_add_application(
    NAME ${BICYCLE_PROGNAME}
    PRIORITY ${BICYCLE_PRIORITY}
    INCLUDE_DIRECTORIES ${INCDIR}
    STACKSIZE ${BICYCLE_STACKSIZE}
    SRCS ${MAINSRC} ${CSRCS}
    DEPENDS
    lvgl
    board)

  nuttx_add_application(
    NAME bicycle_nsh
    PRIORITY 100
    INCLUDE_DIRECTORIES ${INCDIR}
    # 8192：与 `ctl` 同一类（NSH 侧命令面 + 进 UI/nav 代码路径）。
    # 4096 那次教训见 nsh/CMakeLists.txt 顶部的"栈大小口径"一节。
    STACKSIZE 8192
    SRCS
      ${BICYCLE_DIR}/src/bicycle_nsh_main.c
      ${BICYCLE_DIR}/src/bicycle_nsh_ctl.c
    DEPENDS
    board)
endif()
