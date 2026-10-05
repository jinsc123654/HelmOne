# ##############################################################################
# vendor/my_vendor/boards/sf32lb52/my_vendor/vela_override/CMakeLists.txt
#
# Vela 抽换层：按文件名替换 OpenVela 上游编译单元，不改 nuttx / apps /
# frameworks / zblue git。往对应子目录丢同名 .c 即生效。
#
# 2026-09-25：把 patch_adapter_service.py 的 v3 改回来（日常断链的 DFX 上报
# ERROR→WARNING，DFX 事件照发），并降级 sal_gatt_client_interface.c 的
# `[UNSUBSCRIBED]` 与"未认领即释放"。本行同时起"真实编辑逼 reconfigure"的作用。
#
# 每个抽换件（*.c 与 patch_*.py）**文件头**都带：替的是哪个上游仓库/文件、写入时的 HEAD、
# 该上游文件在 HEAD 的 blob、为什么抽换、以及版本漂移自查命令（rev-parse/diff）。
# 本表只留一句话原因与它影响哪个 lib/target；要看全的打开文件头。
#
# 每个抽换件尾部还有两行"标记"（照抄即可，新增抽换件别漏）：
#   #pragma message("myvendor override compiled: vela_override/<rel> -- 上游 … -- 原因")
#   const char myvendor_override_marker_<sym>[] __attribute__((used,
#       section(".myvendor_marker"))) = "vela_override/<rel> -- 上游 … -- 原因";
#
# 编译期那一行**故意只用 note（info）**：note 关不掉，而 `#pragma GCC warning` 在量产构建里
# 会被 `-w` / 全局压制掉（2026-09-19 用户要求改成 info）；裸 `#warning` 更不行 —— 本 target 带
# `-Wno-cpp`，会被静默（实测一条都不出）。
#
# 段名必须是 `.myvendor_marker`，不能写 `.rodata`：这类常量没人引用，`.rodata` 会被链接期
# --gc-sections 当垃圾段丢掉。板级链接脚本 scripts/ld.script 里对它做了
# `KEEP(*(.myvendor_marker))`，所以它稳稳进镜像（只占 flash，不占 SRAM）。
# 查"镜像里到底有哪些抽换件"：
#   strings -a cmake_out/<cfg>/nuttx.bin | grep '^vela_override/'      # 裸 bin 里就有
#
#   bluetooth/   → libbluetooth
#     scan_manager.c               LOCAL 下不编 bt_socket.h
#     sal_gatt_client_interface.c  keep_db / 溢出不拆链
#     sal_gatt_server_interface.c  CCC 写回调按写入字节/peer cfg 上报（不用 conn_index）
#     sal_le_advertise_interface.c SF32 走 legacy bt_le_adv_start
#     sal_le_scan_interface.c      -EALREADY 重试、options=0
#     patch_adapter_service.py     加密状态变化 BLE 分支 NULL 设备保护（find_create + guard）
#     patch_adapter_state.py       adapter turn-on 失败退回 off_state（HSM 没有失败出口 ⇒ 永久死态）
#     patch_log_server.py          DIRECT 模式关掉 property monitor（那三个符号只在 client 模式编译 ⇒ 否则开了日志就链接失败）
#     patch_hci_h4.py              RX 取包不再阻塞服务线程（K_FOREVER→K_NO_WAIT + 不可丢包退帧重试）；补 ext adv 到可丢集合；h4_open 例行信息 ERROR→INFO
#   fs/          → lib fs
#     fs_procfs_mount.c            df -h 用 uint64，避免 >=4GiB wrap
#     fs_fat32util.c               FatSz16==0 当 FAT32（大簇 1GiB 卷）
#   freetype/    → lib freetype
#     ftsystem.c                   FT heap 走 PSRAM（不改 apps/external/freetype）
#   mtd/         → lib drivers
#     ftl.c                        O_DIRECT 写成功返回扇区数（不是 leftover 0）
#   usbdev/      → lib drivers
#     mtp.c                        iProduct / iSerialNumber 可写（MAC 后缀）
#   zblue/       → lib zblue
#     init.c            z_sys_init 幂等（SAL BREDR+LE 都可能调用）
#     system_work_q.c   禁止第二条 sysworkq 共用 BSS 栈
#     long_wq.c         禁止第二条 BT LW WQ 共用 BSS 栈
#     patch_hci_core.py 超时返回 -ETIMEDOUT；skip_sync 轮询退出不可中断等待
#     patch_gatt_ccc_pool.py 断开释放 CCC 槽 + 池满时回收已断开的 peer；CCC 写/复位原始字节取证；地址对不上不误关 CCC
#     patch_net_buf_timeout.py 缓冲区 K_FOREVER 改成有上限的轮询（否则卡死不可杀）
#     patch_conn_ready_null.py get_conn_ready 判空 + DISCONNECTED 时摘 conn_ready 节点
#     （预留 id.c 扫描 -EACCES）
#   sched/       → lib sched
#     wd_start.c                 PSRAM waitdog 视为合法；断链才剪环，避免 wd_insert HardFault
#     wd_cancel.c                未入链不 list_delete
#     sem_waitirq.c              waitobj==NULL 记 schedmon，不 DEBUGASSERT
#     sem_post.c                 mutex holder≠tid 记 schedmon，不 DEBUGASSERT
#     sched_removereadytorun.c   UP flink==NULL 拼回 IDLE，记 schedmon，不 DEBUGASSERT
#
# DEFER 执行时 CMAKE_CURRENT_LIST_DIR 已是 nuttx，路径必须先存下来。
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

