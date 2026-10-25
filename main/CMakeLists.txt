# Define source files
set(SOURCES "audio/audio_codec.cc"
            "audio/audio_debugger.cc"
            "audio/audio_service.cc"
            "audio/demuxer/ogg_demuxer.cc"
            "notify/notify_player.cc"
            "audio/codecs/no_audio_codec.cc"
            "audio/codecs/box_audio_codec.cc"
            "audio/codecs/es8311_audio_codec.cc"
            "audio/codecs/es8374_audio_codec.cc"
            "audio/codecs/es8388_audio_codec.cc"
            "audio/codecs/es8389_audio_codec.cc"
            "audio/codecs/dummy_audio_codec.cc"
            "led/single_led.cc"
            "led/circular_strip.cc"
            "led/gpio_led.cc"
            "display/display.cc"
            "display/text_glyph.cc"
            "display/lcd_display.cc"
            "display/oled_display.cc"
            "display/lvgl_display/lvgl_display.cc"
            "display/emote_display.cc"
            "display/lvgl_display/emoji_collection.cc"
            "display/lvgl_display/lvgl_theme.cc"
            "display/lvgl_display/lvgl_font.cc"
            "display/lvgl_display/dynamic_glyph_cache.cc"
            "display/lvgl_display/lvgl_image.cc"
            "display/lvgl_display/gif/lvgl_gif.cc"
            "display/lvgl_display/gif/gifdec.c"
            "display/lvgl_display/jpg/image_to_jpeg.cpp"
            "display/lvgl_display/jpg/jpeg_to_image.c"
            "protocols/protocol.cc"
            "protocols/text_glyph_payload.cc"
            "protocols/mqtt_protocol.cc"
            "protocols/websocket_protocol.cc"
            "mcp_server.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "device_state_machine.cc"
            "assets.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg" "audio" "audio/demuxer" "protocols")

# Add board common files
list(APPEND SOURCES
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "boards/common/ml307_board.cc"
    "boards/common/nt26_board.cc"
    "boards/common/dual_network_board.cc"
    "boards/common/adc_battery_monitor.cc"
    "boards/common/axp2101.cc"
    "boards/common/backlight.cc"
    "boards/common/button.cc"
    "boards/common/i2c_device.cc"
    "boards/common/knob.cc"
    "boards/common/power_save_timer.cc"
    "boards/common/press_to_talk_mcp_tool.cc"
    "boards/common/sleep_timer.cc"
    "boards/common/sy6970.cc"
    "boards/common/system_reset.cc"
)
if(CONFIG_XIAOZHI_NETWORK_ETHERNET)
    list(APPEND SOURCES "boards/common/ethernet_board.cc")
endif()
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)


idf_build_get_property(build_components BUILD_COMPONENTS)
# Function to find component dynamically by pattern
function(find_component_by_pattern PATTERN COMPONENT_VAR PATH_VAR)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${PATTERN}")
            set(${COMPONENT_VAR} ${COMPONENT} PARENT_SCOPE)
            idf_component_get_property(COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
            set(${PATH_VAR} "${COMPONENT_PATH}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

# Set default BUILTIN_TEXT_FONT and BUILTIN_ICON_FONT
set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)
set(BUILTIN_ICON_FONT font_material_symbols_14_1)

set(EMOTE_RESOLUTION "320_240")

# Select the board source directory from Kconfig
if(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI)
    set(BOARD_DIR "bread-compact-wifi")
elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_CAM)
    set(BOARD_DIR "bread-compact-wifi-s3cam")
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)
    set(BUILTIN_ICON_FONT font_material_symbols_16_4)
    set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_64)
elseif(CONFIG_BOARD_TYPE_ESP32_S3_KORVO_2_V3_RNDIS)
    set(BOARD_DIR "espressif/esp32-s3-korvo-2-v3.0-rndis")
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_20_4)
    set(BUILTIN_ICON_FONT font_material_symbols_20_4)
    set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_64)
else()
    set(BOARD_DIR "bread-compact-wifi-s3cam")
endif()

