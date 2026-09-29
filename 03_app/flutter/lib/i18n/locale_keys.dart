/// 文案键常量。UI 中使用 `LocaleKeys.xxx.tr`，勿直接写死多语言字符串。
abstract class LocaleKeys {
  // —— 通用 ——
  static const appName = 'app_name';
  static const language = 'language';
  static const langZh = 'lang_zh';
  static const langEn = 'lang_en';
  static const refresh = 'refresh';
  static const neverWritten = 'never_written';
  static const updatedAt = 'updated_at';
  static const cancel = 'cancel';
  static const confirm = 'confirm';

  /// 产品界面通用失败（不带异常原文/错误码）。
  static const failed = 'failed';

  // —— 欢迎页 ——
  static const welcomeSubtitle = 'welcome_subtitle';
  static const welcomePrivacyBody = 'welcome_privacy_body';
  static const welcomeAgreeCheck = 'welcome_agree_check';
  static const welcomeEnter = 'welcome_enter';

  // —— 首页 ——
  static const homeKvTitle = 'home_kv_title';
  static const homeKvBump = 'home_kv_bump';
  static const homeKvSnackbar = 'home_kv_snackbar';
  static const homeDbTitle = 'home_db_title';
  static const homeDbBump = 'home_db_bump';
  static const homeDbSnackbar = 'home_db_snackbar';
  static const homeBleTitle = 'home_ble_title';
  static const homeBleScan = 'home_ble_scan';
  static const homeRefreshPersist = 'home_refresh_persist';
  static const homeBlePermissionDenied = 'home_ble_permission_denied';
  static const homeOpenLogs = 'home_open_logs';

  /// 首页进入 Companion 协议联调页的按钮。
  static const homeOpenCompanionDebug = 'home_open_companion_debug';

  /// 首页进入固件 OTA 页。
  static const homeOpenOta = 'home_open_ota';

  /// 首页进入星历同步页。
  static const homeOpenEph = 'home_open_eph';

  /// 星历同步页。
  static const ephTitle = 'eph_title';
  static const ephNeedBle = 'eph_need_ble';
  static const ephMtpBusy = 'eph_mtp_busy';
  static const ephHint = 'eph_hint';
  static const ephSync = 'eph_sync';
  static const ephDownloading = 'eph_downloading';
  static const ephConverting = 'eph_converting';
  static const ephUploading = 'eph_uploading';
  static const ephProgress = 'eph_progress';
  static const ephDone = 'eph_done';
  static const ephLastSync = 'eph_last_sync';
  static const ephNever = 'eph_never';
  static const ephDetails = 'eph_details';
  static const ephDownloadAction = 'eph_download_action';
  static const ephConvertAction = 'eph_convert_action';
  static const ephPushAction = 'eph_push_action';
  static const ephFile = 'eph_file';
  static const ephFrames = 'eph_frames';
  static const ephNextOnConnect = 'eph_next_on_connect';
  static const ephNextNone = 'eph_next_none';
  static const ephNextDue = 'eph_next_due';
  static const ephNextSyncing = 'eph_next_syncing';
  static const ephNextInMin = 'eph_next_in_min';
  static const ephNextInHours = 'eph_next_in_hours';
  static const ephNextInHoursMin = 'eph_next_in_hours_min';