set(MYVENDOR_BT_OVERRIDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/bluetooth"
    CACHE INTERNAL "Board replacements for libbluetooth .c files")
set(MYVENDOR_FS_OVERRIDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/fs"
    CACHE INTERNAL "Board replacements for lib fs .c files")
set(MYVENDOR_MTD_OVERRIDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/mtd"
    CACHE INTERNAL "Board replacements for drivers/mtd .c files")
set(MYVENDOR_USBDEV_OVERRIDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/usbdev"
    CACHE INTERNAL "Board replacements for drivers/usbdev .c files")
set(MYVENDOR_ZBLUE_OVERRIDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/zblue"
    CACHE INTERNAL "Board replacements for zblue .c files")
set(MYVENDOR_FT_OVERRIDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/freetype"
    CACHE INTERNAL "Board replacements for freetype .c files")
set(MYVENDOR_SCHED_OVERRIDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/sched"
    CACHE INTERNAL "Board replacements for libsched .c files")
set(MYVENDOR_BOARD_INCLUDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../include"
    CACHE INTERNAL "Board include dir for driver overrides")

function(myvendor_patch_target_by_name target ov_dir)
  if(NOT TARGET ${target})
    return()
  endif()

  if(NOT EXISTS "${ov_dir}")
    return()
  endif()

  file(GLOB _ov_files "${ov_dir}/*.c")
  if(NOT _ov_files)
    return()
  endif()

  set(_ov_names)
  foreach(_f IN LISTS _ov_files)
    get_filename_component(_n "${_f}" NAME)
    list(APPEND _ov_names "${_n}")
  endforeach()

  get_target_property(_srcs ${target} SOURCES)
  if(NOT _srcs)
    return()
  endif()

  set(_kept)
  set(_used)
  foreach(_src IN LISTS _srcs)
    get_filename_component(_n "${_src}" NAME)
    if(_n IN_LIST _ov_names)
      # 同名文件在这个 target 里不止一个时，两个都会被摘掉、只补一份板级
      # 版本 —— 另一个的符号会变成链接期 undefined reference（zblue 的
      # buf.c 踩过）。既去重也把嗓门放大。
      if(_n IN_LIST _used)
        message(WARNING
                "my_vendor: ${target} has more than one ${_n}; every copy is "
                "replaced by the board file in ${ov_dir}")
      else()
        list(APPEND _used "${_n}")
      endif()
    else()
      list(APPEND _kept "${_src}")
    endif()
  endforeach()

  if(NOT _used)
    message(WARNING "my_vendor: ${ov_dir} .c not in ${target}")
    return()
  endif()

  set_property(TARGET ${target} PROPERTY SOURCES ${_kept})
  foreach(_n IN LISTS _used)
    target_sources(${target} PRIVATE "${ov_dir}/${_n}")
    message(STATUS "my_vendor: ${target} uses board ${_n}")
  endforeach()

  foreach(_n IN LISTS _ov_names)
    if(NOT _n IN_LIST _used)
      message(WARNING "my_vendor: override ${_n} not in ${target}, skipped")
    endif()
  endforeach()
endfunction()