if(NOT BOARD_DIR AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    message(FATAL_ERROR "The selected board does not define BOARD_DIR")
endif()

set(BOARD_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/config.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BOARD_CONFIG_FILE}")

set(BOARD_TYPE "")
set(BOARD_MANUFACTURER "")
if(EXISTS "${BOARD_CONFIG_FILE}")
    file(READ "${BOARD_CONFIG_FILE}" BOARD_CONFIG_JSON)
    string(REGEX MATCH "\"type\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" BOARD_TYPE_MATCH "${BOARD_CONFIG_JSON}")
    if(BOARD_TYPE_MATCH)
        set(BOARD_TYPE "${CMAKE_MATCH_1}")
    endif()
    string(REGEX MATCH "\"manufacturer\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" BOARD_MANUFACTURER_MATCH "${BOARD_CONFIG_JSON}")
    if(BOARD_MANUFACTURER_MATCH)
        set(BOARD_MANUFACTURER "${CMAKE_MATCH_1}")
    endif()
endif()
if(NOT BOARD_TYPE)
    string(REPLACE "/" "-" BOARD_TYPE "${BOARD_DIR}")
endif()
if(NOT BOARD_MANUFACTURER AND BOARD_DIR MATCHES "^([^/]+)/")
    set(BOARD_MANUFACTURER "${CMAKE_MATCH_1}")
endif()

file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})

# Select one audio engine for each chip family
if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32S31)
    list(APPEND SOURCES "audio/engines/afe_audio_engine.cc")
    list(APPEND SOURCES "audio/wake_words/custom_wake_word.cc")
    list(APPEND SOURCES "audio/wake_words/wake_word_audio_cache.cc")
else()
    list(APPEND SOURCES "audio/engines/lite_audio_engine.cc")
    list(APPEND SOURCES "audio/wake_words/esp_wake_word.cc")
endif()

if (CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
    list(APPEND SOURCES "boards/common/blufi.cpp")
endif ()

if(CONFIG_LANGUAGE_HI_IN)
    set(LANG_DIR "hi-IN")
else()
    set(LANG_DIR "en-US")
endif()

# Define generation path
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")

file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

# Include EspVideo & Esp32Camera if target is ESP32S3
if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND SOURCES "boards/common/esp_video.cc"
                        "boards/common/rndis_board.cc"
                        "boards/common/esp32_camera.cc"
                        )
endif()

set(MAIN_PRIV_REQUIRES_EXTRA "")

# 📌 यहाँ मुख्य रजिस्टर फंक्शन है जिसमें esp_wifi और esp_now जोड़ा गया है
idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES
                        esp_wifi
                        esp_now
                        esp_pm
                        esp_psram
                        esp_netif
                        esp_eth
                        esp_driver_gpio
                        esp_driver_uart
                        esp_driver_spi
                        esp_driver_i2c
                        esp_driver_i2s
                        esp_driver_ledc
                        esp_driver_jpeg
                        esp_driver_ppa
                        esp_driver_tsens
                        esp_app_format
                        app_update
                        spi_flash
                        mbedtls
                        console
                        efuse
                        bt
                        fatfs
                        xiaozhi-fonts
                        ${MAIN_PRIV_REQUIRES_EXTRA}
                    )

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
endif()

if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB}
                        PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                        PRIVATE BOARD_MANUFACTURER=\"${BOARD_MANUFACTURER}\"
                        PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
                        )
    if(CONFIG_IDF_TARGET_ESP32S3)
        idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
        if(lvgl_lib)
            target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/display")
            if(CONFIG_SPIRAM)
                target_compile_definitions(${lvgl_lib} PRIVATE
                    "LV_MEM_POOL_INCLUDE=\"lvgl_psram_pool.h\"")
            elseif(CONFIG_LV_USE_BUILTIN_MALLOC)
                target_compile_definitions(${lvgl_lib} PRIVATE
                    "LV_MEM_POOL_INCLUDE=\"lvgl_no_psram_pool.h\"")
            endif()
        endif()
    endif()
endif()