  // —— 消息通知 ——
  static const notifTitle = 'notif_title';
  static const notifMaster = 'notif_master';
  static const notifHelpHint = 'notif_help_hint';
  static const notifHelpLink = 'notif_help_link';
  static const notifLockOnly = 'notif_lock_only';
  static const notifLockOnlySub = 'notif_lock_only_sub';
  static const notifRideOnly = 'notif_ride_only';
  static const notifRideOnlySub = 'notif_ride_only_sub';
  static const notifModeHint = 'notif_mode_hint';
  static const notifModeSync = 'notif_mode_sync';
  static const notifModeCustom = 'notif_mode_custom';
  static const notifNeedPermission = 'notif_need_permission';
  static const notifNeedToggle = 'notif_need_toggle';
  static const notifGoAuthorize = 'notif_go_authorize';
  static const notifHelpTitle = 'notif_help_title';
  static const notifHelpBody = 'notif_help_body';
  static const notifStaleCacheTitle = 'notif_stale_cache_title';
  static const notifStaleCacheBody = 'notif_stale_cache_body';
  static const notifStaleCacheButton = 'notif_stale_cache_button';
  static const notifStaleCacheOk = 'notif_stale_cache_ok';
  static const notifStaleCacheStill = 'notif_stale_cache_still';
  static const notifAppSearch = 'notif_app_search';
  static const notifAppEmpty = 'notif_app_empty';
  static const notifUploadIcons = 'notif_upload_icons';
  static const notifUploadIconsSub = 'notif_upload_icons_sub';
  static const notifUploadNeedBle = 'notif_upload_need_ble';
  static const notifUploadMtpBusy = 'notif_upload_mtp_busy';
  static const notifUploadProgress = 'notif_upload_progress';
  static const notifUploadDone = 'notif_upload_done';
  static const notifUploadNone = 'notif_upload_none';
  static const notifAppSms = 'notif_app_sms';
  static const notifAppWechat = 'notif_app_wechat';
  static const notifAppAlipay = 'notif_app_alipay';
  static const notifAppBaiduMap = 'notif_app_baidu_map';
  static const notifAppDingTalk = 'notif_app_dingtalk';
  static const notifAppDidi = 'notif_app_didi';
  static const notifAppMeituan = 'notif_app_meituan';
  static const notifAppQq = 'notif_app_qq';
  static const notifAppQqMail = 'notif_app_qqmail';
  static const homeOpenMap = 'home_open_map';
  static const bleSupported = 'ble_supported';
  static const bleUnsupported = 'ble_unsupported';

  // —— 蓝牙扫描 ——
  static const bleScanTitle = 'ble_scan_title';
  static const bleScanFilterTitle = 'ble_scan_filter_title';
  static const bleScanFilterSubtitle = 'ble_scan_filter_subtitle';
  static const bleScanPermissionDenied = 'ble_scan_permission_denied';
  static const bleScanTurnOnBluetooth = 'ble_scan_turn_on_bluetooth';
  static const bleScanStartFailed = 'ble_scan_start_failed';
  static const bleScanUnknownDevice = 'ble_scan_unknown_device';
  static const bleScanEmptyScanning = 'ble_scan_empty_scanning';
  static const bleScanEmptyIdle = 'ble_scan_empty_idle';
  static const bleScanConnect = 'ble_scan_connect';
  static const bleScanStop = 'ble_scan_stop';
  static const bleScanRescan = 'ble_scan_rescan';
  static const bleScanConnected = 'ble_scan_connected';
  static const bleScanConnectFailed = 'ble_scan_connect_failed';
  static const bleErrNoResponse = 'ble_err_no_response';
  static const bleErrFailed = 'ble_err_failed';
  static const bleErrRetrying = 'ble_err_retrying';
  static const bleErrNeedPair = 'ble_err_need_pair';
  static const pairGuideTitle = 'pair_guide_title';
  static const pairGuideStep1 = 'pair_guide_step1';
  static const pairGuideStep2 = 'pair_guide_step2';
  static const pairGuideStep3 = 'pair_guide_step3';
  static const pairGuideNote = 'pair_guide_note';
  static const bleScanPairHint = 'ble_scan_pair_hint';
  static const rebindTitle = 'rebind_title';
  static const rebindSub = 'rebind_sub';
  static const rebindBoundHint = 'rebind_bound_hint';
  static const rebindFreeHint = 'rebind_free_hint';
  static const rebindWhy = 'rebind_why';
  static const rebindPairStep3 = 'rebind_pair_step3';
  static const rebindUnbind = 'rebind_unbind';
  static const rebindConfirmTitle = 'rebind_confirm_title';
  static const rebindConfirmBody = 'rebind_confirm_body';
  static const rebindUnbound = 'rebind_unbound';
  static const rebindNeedUnbind = 'rebind_need_unbind';
  static const rebindScan = 'rebind_scan';
  static const deviceReconnecting = 'device_reconnecting';
  static const deviceBoundHint = 'device_bound_hint';
  static const connPickTitle = 'conn_pick_title';
  static const connScanEmpty = 'conn_scan_empty';
  static const connRetry = 'conn_retry';
  static const connFound = 'conn_found';
  static const connSearching = 'conn_searching';
  static const connSearchHint = 'conn_search_hint';
  static const bleSnackbarTitle = 'ble_snackbar_title';

