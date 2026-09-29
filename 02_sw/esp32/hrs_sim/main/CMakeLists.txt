idf_component_register(
    SRCS "main.c" "hr_sim.c" "gatt_hr.c" "cli.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer console esp_driver_rmt)
