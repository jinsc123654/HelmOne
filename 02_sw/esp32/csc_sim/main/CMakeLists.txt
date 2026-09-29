idf_component_register(
    SRCS "main.c" "csc_sim.c" "gatt_csc.c" "cli.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer console esp_driver_rmt)