  // —— 日志 ——
  static const logTitle = 'log_title';
  static const logDir = 'log_dir';
  static const logEmpty = 'log_empty';
  static const logPickHint = 'log_pick_hint';
  static const logClear = 'log_clear';
  static const logClearConfirmTitle = 'log_clear_confirm_title';
  static const logClearConfirmBody = 'log_clear_confirm_body';
  static const logWriteSampleCrash = 'log_write_sample_crash';
  static const logSampleCrashDone = 'log_sample_crash_done';
  static const logExportAll = 'log_export_all';
  static const logExportSelected = 'log_export_selected';
  static const logExportSubject = 'log_export_subject';
  static const logExportFailed = 'log_export_failed';

  // —— 日志文本查看器（本机日志 + 码表抓取共用）——
  static const logPreviewEmpty = 'log_preview_empty';
  static const logPreviewTruncated = 'log_preview_truncated';
  static const logPreviewTruncatedHead = 'log_preview_truncated_head';
  static const logFileGone = 'log_file_gone';

  // —— 本机日志页分区与操作 ——
  static const logSectionCrash = 'log_section_crash';
  static const logSectionApp = 'log_section_app';
  static const logCount = 'log_count';
  static const logDelete = 'log_delete';
  static const logDeleteConfirm = 'log_delete_confirm';
  static const logMore = 'log_more';
  static const logTotal = 'log_total';

  // —— 文件浏览器（缓存页 / 码表文件管理）——
  static const pathUp = 'path_up';
  static const actionMore = 'action_more';
  static const actionPreview = 'action_preview';
  static const actionShare = 'action_share';
  static const actionRename = 'action_rename';
  static const actionDelete = 'action_delete';
  static const actionMove = 'action_move';
  static const actionDownload = 'action_download';
  static const actionUpload = 'action_upload';
  static const actionNewFolder = 'action_new_folder';
  static const dirLabel = 'dir_label';
  static const emptyDir = 'empty_dir';
  static const itemCount = 'item_count';
  static const totalSize = 'total_size';
  static const newNameLabel = 'new_name_label';
  static const invalidName = 'invalid_name';
  static const closeLabel = 'close_label';

  // —— App 后台缓存页 ——
  static const cacheTitle = 'cache_browser_title';
  static const cacheSub = 'cache_browser_sub';
  static const cacheDeleteFileConfirm = 'cache_delete_file_confirm';
  static const cacheDeleteDirConfirm = 'cache_delete_dir_confirm';
  static const cacheDeleted = 'cache_deleted';
  static const cacheClearAll = 'cache_clear_all';
  static const cacheClearAllConfirm = 'cache_clear_all_confirm';
  static const cacheImageLoadFailed = 'cache_image_load_failed';

  // —— 码表文件管理页 ——
  static const fsTitle = 'fs_title';
  static const fsRootHint = 'fs_root_hint';
  static const fsNotReady = 'fs_not_ready';
  static const fsEnterBigTitle = 'fs_enter_big_title';
  static const fsEnterBigBody = 'fs_enter_big_body';
  static const fsEnter = 'fs_enter';
  static const fsDeleteFileConfirm = 'fs_delete_file_confirm';
  static const fsDeleteDirConfirm = 'fs_delete_dir_confirm';
  static const fsDeleted = 'fs_deleted';
  static const fsMoveTitle = 'fs_move_title';
  static const fsMoveLabel = 'fs_move_label';
  static const fsMoveConfirm = 'fs_move_confirm';
  static const fsMkdirTitle = 'fs_mkdir_title';
  static const fsCreate = 'fs_create';
  static const fsUploadBigTitle = 'fs_upload_big_title';
  static const fsUploadBigBody = 'fs_upload_big_body';
  static const fsReadFailed = 'fs_read_failed';
  static const fsDownloaded = 'fs_downloaded';
  static const fsDownloadFailed = 'fs_download_failed';
  static const fsShareFailed = 'fs_share_failed';
  static const fsDeleteFailed = 'fs_delete_failed';
  static const fsUploadFailed = 'fs_upload_failed';
  static const fsOpFailed = 'fs_op_failed';
  static const coredumpTitle = 'coredump_title';
  static const coredumpEmpty = 'coredump_empty';
  static const coredumpPull = 'coredump_pull';
  static const coredumpPulling = 'coredump_pulling';
  static const coredumpNeedBle = 'coredump_need_ble';
  static const coredumpMtpBusy = 'coredump_mtp_busy';
  static const coredumpNone = 'coredump_none';
  static const coredumpDone = 'coredump_done';
  static const coredumpShare = 'coredump_share';
  static const coredumpShareFail = 'coredump_share_fail';
  static const coredumpShareSubject = 'coredump_share_subject';
  static const coredumpProgress = 'coredump_progress';
  static const coredumpPulledAt = 'coredump_pulled_at';
  static const coredumpDelete = 'coredump_delete';
  static const coredumpDeleteConfirm = 'coredump_delete_confirm';
  static const coredumpClear = 'coredump_clear';
  static const coredumpClearConfirmTitle = 'coredump_clear_confirm_title';
  static const coredumpClearConfirmBody = 'coredump_clear_confirm_body';