# 用 patch 脚本改写一个 zblue 上游 .c，然后编译改写后的副本（上游 git 不动）。
# 适合 gatt.c / hci_core.c 这类几千行、不方便整个塞进 git 的文件。
#
# @param _name    上游 .c 的文件名
# @param _script  MYVENDOR_ZBLUE_OVERRIDE_DIR 下的 patch 脚本
# @param _desc    成功时的说明（阶段 3 消息）
# @param _subdir  相对 zblue/subsys/bluetooth/ 的目录，缺省 host/
function(myvendor_patch_zblue_via_script _name _script _desc)
  if(NOT TARGET zblue)
    return()
  endif()
  if(NOT DEFINED NUTTX_APPS_DIR)
    return()
  endif()

  set(_subdir "subsys/bluetooth/host")
  if(ARGC GREATER 3)
    set(_subdir "${ARGV3}")
  endif()

  set(_src "${NUTTX_APPS_DIR}/external/zblue/zblue/${_subdir}/${_name}")
  if(NOT EXISTS "${_src}")
    message(WARNING "my_vendor: ${_name} not found, skip ${_desc}")
    return()
  endif()

  # 改写后的副本放在 build 目录：${CMAKE_BINARY_DIR}/myvendor_zblue/myvendor_<name>
  #
  # 前缀不是装饰：静态库成员名取的是 basename，`buf.c` 这种重名文件会让
  # `libzblue.a` 里出现两个 `buf.c.o` 成员。另外日志/反汇编里
  # `myvendor_buf.c` 一眼就是"打过补丁的那份"，比裸 `buf.c` 好认。
  set(_dst "${CMAKE_BINARY_DIR}/myvendor_zblue/myvendor_${_name}")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/myvendor_zblue")
  execute_process(
    COMMAND python3 "${MYVENDOR_ZBLUE_OVERRIDE_DIR}/${_script}" "${_src}"
            "${_dst}"
    RESULT_VARIABLE _r
    ERROR_VARIABLE _e)
  if(NOT _r EQUAL 0)
    message(WARNING "my_vendor: ${_desc} patch failed: ${_e}")
    return()
  endif()

  get_target_property(_srcs zblue SOURCES)
  if(NOT _srcs)
    return()
  endif()

  # 按**路径后缀**摘源，不能按 basename。
  #
  # zblue 里重名文件不止一处：`buf.c` 同时有
  # `subsys/bluetooth/host/buf.c`（BT 缓冲池、bt_buf_get_rx / hci_rx_pool /
  # bt_buf_make_view…）和 `port/lib/net_buf/buf.c`（通用 net_buf，本补丁要改的
  # 那个）。按文件名匹配会把宿主那份一起删掉，现场表现是链接期一堆
  # `undefined reference to bt_buf_get_rx / hci_rx_pool / discardable_pool`。
  set(_suffix "/${_subdir}/${_name}")
  string(LENGTH "${_suffix}" _suflen)

  set(_kept)
  set(_hit FALSE)
  foreach(_s IN LISTS _srcs)
    string(LENGTH "${_s}" _slen)
    math(EXPR _want "${_slen} - ${_suflen}")
    string(FIND "${_s}" "${_suffix}" _pos)
    # `_pos == -1` 表示没找到；而 `_want` 在**条目比后缀短一个字符**时也是 -1，
    # 只判 `EQUAL` 就会把无关源文件当成命中摘掉：`zblue/port/kernel/mem_slab.c`
    # 与 `zblue/port/lib/net_buf/buf.c` 都是 28 字符，正好撞上 29 字符的
    # `/subsys/bluetooth/host/gatt.c`。现场表现是这两个文件从 ninja 图里消失
    # （k_mem_slab_* / net_buf_* 链接期 undefined），而真正要改的 buf.c 那次
    # 因为条目已被误删、`_hit` 变 FALSE 直接 return，连副本都没加。
    if(_pos GREATER -1 AND _pos EQUAL _want)
      list(APPEND _kept "${_dst}")
      set(_hit TRUE)
    else()
      list(APPEND _kept "${_s}")
    endif()
  endforeach()
  if(NOT _hit)
    message(WARNING "my_vendor: zblue has no ${_name}, skip ${_desc}")
    return()
  endif()

  set_property(TARGET zblue PROPERTY SOURCES ${_kept})
  # 副本不在上游目录里，同目录的 *_internal.h 需要显式 -I
  set_property(
    SOURCE "${_dst}"
    APPEND
    PROPERTY COMPILE_OPTIONS
             "-I${NUTTX_APPS_DIR}/external/zblue/zblue/${_subdir}")
  message(STATUS "my_vendor: ${_desc}")
