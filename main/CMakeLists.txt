set(EXTRA_SRCS "")
set(EXTRA_DEFINES "")

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/main.cmake.extra")
    include(${CMAKE_CURRENT_LIST_DIR}/main.cmake.extra)
endif()

if(IDF_TARGET STREQUAL "esp32s3")
    set(BOARD_SRCS "board_waveshare_wvshr200.c")

    idf_component_register(
        SRCS "main.c" "${BOARD_SRCS}" "board_defaults.c" ${EXTRA_SRCS}
        INCLUDE_DIRS "."
    )

elseif(IDF_TARGET STREQUAL "esp32p4")
    set(BOARD_SRCS "board_p4_ev.c")

    idf_component_register(
        SRCS "main.c" "${BOARD_SRCS}" "board_defaults.c" ${EXTRA_SRCS}
        INCLUDE_DIRS "."
        PRIV_REQUIRES esp_lcd esp_mm esp_psram esp_driver_jpeg esp_timer
                      esp_event esp_wifi esp_netif nvs_flash esp_driver_gpio
                      espressif__esp_hosted espressif__esp_lcd_ek79007
    )
endif()

if(EXTRA_DEFINES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${EXTRA_DEFINES})
endif()