  // —— 日志抓取页（崩溃记录 + 诊断日志）——
  static const captureTitle = 'capture_title';
  static const captureCoredumpSection = 'capture_coredump_section';
  static const captureCoredumpSub = 'capture_coredump_sub';
  static const captureDiagSection = 'capture_diag_section';
  static const captureDiagSub = 'capture_diag_sub';
  static const captureDiagEmpty = 'capture_diag_empty';
  static const captureOriginalName = 'capture_original_name';
  static const captureAt = 'capture_at';
  static const captureDoneBoth = 'capture_done_both';
  static const captureNothing = 'capture_nothing';

  // —— 全局抓取悬浮框 ——
  static const transferMore = 'transfer_more';
  static const transferDismiss = 'transfer_dismiss';
  static const transferSpeed = 'transfer_speed';

  // —— 地图 / 定位 ——
  static const mapTitle = 'map_title';
  static const mapLocate = 'map_locate';
  static const mapLocating = 'map_locating';
  static const mapServiceOff = 'map_service_off';
  static const mapPermissionDenied = 'map_permission_denied';
  static const mapCoord = 'map_coord';
  static const mapOpenSettings = 'map_open_settings';

  // —— 404 ——
  static const notFoundTitle = 'not_found_title';
  static const notFoundGoHome = 'not_found_go_home';

  // —— 固件 OTA ——
  static const otaTitle = 'ota_title';
  static const otaNeedBle = 'ota_need_ble';
  static const otaMtpBusy = 'ota_mtp_busy';
  static const otaCheck = 'ota_check';
  static const otaChecking = 'ota_checking';
  static const otaCloudVersion = 'ota_cloud_version';
  static const otaDeviceVersion = 'ota_device_version';
  static const otaDeviceUnknown = 'ota_device_unknown';
  static const otaHardware = 'ota_hardware';
  static const otaBootloader = 'ota_bootloader';
  static const otaRunningSlot = 'ota_running_slot';
  static const otaRunningSlotBare = 'ota_running_slot_bare';
  static const otaCached = 'ota_cached';
  static const otaInstallCached = 'ota_install_cached';
  static const otaNote = 'ota_note';
  static const otaSize = 'ota_size';
  static const otaReleased = 'ota_released';
  static const otaUpToDate = 'ota_up_to_date';
  static const otaUpdateAvailable = 'ota_update_available';
  static const otaInstall = 'ota_install';
  static const otaReinstall = 'ota_reinstall';
  static const otaDownloading = 'ota_downloading';
  static const otaVerifying = 'ota_verifying';
  static const otaUploading = 'ota_uploading';
  static const otaDownloadAction = 'ota_download_action';
  static const otaVerifyAction = 'ota_verify_action';
  static const otaPushAction = 'ota_push_action';
  static const otaProgress = 'ota_progress';
  static const otaDone = 'ota_done';
  static const otaHashFail = 'ota_hash_fail';
  static const otaCheckFail = 'ota_check_fail';
  static const otaSlotTitle = 'ota_slot_title';
  static const otaSlotEmpty = 'ota_slot_empty';
  static const otaHint = 'ota_hint';
  static const otaDetails = 'ota_details';

  // —— 产品壳 ——
  static const tabRide = 'tab_ride';
  static const tabDevice = 'tab_device';
  static const tabSettings = 'tab_settings';