endfunction()

function(myvendor_patch_zblue_hci_timeout)
  myvendor_patch_zblue_via_script(
    hci_core.c
    patch_hci_core.py
    "zblue HCI timeout returns error (no assert)")
endfunction()

# CCC 池只有 BT_MAX_PAIRED + BT_MAX_CONN 个槽且按 peer 地址索引，断开时若没有
# 释放（bonded peer，或 ATT teardown 没跑到），手机每换一次 RPA 就吃掉一个槽，
# 池满后所有 CCC 写都被拒（ATT 0x11 INSUFFICIENT_RESOURCES）——App 侧表现就是
# 「连上了但一直未连接」。见 patch_gatt_ccc_pool.py。
#
# 同一个脚本还加 GATT 上下文池哨兵 + 回调链表校验：2026-09-18 的 panic
# （MemManage DACCVIOL, mmfar=0xfee7fee7, arm_memfault.c:136）就是那张链表被
# 外部写坏之后，被 adapter cycle 里的 bt_sal_gatt_client_disable() 走了进去。
# 现在走之前先验指针，坏了记一行 ERROR 并重建链表，不再 panic。
function(myvendor_patch_zblue_gatt_ccc)
  myvendor_patch_zblue_via_script(
    gatt.c
    patch_gatt_ccc_pool.py
    "zblue GATT: CCC pool reclaim + ctx canary + cb list guard")
endfunction()

# net_buf_alloc_len() 的 K_FOREVER 在池子被抽干时是**不可中断等待**：
# SIGTERM / SIGKILL / task_delete 都打不断它（task_delete 甚至返回成功），
# 于是 companion 卡死在 bt_gatts_notify 上、LCPU 永远不复位、BLE 到重启为止
# 都回不来。改成 K_NO_WAIT 轮询 + 上限，并在 LCPU 恢复期间提前放弃。
# 见 patch_net_buf_timeout.py。
function(myvendor_patch_zblue_net_buf_timeout)
  myvendor_patch_zblue_via_script(
    buf.c
    patch_net_buf_timeout.py
    "zblue net_buf alloc can not wait forever"
    "port/lib/net_buf")
endfunction()

# 配对密钥（含 IRK/LTK）落盘：settings 关着 ⇒ zblue 的 bt_keys_store() 是空动作
# （keys.h 里退化成 `return 0` 的 inline），重启就把设备自己的身份/密钥丢光
# （手机还记着旧的 → 连不上/服务发现走不动）。
# 补丁两处（同一个脚本，按文件名分派）：
#   keys.c 只搬字节的 KV 读写 + 惰性恢复 + 解绑/清池钩子；
#   smp.c  配对完成处整池落盘 —— 保存钩子必须挂在这条总会跑的路径上，
#          挂 bt_keys_store() 是死路（那函数只在 CONFIG_SETTINGS 打开时才有实体）。
function(myvendor_patch_zblue_keys_kv)
  myvendor_patch_zblue_via_script(
    keys.c
    patch_keys_kv.py
    "zblue bt_keys persist to /mnt/kv/bt_keys.bin (keys.c)")
  myvendor_patch_zblue_via_script(
    smp.c
    patch_keys_kv.py
    "zblue bt_keys persist to /mnt/kv/bt_keys.bin (smp.c save hook)")
endfunction()

# get_conn_ready() 对 conn_ready 链表 peek_head→CONTAINER_OF，原先没有判空；
# 且 DISCONNECTED 时不摘节点 ⇒ TX 消费前断开会留下指着已清零 conn 的 stale 节点，
# 下次 peek 就是空指针 + 成员偏移（现场 cfsr=0x82 bfar=0x124，ble_companion panic）。
# 见 patch_conn_ready_null.py。
function(myvendor_patch_zblue_conn_ready)
  myvendor_patch_zblue_via_script(
    conn.c
    patch_conn_ready_null.py
    "zblue conn_ready: NULL guard + unlink on disconnect")
endfunction()

