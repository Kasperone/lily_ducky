# =============================================================================
# ESP-IDF component for the `T-Dongle-C5-idf` environment only.
# =============================================================================
# The hybrid SoftAP firmware that passed on hardware (issue #9) was a flat
# ESP-IDF tree. It is ported here under src/idf_softap/ so the main repository
# layout stays consistent; this component lists those sources explicitly rather
# than globbing, because src/ also holds the Arduino firmware (main.cpp,
# c2/web_server.cpp, hal/) which cannot compile without the Arduino core.
#
# Arduino environments do not read this file at all - they compile src/ with
# SCons and exclude the idf_softap/ subtree via `build_src_filter` in
# platformio.ini. Nothing here runs for framework = arduino.
# =============================================================================
idf_component_register(
    SRCS "idf_softap/app_main.c"
         "idf_softap/c2_server.c"
         "idf_softap/storage.c"
         "idf_softap/interpreter.cpp"
         "idf_softap/interp_hal.c"
         "idf_softap/board_spi.c"
         "idf_softap/st7735.c"
         "idf_softap/duck_display.c"
         "idf_softap/recon.c"
         "idf_softap/pcap_writer.c"
    INCLUDE_DIRS "idf_softap"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server
             fatfs vfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio
             esp_driver_ledc esp_timer
)