  static const deviceConnect = 'device_connect';
  static const deviceConnecting = 'device_connecting';
  static const deviceConnected = 'device_connected';
  static const deviceDisconnected = 'device_disconnected';
  static const deviceMismatch = 'device_mismatch';
  static const deviceMismatchShort = 'device_mismatch_short';
  static const deviceSync = 'device_sync';
  static const deviceSyncing = 'device_syncing';
  static const deviceSyncList = 'device_sync_list';
  static const deviceSyncPull = 'device_sync_pull';
  static const deviceSyncPush = 'device_sync_push';
  static const deviceSyncSpeed = 'device_sync_speed';
  static const deviceSyncBytes = 'device_sync_bytes';
  static const deviceNeedConnect = 'device_need_connect';
  static const deviceLinkLost = 'device_link_lost';
  static const deviceRetry = 'device_retry';
  static const deviceBattery = 'device_battery';
  static const deviceBatteryUnknown = 'device_battery_unknown';
  static const deviceRecording = 'device_recording';
  static const deviceMoving = 'device_moving';
  static const devicePaused = 'device_paused';
  static const deviceGpsPhone = 'device_gps_phone';
  static const deviceGpsInternal = 'device_gps_internal';
  static const deviceMtpBusy = 'device_mtp_busy';
  static const deviceStorage = 'device_storage';
  static const deviceHr = 'device_hr';
  static const deviceCadence = 'device_cadence';
  static const devicePower = 'device_power';
  static const deviceDisconnect = 'device_disconnect';
  static const deviceIdleHint = 'device_idle_hint';
  static const deviceNoName = 'device_no_name';

  static const rideTitle = 'ride_title';
  static const rideRecent = 'ride_recent';
  static const rideEmpty = 'ride_empty';
  static const rideNavRoutes = 'ride_nav_routes';
  static const rideNavRoutesSub = 'ride_nav_routes_sub';
  static const rideGpx = 'ride_gpx';
  static const rideGpxSub = 'ride_gpx_sub';
  static const rideFavorites = 'ride_favorites';
  static const rideFavoritesSub = 'ride_favorites_sub';
  static const rideDetailTitle = 'ride_detail_title';
  static const rideShare = 'ride_share';
  static const rideShareImage = 'ride_share_image';
  static const rideShareGpx = 'ride_share_gpx';
  static const rideSharePreparing = 'ride_share_preparing';
  static const rideShareFail = 'ride_share_fail';
  static const rideDelete = 'ride_delete';
  static const ridePoints = 'ride_points';
  static const rideDistance = 'ride_distance';
  static const rideSize = 'ride_size';
  static const rideStatKm = 'ride_stat_km';
  static const rideStatRides = 'ride_stat_rides';
  static const rideStatLast = 'ride_stat_last';
  static const rideStatDash = 'ride_stat_dash';
  static const rideStatTime = 'ride_stat_time';
  static const ridePlanTitle = 'ride_plan_title';
  static const ridePlanHint = 'ride_plan_hint';
  static const ridePlanRides = 'ride_plan_rides';
  static const ridePlanHours = 'ride_plan_hours';
  static const ridePlanKm = 'ride_plan_km';
  static const ridePlanRidesUnit = 'ride_plan_rides_unit';
  static const ridePlanHoursUnit = 'ride_plan_hours_unit';
  static const ridePlanThisWeek = 'ride_plan_this_week';
  static const ridePlanEdit = 'ride_plan_edit';
  static const rideMetricDistance = 'ride_metric_distance';
  static const rideMetricDuration = 'ride_metric_duration';
  static const rideMetricAvgSpeed = 'ride_metric_avg_speed';
  static const rideMetricMaxSpeed = 'ride_metric_max_speed';
  static const rideMetricClimb = 'ride_metric_climb';
  static const rideMetricEleMax = 'ride_metric_ele_max';
  static const ridePinStart = 'ride_pin_start';
  static const ridePinEnd = 'ride_pin_end';
  static const rideOutdoor = 'ride_outdoor';
  static const rideMetricKcal = 'ride_metric_kcal';
  static const rideMetricAvgHr = 'ride_metric_avg_hr';
  static const rideMetricMaxHr = 'ride_metric_max_hr';
  static const rideMetricMoveTime = 'ride_metric_move_time';
  static const rideHrCard = 'ride_hr_card';
  static const rideNoHr = 'ride_no_hr';
  static const rideSpeedCard = 'ride_speed_card';
  static const rideSlower = 'ride_slower';
  static const rideFaster = 'ride_faster';
  static const rideZoneWarmup = 'ride_zone_warmup';
  static const rideZoneFat = 'ride_zone_fat';
  static const rideZoneAerobic = 'ride_zone_aerobic';
  static const rideZoneAnaerobic = 'ride_zone_anaerobic';
  static const rideZoneExtreme = 'ride_zone_extreme';
  static const rideDistM = 'ride_dist_m';
  static const rideDistKm = 'ride_dist_km';
  static const ridePeriodWeek = 'ride_period_week';
  static const ridePeriodNow = 'ride_period_now';
  static const rideMonthPick = 'ride_month_pick';
  static const rideMonthNoData = 'ride_month_no_data';
  static const rideMonth1 = 'ride_month_1';
  static const rideMonth2 = 'ride_month_2';
  static const rideMonth3 = 'ride_month_3';
  static const rideMonth4 = 'ride_month_4';
  static const rideMonth5 = 'ride_month_5';
  static const rideMonth6 = 'ride_month_6';
  static const rideMonth7 = 'ride_month_7';
  static const rideMonth8 = 'ride_month_8';
  static const rideMonth9 = 'ride_month_9';
  static const rideMonth10 = 'ride_month_10';
  static const rideMonth11 = 'ride_month_11';
  static const rideMonth12 = 'ride_month_12';

