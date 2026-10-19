cmake_minimum_required(VERSION 3.16)
project(SkywaveLink VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC-specific settings
if(MSVC)
    add_compile_definitions(_USE_MATH_DEFINES)
    add_compile_options(/MP)
    add_compile_options(/wd4244)
    add_compile_options(/wd4267)
    add_compile_options(/utf-8)
endif()

# Build options
option(SKYWAVE_LINK_BUILD_TESTS "Build test suite" OFF)

# Warnings for this project's own sources; thirdparty/ is exempt (its
# headers are included as SYSTEM, its sources keep their defaults).
if(NOT MSVC)
    set(SKYWAVE_LINK_WARNINGS -Wall -Wextra)
endif()

# Find dependencies
find_package(Threads REQUIRED)

# Find SQLite3
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3 sqlite3)
endif()

if(NOT SQLITE3_FOUND)
    # Try find_package as fallback
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(SQLITE3_LIBRARIES SQLite::SQLite3)
        set(SQLITE3_INCLUDE_DIRS ${SQLite3_INCLUDE_DIRS})
        set(SQLITE3_FOUND TRUE)
    endif()
endif()

# Find OpenSSL for SMTP TLS
find_package(OpenSSL QUIET)

# Find Hamlib for local radio control
set(HAMLIB_LINK_TARGET "")
if(PkgConfig_FOUND)
    pkg_check_modules(HAMLIB QUIET IMPORTED_TARGET hamlib)
endif()
if(HAMLIB_FOUND)
    set(HAMLIB_LINK_TARGET PkgConfig::HAMLIB)
else()
    find_library(HAMLIB_LIBRARIES NAMES hamlib)
    find_path(HAMLIB_INCLUDE_DIRS hamlib/rig.h)
    if(HAMLIB_LIBRARIES AND HAMLIB_INCLUDE_DIRS)
        set(HAMLIB_FOUND TRUE)
        set(HAMLIB_LINK_TARGET ${HAMLIB_LIBRARIES})
    endif()
endif()
if(NOT HAMLIB_FOUND)
    message(FATAL_ERROR "Hamlib not found. Install hamlib.")
endif()