# scan.c 有四处 `hdev->scan_ctx` 裸解引用（适配器拆栈 / HCI 复位未回的窗口里它是
# NULL）：bt_scan_softreset（2026-09-18 现场 panic 的那处）、bt_le_scan_stop_mc
# 尾部、以及两个同族入口 bt_le_scan_user_add / user_remove。2026-09-18 先就地改过
# 前两处；2026-09-24 全部收进脚本（上游 git 回到不动状态）。
# 见 patch_scan_stop_null.py 与 docs/pitch/zblue-scan-stop-null-ctx.md。
function(myvendor_patch_zblue_scan_null)
  myvendor_patch_zblue_via_script(
    scan.c
    patch_scan_stop_null.py
    "zblue scan: 4x scan_ctx NULL guard (softreset/stop_mc/user_add/user_remove)")
endfunction()

# id.c：`LE Set Random Address` 在外设链路已建立时会被控制器拒绝（-EACCES），
# 上游只对"扫描中/发起中"放行 ⇒ 手机连着时观测者起不来（一帧都收不到）。
# 放宽为"一律保留当前身份地址继续扫"。见 patch_id_scan_addr.py。
function(myvendor_patch_zblue_id_scan_addr)
  myvendor_patch_zblue_via_script(
    id.c
    patch_id_scan_addr.py
    "zblue id: tolerate -EACCES from Set Random Address (keep identity)")
endfunction()