  /// 12 个月的名字，按下标取（`[0]` = 一月）。写法同 `rideWd1…7`。
  static const rideMonths = <String>[
    rideMonth1,
    rideMonth2,
    rideMonth3,
    rideMonth4,
    rideMonth5,
    rideMonth6,
    rideMonth7,
    rideMonth8,
    rideMonth9,
    rideMonth10,
    rideMonth11,
    rideMonth12,
  ];

  static const rideThisMonth = 'ride_this_month';
  static const rideLoopSuffix = 'ride_loop_suffix';
  static const rideRecentIn = 'ride_recent_in';
  static const rideEmptyIn = 'ride_empty_in';
  static const rideStatsTotal = 'ride_stats_total';
  static const rideSummaryTitle = 'ride_summary_title';
  static const rideSummarySpan = 'ride_summary_span';
  static const rideSummaryAvgRide = 'ride_summary_avg_ride';
  static const rideSummaryBestKm = 'ride_summary_best_km';
  static const rideSummaryBestDur = 'ride_summary_best_dur';
  static const rideSummaryDays = 'ride_summary_days';
  static const rideChartKmDay = 'ride_chart_km_day';
  static const rideChartKmWeek = 'ride_chart_km_week';
  static const rideChartTimeWeek = 'ride_chart_time_week';
  static const rideChartKmMonth = 'ride_chart_km_month';
  static const rideChartTimeMonth = 'ride_chart_time_month';
  static const rideStatAvgDay = 'ride_stat_avg_day';
  static const rideStatAvgWeek = 'ride_stat_avg_week';
  static const rideStatAvgMonth = 'ride_stat_avg_month';
  static const rideSearchHint = 'ride_search_hint';
  static const rideSearchCount = 'ride_search_count';
  static const rideSearchEmpty = 'ride_search_empty';
  static const rideShareOf = 'ride_share_of';
  static const rideWd1 = 'ride_wd_1';
  static const rideWd2 = 'ride_wd_2';
  static const rideWd3 = 'ride_wd_3';
  static const rideWd4 = 'ride_wd_4';
  static const rideWd5 = 'ride_wd_5';
  static const rideWd6 = 'ride_wd_6';
  static const rideWd7 = 'ride_wd_7';
  static const ridePeriodMonth = 'ride_period_month';
  static const ridePeriodStats = 'ride_period_stats';
  static const rideRecentWeek = 'ride_recent_week';
  static const rideEmptyWeek = 'ride_empty_week';
  static const rideSelect = 'ride_select';
  static const rideSelectDone = 'ride_select_done';
  static const rideSelectAll = 'ride_select_all';
  static const rideSelectNone = 'ride_select_none';
  static const rideSelectedN = 'ride_selected_n';
  static const rideBatchDelete = 'ride_batch_delete';
  static const rideBatchDeleteConfirm = 'ride_batch_delete_confirm';
  static const rideRename = 'ride_rename';
  static const rideRenameHint = 'ride_rename_hint';
  static const rideUploadImport = 'ride_upload_import';