# Vendored third-party dependencies kept inside this project
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui")
set(STB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb")

# nlohmann/json header-only library
set(JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/json/include")

# No-modem test link emulator (TCP command/data proxy)
add_executable(skywave_link_emulator
    tools/skywave_link_emulator.cpp
)
target_link_libraries(skywave_link_emulator PRIVATE Threads::Threads)
target_compile_options(skywave_link_emulator PRIVATE ${SKYWAVE_LINK_WARNINGS})

# Core static library (non-GUI sources, linkable by tests)
add_library(skywave_link_core STATIC
    src/core/config.cpp
    src/core/freq_manager.cpp
    src/core/message_store.cpp
    src/core/modem_client.cpp
    src/core/radio_controller.cpp
    src/core/file_transfer.cpp
    src/protocol/lzh.cpp
    src/protocol/skywave_protocol.cpp
    src/protocol/b2f.cpp
    src/position/aprs_client.cpp
    src/position/nmea_parser.cpp
    src/email/smtp_client.cpp
    src/email/pop3_client.cpp
    src/email/winlink_telnet_client.cpp
    src/email/email_gateway_job.cpp
    src/messaging/message_router.cpp
    src/core/channel_scanner.cpp
    src/core/image_preview.cpp
    src/core/voice_message.cpp
    src/core/mesh_relay.cpp
    src/core/web_server.cpp
)

target_include_directories(skywave_link_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${HAMLIB_INCLUDE_DIRS}
)
target_include_directories(skywave_link_core SYSTEM PUBLIC ${STB_INCLUDE_DIR})

target_link_libraries(skywave_link_core PUBLIC Threads::Threads ${HAMLIB_LINK_TARGET})
target_compile_options(skywave_link_core PRIVATE ${SKYWAVE_LINK_WARNINGS})

if(SQLITE3_FOUND)
    target_include_directories(skywave_link_core PUBLIC ${SQLITE3_INCLUDE_DIRS})
    target_link_libraries(skywave_link_core PUBLIC ${SQLITE3_LIBRARIES})
    target_compile_definitions(skywave_link_core PUBLIC SKYWAVE_LINK_HAS_SQLITE)
else()
    message(WARNING "SQLite3 not found - message store will use file-based storage")
endif()

if(OPENSSL_FOUND)
    target_include_directories(skywave_link_core PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(skywave_link_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(skywave_link_core PUBLIC SKYWAVE_LINK_HAS_OPENSSL)
else()
    message(WARNING "OpenSSL not found - SMTP TLS will be disabled")
endif()

# Control-protocol probe against a live modem (see tools/skywave_link_probe.cpp)
add_executable(skywave_link_probe tools/skywave_link_probe.cpp)
target_link_libraries(skywave_link_probe PRIVATE skywave_link_core)
target_compile_options(skywave_link_probe PRIVATE ${SKYWAVE_LINK_WARNINGS})

# Find SDL2
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 sdl2)
endif()
if(NOT SDL2_FOUND)
    find_package(SDL2 QUIET)
endif()

find_package(OpenGL QUIET)

# UI fonts compiled into the GUI binary (no runtime file lookup). The TTFs are
# the ones Dear ImGui ships in misc/fonts (Roboto: Apache 2.0, Cousine: SIL
# OFL 1.1; see thirdparty/imgui/misc/fonts/LICENSE.txt). A missing file is
# embedded as empty and the GUI falls back to ImGui's built-in font.
set(SKYWAVE_LINK_FONT_DIR "${IMGUI_DIR}/misc/fonts" CACHE PATH
    "Directory holding Roboto-Medium.ttf and Cousine-Regular.ttf to embed in the GUI")

function(skywave_link_embed_font out_var symbol path)
    if(EXISTS "${path}")
        file(READ "${path}" _hex HEX)
        file(SIZE "${path}" _size)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
        string(REGEX REPLACE "((0x..,){20})" "\\1\n" _bytes "${_bytes}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        message(STATUS "Embedding UI font: ${path}")
    else()
        set(_bytes "0")
        set(_size 0)
        message(STATUS "UI font not found, using ImGui default: ${path}")
    endif()
    set(${out_var}
        "extern const unsigned char ${symbol}[];\nextern const unsigned int ${symbol}Size;\nconst unsigned char ${symbol}[] = {\n${_bytes}\n};\nconst unsigned int ${symbol}Size = ${_size};\n\n"
        PARENT_SCOPE)
endfunction()

skywave_link_embed_font(_font_ui kEmbeddedFontUi "${SKYWAVE_LINK_FONT_DIR}/Roboto-Medium.ttf")
skywave_link_embed_font(_font_mono kEmbeddedFontMono "${SKYWAVE_LINK_FONT_DIR}/Cousine-Regular.ttf")
set(SKYWAVE_LINK_FONTS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_fonts.cpp")
file(CONFIGURE OUTPUT "${SKYWAVE_LINK_FONTS_CPP}"
     CONTENT "// Generated by CMake from TTF files; do not edit.\n\n${_font_ui}${_font_mono}"
     @ONLY)

if(SDL2_FOUND AND OPENGL_FOUND AND EXISTS "${IMGUI_DIR}/imgui.cpp")
    set(SKYWAVE_LINK_GUI_SOURCES
        src/main.cpp
        src/gui/app.cpp
        src/gui/shared_state.cpp
        src/gui/chat_window.cpp
        src/gui/file_window.cpp
        src/gui/message_window.cpp
        src/gui/position_window.cpp
        src/gui/settings_window.cpp
        src/gui/weather_window.cpp
        src/gui/mail_window.cpp
        src/gui/bbs_window.cpp
        src/gui/frequency_window.cpp
        src/gui/station_map_window.cpp
        src/gui/beacon_panel.cpp
        src/gui/ping_panel.cpp
        src/gui/main_content.cpp
        src/gui/modern_style.cpp
    )
    # Own GUI sources only: the vendored ImGui files keep their defaults.
    set_source_files_properties(${SKYWAVE_LINK_GUI_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${SKYWAVE_LINK_WARNINGS}")

    add_executable(skywave_link
        ${SKYWAVE_LINK_GUI_SOURCES}
        ${SKYWAVE_LINK_FONTS_CPP}

        # ImGui core
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        # ImGui backends
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
    )

    target_include_directories(skywave_link SYSTEM PRIVATE
        ${JSON_INCLUDE_DIR}
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${SDL2_INCLUDE_DIRS}
    )

    target_compile_definitions(skywave_link PRIVATE
        SKYWAVE_LINK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_directories(skywave_link PRIVATE ${SDL2_LIBRARY_DIRS})

    target_link_libraries(skywave_link PRIVATE
        skywave_link_core
        ${SDL2_LIBRARIES}
        ${OPENGL_LIBRARIES}
    )

    # Platform-specific settings
    if(APPLE)
        target_link_libraries(skywave_link PRIVATE
            "-framework OpenGL"
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
        )
    elseif(WIN32)
        target_link_libraries(skywave_link PRIVATE
            opengl32
            imm32
            ws2_32
        )
    elseif(UNIX)
        target_link_libraries(skywave_link PRIVATE dl)
    endif()

    add_custom_command(TARGET skywave_link POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:skywave_link>/assets/maps"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/maps/natural_earth_gray_50m.png"
            "$<TARGET_FILE_DIR:skywave_link>/assets/maps/natural_earth_gray_50m.png"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/maps/README.txt"
            "$<TARGET_FILE_DIR:skywave_link>/assets/maps/README.txt"
    )

    message(STATUS "Skywave Link: Building with SDL2 + OpenGL")
    if(SQLITE3_FOUND)
        message(STATUS "  SQLite3: Found")
    endif()
    if(OPENSSL_FOUND)
        message(STATUS "  OpenSSL: Found")
    endif()
else()
    message(WARNING "SDL2/OpenGL not found - building core library and tests only")
endif()

# Tests
if(SKYWAVE_LINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
