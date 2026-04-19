idf_component_register(
    SRCS
        "app_state.c"
        "can_bus.c"
        "control.c"
        "meb-preheat.c"
        "status_led.c"
        "usb_console.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_twai
        esp_driver_usb_serial_jtag
        esp_driver_gpio
        esp_system
        esp_timer
        espressif__led_strip
)