  static const placeholderMcuPush = 'placeholder_mcu_push';
  static const placeholderMcuPushNav = 'placeholder_mcu_push_nav';
  static const placeholderMcuPushFav = 'placeholder_mcu_push_fav';
  static const waypointSync = 'waypoint_sync';
  static const waypointSyncFavDone = 'waypoint_sync_fav_done';
  static const waypointSyncNavDone = 'waypoint_sync_nav_done';

  static const navRoutesTitle = 'nav_routes_title';
  static const navRoutesEmpty = 'nav_routes_empty';
  static const navRoutesNew = 'nav_routes_new';
  static const navRoutesCount = 'nav_routes_count';
  static const navRoutesName = 'nav_routes_name';
  static const navRoutesNameHint = 'nav_routes_name_hint';
  static const navRouteEditorTitle = 'nav_route_editor_title';
  static const navRouteSave = 'nav_route_save';
  static const navRouteDelete = 'nav_route_delete';
  static const navRouteMaxPts = 'nav_route_max_pts';
  static const navRouteNeedTwo = 'nav_route_need_two';
  static const navRouteNeedOne = 'nav_route_need_one';
  static const navRouteTapHint = 'nav_route_tap_hint';
  static const navRouteStopTitle = 'nav_route_stop_title';
  static const navRouteStopLabel = 'nav_route_stop_label';
  static const navRouteStopHint = 'nav_route_stop_hint';
  static const navRouteStopDefault = 'nav_route_stop_default';
  static const navRouteStopNeed = 'nav_route_stop_need';
  static const gpxTapHint = 'gpx_tap_hint';
  static const gpxEditorNameHint = 'gpx_editor_name_hint';
  static const roadSnapFail = 'road_snap_fail';
  static const roadRouteFail = 'road_route_fail';
  static const mapEditorUndo = 'map_editor_undo';
  static const mapEditorLocate = 'map_editor_locate';
  static const mapEditorMove = 'map_editor_move';
  static const mapEditorMoving = 'map_editor_moving';
  static const mapEditorInsertAfter = 'map_editor_insert_after';
  static const mapEditorHintNav = 'map_editor_hint_nav';
  static const mapEditorHintGpx = 'map_editor_hint_gpx';
  static const mapEditorStats = 'map_editor_stats';
  static const mapEditorEmptyStops = 'map_editor_empty_stops';
  static const mapEditorDiscardTitle = 'map_editor_discard_title';
  static const mapEditorDiscardBody = 'map_editor_discard_body';
  static const mapEditorRemove = 'map_editor_remove';
  static const mapEditorStopN = 'map_editor_stop_n';
  static const mapEditorCrosshair = 'map_editor_crosshair';
  static const mapEditorLat = 'map_editor_lat';
  static const mapEditorLng = 'map_editor_lng';
  static const mapEditorCoordTitle = 'map_editor_coord_title';
  static const mapEditorCoordEdit = 'map_editor_coord_edit';
  static const mapEditorCoordAdd = 'map_editor_coord_add';
  static const mapEditorCoordRange = 'map_editor_coord_range';
  static const mapEditorCoordBad = 'map_editor_coord_bad';
  static const mapEditorCrosshairHint = 'map_editor_crosshair_hint';
  static const mapEditorPlaceHere = 'map_editor_place_here';
  static const mapEditorSnapping = 'map_editor_snapping';
  static const mapEditorRouting = 'map_editor_routing';
  static const roadSnapKept = 'road_snap_kept';
  static const roadSnapOffline = 'road_snap_offline';
  static const mapPickCoord = 'map_pick_coord';
  static const mapEditorNeedName = 'map_editor_need_name';

  static const gpxTitle = 'gpx_title';
  static const gpxEmpty = 'gpx_empty';
  static const gpxImportFile = 'gpx_import_file';
  static const gpxUpload = 'gpx_upload';
  static const gpxUploaded = 'gpx_uploaded';
  static const gpxNew = 'gpx_new';
  static const gpxEditorTitle = 'gpx_editor_title';
  static const gpxNeedBle = 'gpx_need_ble';
  static const gpxUploadName = 'gpx_upload_name';
  static const gpxUploadNameLabel = 'gpx_upload_name_label';
  static const gpxUploadNameHint = 'gpx_upload_name_hint';
  static const gpxUploadedMany = 'gpx_uploaded_many';
  static const gpxUploading = 'gpx_uploading';
  static const gpxUploadProgress = 'gpx_upload_progress';
  static const gpxNotGpx = 'gpx_not_gpx';
  static const navUploadImport = 'nav_upload_import';
  static const navUploadNeedPts = 'nav_upload_need_pts';

