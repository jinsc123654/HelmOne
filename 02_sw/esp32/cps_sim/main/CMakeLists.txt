idf_component_register(
    SRCS "main.c" "cps_sim.c" "gatt_cps.c" "cli.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer console esp_driver_rmt)
