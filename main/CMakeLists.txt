idf_component_register(SRCS "handheld.c" "chip8.c" "wifi.c" "net.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi nvs_flash esp_netif esp_event
                                esp_driver_i2s esp_driver_spi esp_driver_gpio
                                esp_lcd espressif__esp_lcd_ili9341
                                esp_timer
                                esp_http_client esp-tls mbedtls)