  static const favsTitle = 'favs_title';
  static const favsEmpty = 'favs_empty';
  static const favsAdd = 'favs_add';
  static const favsName = 'favs_name';
  static const favsNameTitle = 'favs_name_title';
  static const favsNameHint = 'favs_name_hint';
  static const favsPickHint = 'favs_pick_hint';
  static const favsNeedFix = 'favs_need_fix';
  static const favsPickMap = 'favs_pick_map';
  static const favsUseHere = 'favs_use_here';
  static const favsSaved = 'favs_saved';
  static const favsEditTitle = 'favs_edit_title';
  static const favsConfirm = 'favs_confirm';
  static const favsNeedName = 'favs_need_name';
  static const favsNeedPin = 'favs_need_pin';
  static const favsMoveHint = 'favs_move_hint';

  static const settingsTitle = 'settings_title';
  static const settingsNotif = 'settings_notif';
  static const settingsOta = 'settings_ota';
  static const settingsEph = 'settings_eph';
  static const settingsCoredump = 'settings_coredump';
  static const settingsCoredumpSub = 'settings_coredump_sub';
  static const settingsLang = 'settings_lang';
  static const settingsFavs = 'settings_favs';
  static const settingsFiles = 'settings_files';
  static const settingsMapUsb = 'settings_map_usb';
  static const settingsMapUsbTitle = 'settings_map_usb_title';
  static const settingsMapUsbBody = 'settings_map_usb_body';
  static const settingsKeepAlive = 'settings_keep_alive';
  static const settingsKeepAliveSub = 'settings_keep_alive_sub';
  static const settingsCache = 'settings_cache';
  static const settingsCacheSub = 'settings_cache_sub';
  static const osmCitiesTitle = 'osm_cities_title';
  static const osmCitiesDownload = 'osm_cities_download';
  static const osmCitiesDownloadSub = 'osm_cities_download_sub';
  static const osmCitiesLocal = 'osm_cities_local';
  static const osmCitiesLocalSub = 'osm_cities_local_sub';
  static const osmCitiesLocalEmpty = 'osm_cities_local_empty';
  static const osmCitiesSearch = 'osm_cities_search';
  static const osmCitiesHint = 'osm_cities_hint';
  static const osmCitiesProgress = 'osm_cities_progress';
  static const osmCitiesComplete = 'osm_cities_complete';
  static const osmCitiesPartial = 'osm_cities_partial';
  static const osmCitiesAboutSize = 'osm_cities_about_size';
  static const osmCitiesCancel = 'osm_cities_cancel';
  static const osmCitiesDelete = 'osm_cities_delete';
  static const osmCitiesResume = 'osm_cities_resume';
  static const osmCitiesFailed = 'osm_cities_failed';
  static const settingsLogsSub = 'settings_logs_sub';
  static const settingsAppUpdate = 'settings_app_update';
  static const settingsAppUpdateTitle = 'settings_app_update_title';
  static const settingsAppUpdateHint = 'settings_app_update_hint';
  static const settingsAppUpdateCheck = 'settings_app_update_check';
  static const settingsAppUpdateChecking = 'settings_app_update_checking';
  static const settingsAppUpdateNone = 'settings_app_update_none';
  static const settingsAppUpdateCurrent = 'settings_app_update_current';
  static const settingsDeveloper = 'settings_developer';
  static const settingsVersionHint = 'settings_version_hint';

  static const aboutTitle = 'about_title';
  static const aboutVersion = 'about_version';
  static const aboutDeveloperName = 'about_developer_name';
  static const authorRole = 'author_role';
  static const aboutApp = 'about_app';
  static const aboutAppDesc = 'about_app_desc';
  static const aboutThanks = 'about_thanks';
  static const aboutCopyright = 'about_copyright';
  static const aboutLinkCopied = 'about_link_copied';

  static const syncDone = 'sync_done';
  static const syncDoneFull = 'sync_done_full';
  static const syncNone = 'sync_none';
  static const syncFail = 'sync_fail';
  static const syncPartialPush = 'sync_partial_push';
  static const deleteConfirm = 'delete_confirm';
  static const save = 'save';
  static const add = 'add';
}