function(myvendor_patch_upstream_sources)
  myvendor_patch_target_by_name(libbluetooth "${MYVENDOR_BT_OVERRIDE_DIR}")

  # 加密状态变化处理器（adapter_service.c）里 BLE 分支缺 NULL 保护：framework 设备表
  # 里还没有该地址时 `device_set_connection_state(NULL,…)` 直接 MemManage fault
  # （现场 cfsr=0x82 mmfar=0xab，pc=device_is_connected）。密钥落盘生效后手机拿旧绑定
  # 重连并加密就会走到 —— 见 patch_adapter_service.py 头注释（含 coredump 现场）。
  # 同 usbdev_fs.c 的套路：配置期脚本定点改写 → 生成到构建目录 → 按文件名顶掉。
  set(MYVENDOR_BT_GEN_DIR "${CMAKE_BINARY_DIR}/myvendor_bt")
  file(MAKE_DIRECTORY "${MYVENDOR_BT_GEN_DIR}")
  if(NOT DEFINED NUTTX_APPS_DIR)
    message(WARNING "my_vendor: NUTTX_APPS_DIR not set, skip adapter_service.c patch")
  else()
    execute_process(
      COMMAND python3 "${MYVENDOR_BT_OVERRIDE_DIR}/patch_adapter_service.py"
              "${NUTTX_APPS_DIR}/frameworks/connectivity/bluetooth/service/src/adapter_service.c"
              "${MYVENDOR_BT_GEN_DIR}/adapter_service.c"
      RESULT_VARIABLE _as_r
      ERROR_VARIABLE _as_e)
    if(NOT _as_r EQUAL 0)
      message(WARNING "my_vendor: adapter_service.c patch failed: ${_as_e}")
    else()
      myvendor_patch_target_by_name(libbluetooth "${MYVENDOR_BT_GEN_DIR}")
    endif()
  endif()

  # adapter 状态机（adapter_state.c）在「turn on 失败」这条路上**没有失败出口**：
  # ble_turning_on_enter() 里 bt_sal_le_enable() 失败时不做任何迁移 ⇒ HSM 永久
  # 停在 ble_turning_on_state（对外状态却是 OFF），而该状态只认 BLE_ENABLED /
  # BLE_PROFILE_ENABLED（都需要 zblue 的 ready 回调 —— bt_enable() 已经失败了），
  # 状态表又是扁平的 ⇒ 再 enable 也回不去。现场就是每 ~76 s 一轮的
  # companion respawn 死循环（LCPU force reset / task restart 都救不回来，
  # 因为它们都不碰这台 HSM）。见 patch_adapter_state.py 头注释。
  if(NOT DEFINED NUTTX_APPS_DIR)
    message(WARNING "my_vendor: NUTTX_APPS_DIR not set, skip adapter_state.c patch")
  else()
    execute_process(
      COMMAND python3 "${MYVENDOR_BT_OVERRIDE_DIR}/patch_adapter_state.py"
              "${NUTTX_APPS_DIR}/frameworks/connectivity/bluetooth/service/src/adapter_state.c"
              "${MYVENDOR_BT_GEN_DIR}/adapter_state.c"
      RESULT_VARIABLE _st_r
      ERROR_VARIABLE _st_e)
    if(NOT _st_r EQUAL 0)
      message(WARNING "my_vendor: adapter_state.c patch failed: ${_st_e}")
    else()
      myvendor_patch_target_by_name(libbluetooth "${MYVENDOR_BT_GEN_DIR}")
    endif()
  endif()

  # log_server.c：framework 的 BT_LOG* 实现。它碰 property_monitor_* 的三处只判了
  # CONFIG_KVDB + __NuttX__，**漏判 CONFIG_KVDB_DIRECT** —— 而 KVDB 的 CMake 只在
  # **非** DIRECT 模式编译提供那三个符号的 kvdb/client.c。本板是 DIRECT ⇒ 三个
  # undefined reference。平时不炸是因为不开日志时 BT_LOG* 是空宏、没人引用
  # bt_log_print_check() ⇒ log_server.o 压根不被链接器拉进来。
  # 见 patch_log_server.py 头注释（含 CMakeLists 行号与 kvdb.h 行号）。
  if(NOT DEFINED NUTTX_APPS_DIR)
    message(WARNING "my_vendor: NUTTX_APPS_DIR not set, skip log_server.c patch")
  else()
    execute_process(
      COMMAND python3 "${MYVENDOR_BT_OVERRIDE_DIR}/patch_log_server.py"
              "${NUTTX_APPS_DIR}/frameworks/connectivity/bluetooth/service/utils/log_server.c"
              "${MYVENDOR_BT_GEN_DIR}/log_server.c"
      RESULT_VARIABLE _ls_r
      ERROR_VARIABLE _ls_e)
    if(NOT _ls_r EQUAL 0)
      message(WARNING "my_vendor: log_server.c patch failed: ${_ls_e}")
    else()
      # 生成的副本在构建目录里，**同目录的头**就找不到了：log_server.c 里
      # 用引号形式 include 了同目录的 `btsnoop_log.h` 和 `log.h`（引号形式先搜
      # "包含者所在目录"），而副本不在那儿。不补的现场是
      #   myvendor_bt/log_server.c:27:10: fatal error: btsnoop_log.h: No such file
      #
      # 用 target_include_directories（**target 作用域**）而不是
      # set_property(SOURCE … COMPILE_OPTIONS …)：后者是本板目录作用域下写的，
      # 对 add_subdirectory 进来的 libbluetooth 不生效 —— 实测
      # compile_commands.json 里那条命令**没有**多出这个 -I（已验），
      # 而 target 作用域一定生效。
      #
      # 追加在末尾（PRIVATE + 追加语义）：**不改变任何既有 include 的解析顺序**，
      # 只是给这两个头多开一条路。service/utils 里有 `log.h` 这种通用名，
      # 所以这一点很重要 —— 排在最后就不会顶掉别人的同名头。
      # （同 myvendor_patch_zblue_via_script 给 zblue 副本补 -I 的意图。）
      target_include_directories(
        libbluetooth
        PRIVATE
          "${NUTTX_APPS_DIR}/frameworks/connectivity/bluetooth/service/utils")
      myvendor_patch_target_by_name(libbluetooth "${MYVENDOR_BT_GEN_DIR}")
    endif()
  endif()

  # hci_h4.c：zblue 的 H4 传输 + 字符设备 poll 回调。RX 取包原来对非可丢事件与 ACL
  # 传 K_FOREVER ⇒ 拿不到 net_buf 就把**整条 BT 服务线程**（所有 SAL/GATT/连接管理）
  # 钉住，而且超时后是丢包（高优先级事件丢一条会让流控与状态永久错位）。
  # 改成 K_NO_WAIT + 不可丢包退帧重试，并补上扩展广播报告到可丢集合
  # （SDK 2.4 的 commit 8a1103b 有这条）。见 patch_hci_h4.py 头注释。
  #
  # 注：这个文件在写入前**已经被人在原地改过**（h4_rx_reopen 那一段）。本补丁只碰
  # get_rx 与调用点，不去动它；走脚本是为了上游重新同步时改动不会丢。
  if(NOT DEFINED NUTTX_APPS_DIR)
    message(WARNING "my_vendor: NUTTX_APPS_DIR not set, skip hci_h4.c patch")
  else()
    execute_process(
      COMMAND python3 "${MYVENDOR_BT_OVERRIDE_DIR}/patch_hci_h4.py"
              "${NUTTX_APPS_DIR}/frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c"
              "${MYVENDOR_BT_GEN_DIR}/hci_h4.c"
      RESULT_VARIABLE _h4_r
      ERROR_VARIABLE _h4_e)
    if(NOT _h4_r EQUAL 0)
      message(WARNING "my_vendor: hci_h4.c patch failed: ${_h4_e}")
    else()
      # 不需要额外 -I：它 include 的 "hci_h4.h" 在
      # service/stacks/zephyr/include，那个目录本来就在 libbluetooth 的 include 路径上。
      myvendor_patch_target_by_name(libbluetooth "${MYVENDOR_BT_GEN_DIR}")
    endif()
  endif()

  myvendor_patch_target_by_name(fs "${MYVENDOR_FS_OVERRIDE_DIR}")
  if(TARGET fs)
    # override fs_fat32util.c: "fs_fat32.h" lives next to the upstream .c
    target_include_directories(fs PRIVATE "${CMAKE_SOURCE_DIR}/fs/fat")
  endif()
  myvendor_patch_target_by_name(zblue "${MYVENDOR_ZBLUE_OVERRIDE_DIR}")
  myvendor_patch_zblue_hci_timeout()
  myvendor_patch_zblue_gatt_ccc()
  myvendor_patch_zblue_net_buf_timeout()
  myvendor_patch_zblue_keys_kv()
  myvendor_patch_zblue_conn_ready()
  myvendor_patch_zblue_scan_null()
  myvendor_patch_zblue_id_scan_addr()
  myvendor_patch_target_by_name(sched "${MYVENDOR_SCHED_OVERRIDE_DIR}")
  if(TARGET sched)
    target_include_directories(sched PRIVATE "${MYVENDOR_BOARD_INCLUDE_DIR}")
  endif()
  myvendor_patch_target_by_name(drivers "${MYVENDOR_MTD_OVERRIDE_DIR}")

  # usbdev_fs.c（1686 行）只改一处，整份带不划算 —— 配置期脚本定点改写，
  # 生成到构建目录，再按文件名顶掉 drivers 里的那份（同 zblue 三个脚本的套路）。
  # 见 vela_override/usbdev/patch_usbdev_fs.py 的头注释（现场/机制/上游 id）。
  set(MYVENDOR_USBDEV_GEN_DIR "${CMAKE_BINARY_DIR}/myvendor_usbdev")
  file(MAKE_DIRECTORY "${MYVENDOR_USBDEV_GEN_DIR}")
  execute_process(
    COMMAND python3 "${MYVENDOR_USBDEV_OVERRIDE_DIR}/patch_usbdev_fs.py"
            "${CMAKE_SOURCE_DIR}/drivers/usbdev/usbdev_fs.c"
            "${MYVENDOR_USBDEV_GEN_DIR}/usbdev_fs.c"
    RESULT_VARIABLE _uf_r
    ERROR_VARIABLE _uf_e)
  if(NOT _uf_r EQUAL 0)
    message(WARNING "my_vendor: usbdev_fs.c patch failed: ${_uf_e}")
  else()
    myvendor_patch_target_by_name(drivers "${MYVENDOR_USBDEV_GEN_DIR}")
  endif()

  if(CONFIG_USBMTP)
    myvendor_patch_target_by_name(drivers "${MYVENDOR_USBDEV_OVERRIDE_DIR}")
    if(TARGET drivers)
      # override mtp.c: "usbdev_fs.h" + "myvendor_identity.h"
      target_include_directories(drivers PRIVATE
        "${CMAKE_SOURCE_DIR}/drivers/usbdev"
        "${MYVENDOR_BOARD_INCLUDE_DIR}")
    endif()
  endif()
  if(CONFIG_LIB_FREETYPE AND CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC)
    myvendor_patch_target_by_name(freetype "${MYVENDOR_FT_OVERRIDE_DIR}")
    if(TARGET freetype)
      # override ftsystem.c: "board_malloc.h" + upstream "mbfc.h"
      target_include_directories(
        freetype PRIVATE "${MYVENDOR_BOARD_INCLUDE_DIR}"
                         "${NUTTX_APPS_DIR}/external/freetype/freetype/builds/nuttx")
    endif()
  endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
                 myvendor_patch_upstream_sources)
