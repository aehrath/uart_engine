cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" UART_DMA_VERSION_TEXT)
string(STRIP "${UART_DMA_VERSION_TEXT}" UART_DMA_VERSION_TEXT)
project(uart_dma VERSION "${UART_DMA_VERSION_TEXT}" LANGUAGES C)

configure_file(
    config/app_version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/uart_dma/app_version.h"
    @ONLY
)

if(APPLE)
    enable_language(OBJC)
endif()

include(FetchContent)

add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/config"
)

if(WIN32)
    set(FREERTOS_PORT "MSVC_MINGW" CACHE STRING "FreeRTOS portable layer")
else()
    set(FREERTOS_PORT "GCC_POSIX" CACHE STRING "FreeRTOS portable layer")
endif()
set(FREERTOS_HEAP "4" CACHE STRING "FreeRTOS heap implementation")

FetchContent_Declare(freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG V11.3.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(freertos_kernel)

add_library(ble_hci_wire STATIC src/ble_hci_wire.c)
target_compile_features(ble_hci_wire PRIVATE c_std_11)
target_include_directories(ble_hci_wire PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(MSVC)
    target_compile_options(ble_hci_wire PRIVATE /W4 /WX)
else()
    target_compile_options(ble_hci_wire PRIVATE -Wall -Wextra -Werror)
endif()

add_library(uart_dma STATIC
    src/ble_hci_parser.c
    src/uart_crc16.c
    src/uart_dma_ring_buffer.c
    src/uart_dma_freertos.c
)

target_compile_features(uart_dma PRIVATE c_std_11)
target_include_directories(uart_dma PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(uart_dma PUBLIC freertos_kernel ble_hci_wire)

if(MSVC)
    target_compile_options(uart_dma PRIVATE /W4 /WX)
else()
    target_compile_options(uart_dma PRIVATE -Wall -Wextra -Werror)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(uart_dma_tests tests/test_uart_dma.c)
    target_link_libraries(uart_dma_tests PRIVATE uart_dma)

    if(MSVC)
        target_compile_options(uart_dma_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(uart_dma_tests PRIVATE -Wall -Wextra -Werror)
    endif()

    add_test(NAME uart_dma_tests COMMAND uart_dma_tests)
endif()

option(UART_DMA_BUILD_DEMO "Build the native UART DMA round-trip demo" ON)

if(UART_DMA_BUILD_DEMO)
    add_executable(uart_dma_demo demo/uart_dma_demo.c)
    target_link_libraries(uart_dma_demo PRIVATE uart_dma)

    if(MSVC)
        target_compile_options(uart_dma_demo PRIVATE /W4 /WX)
    else()
        target_compile_options(uart_dma_demo PRIVATE -Wall -Wextra -Werror)
    endif()

    if(BUILD_TESTING)
        add_test(NAME uart_dma_demo COMMAND uart_dma_demo)
        set_tests_properties(uart_dma_demo PROPERTIES TIMEOUT 10)
    endif()
endif()

option(UART_DMA_BUILD_SERVER "Build the continuous BLE-side UART server" ON)

if(UNIX AND UART_DMA_BUILD_SERVER)
    add_executable(ble_uart_server
        apps/ble_uart_server.c
        apps/server/pseudo_uart.c
        apps/server/serial_ingress.c
    )
    target_include_directories(ble_uart_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/common")
    target_include_directories(ble_uart_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/server")
    target_link_libraries(ble_uart_server PRIVATE uart_dma)
    target_compile_definitions(ble_uart_server PRIVATE _DEFAULT_SOURCE)

    find_library(UTIL_LIBRARY util)
    if(UTIL_LIBRARY)
        target_link_libraries(ble_uart_server PRIVATE "${UTIL_LIBRARY}")
    endif()

    target_compile_options(ble_uart_server PRIVATE -Wall -Wextra -Werror)
endif()

option(UART_DMA_BUILD_CLIENT "Build the C UART HCI client" ON)

add_library(ble_uart_app_common STATIC
    apps/common/h4_wire.c
    apps/common/serial_port.c
)
target_include_directories(ble_uart_app_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(ble_uart_app_common PRIVATE c_std_11)
target_link_libraries(ble_uart_app_common PUBLIC ble_hci_wire)
if(MSVC)
    target_compile_options(ble_uart_app_common PRIVATE /W4)
else()
    target_compile_definitions(ble_uart_app_common PRIVATE _DEFAULT_SOURCE)
    target_compile_options(ble_uart_app_common PRIVATE -Wall -Wextra -Werror)
endif()

if(UNIX AND UART_DMA_BUILD_CLIENT)
    add_executable(ble_uart_client apps/ble_uart_client.c)
    target_link_libraries(ble_uart_client PRIVATE ble_uart_app_common)
    target_compile_definitions(ble_uart_client PRIVATE _DEFAULT_SOURCE)
    target_compile_features(ble_uart_client PRIVATE c_std_11)
    target_compile_options(ble_uart_client PRIVATE -Wall -Wextra -Werror)
endif()

option(UART_DMA_BUILD_PORTABLE_UI "Build the portable C UART HCI GUI" ON)

if(UART_DMA_BUILD_PORTABLE_UI)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 6.0
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(raygui
        GIT_REPOSITORY https://github.com/raysan5/raygui.git
        GIT_TAG 4.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(raylib raygui)
    FetchContent_Declare(space_grotesk
        URL https://github.com/floriankarsten/space-grotesk/releases/download/2.0.0/SpaceGrotesk-2.0.0.zip
        URL_HASH SHA256=53b415577d4139248555300710bea0d268c7a5be67b93de53b716a9736cabffd
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(space_grotesk)
    if(NOT space_grotesk_POPULATED)
        FetchContent_Populate(space_grotesk)
    endif()
    set(UART_DMA_UI_FONT_SOURCE
        "${space_grotesk_SOURCE_DIR}/ttf/static/SpaceGrotesk-Regular.ttf")
    set(UART_DMA_UI_DISPLAY_FONT_SOURCE
        "${space_grotesk_SOURCE_DIR}/ttf/static/SpaceGrotesk-Bold.ttf")

    add_executable(ble_uart_client_ui
        apps/ble_uart_client_ui.c
        apps/common/dashboard_dropdown.c
        apps/common/dashboard_log.c)
    target_include_directories(ble_uart_client_ui PRIVATE "${raygui_SOURCE_DIR}/src")
    target_link_libraries(ble_uart_client_ui PRIVATE raylib ble_uart_app_common)
    target_compile_definitions(ble_uart_client_ui PRIVATE
        UART_DMA_UI_FONT_FILE="SpaceGrotesk-Regular.ttf"
        UART_DMA_UI_DISPLAY_FONT_FILE="SpaceGrotesk-Bold.ttf")
    target_compile_features(ble_uart_client_ui PRIVATE c_std_11)

    if(MSVC)
        target_compile_options(ble_uart_client_ui PRIVATE /W4)
    else()
        target_compile_definitions(ble_uart_client_ui PRIVATE _DEFAULT_SOURCE)
        target_compile_options(ble_uart_client_ui PRIVATE -Wall -Wextra)
    endif()

    if(APPLE)
        set_target_properties(ble_uart_client_ui PROPERTIES
            MACOSX_BUNDLE TRUE
            OUTPUT_NAME "BLE UART Client"
            MACOSX_BUNDLE_BUNDLE_NAME "BLE UART Client"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.example.ble-uart-client"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        )
    elseif(WIN32)
        set_target_properties(ble_uart_client_ui PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    add_custom_command(TARGET ble_uart_client_ui POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${UART_DMA_UI_FONT_SOURCE}"
                "$<TARGET_FILE_DIR:ble_uart_client_ui>/SpaceGrotesk-Regular.ttf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${UART_DMA_UI_DISPLAY_FONT_SOURCE}"
                "$<TARGET_FILE_DIR:ble_uart_client_ui>/SpaceGrotesk-Bold.ttf"
        COMMENT "Bundling the shared readable UI font"
    )

    if(UNIX AND TARGET ble_uart_server)
        add_executable(ble_uart_server_ui
            apps/ble_uart_server_ui.c
            apps/common/dashboard_dropdown.c
            apps/common/dashboard_log.c)
        add_dependencies(ble_uart_server_ui ble_uart_server)
        target_include_directories(ble_uart_server_ui PRIVATE "${raygui_SOURCE_DIR}/src")
        target_link_libraries(ble_uart_server_ui PRIVATE raylib ble_uart_app_common)
        target_compile_definitions(ble_uart_server_ui PRIVATE
            UART_DMA_UI_FONT_FILE="SpaceGrotesk-Regular.ttf"
            UART_DMA_UI_DISPLAY_FONT_FILE="SpaceGrotesk-Bold.ttf")
        target_compile_features(ble_uart_server_ui PRIVATE c_std_11)
        target_compile_definitions(ble_uart_server_ui PRIVATE _DEFAULT_SOURCE)
        target_compile_options(ble_uart_server_ui PRIVATE -Wall -Wextra)

        if(APPLE)
            set_target_properties(ble_uart_server_ui PROPERTIES
                MACOSX_BUNDLE TRUE
                OUTPUT_NAME "BLE UART Server"
                MACOSX_BUNDLE_BUNDLE_NAME "BLE UART Server"
                MACOSX_BUNDLE_GUI_IDENTIFIER "com.example.ble-uart-server"
                MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            )
            add_custom_command(TARGET ble_uart_server_ui POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:ble_uart_server>"
                        "$<TARGET_BUNDLE_DIR:ble_uart_server_ui>/Contents/MacOS/ble_uart_server_engine"
                COMMENT "Bundling the continuous FreeRTOS BLE server engine"
            )
        endif()
        add_custom_command(TARGET ble_uart_server_ui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${UART_DMA_UI_FONT_SOURCE}"
                    "$<TARGET_FILE_DIR:ble_uart_server_ui>/SpaceGrotesk-Regular.ttf"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${UART_DMA_UI_DISPLAY_FONT_SOURCE}"
                    "$<TARGET_FILE_DIR:ble_uart_server_ui>/SpaceGrotesk-Bold.ttf"
            COMMENT "Bundling the shared readable UI font"
        )
    endif()
endif()

option(UART_DMA_BUILD_APP "Build the macOS UART DMA demo application" ON)

if(APPLE AND UART_DMA_BUILD_APP)
    add_executable(uart_dma_demo_app MACOSX_BUNDLE demo/uart_dma_demo_app.m)
    add_dependencies(uart_dma_demo_app uart_dma_demo)

    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(uart_dma_demo_app PRIVATE "${COCOA_FRAMEWORK}")
    target_compile_options(uart_dma_demo_app PRIVATE
        -fobjc-arc
        -Wall
        -Wextra
        -Werror
    )

    set_target_properties(uart_dma_demo_app PROPERTIES
        OUTPUT_NAME "UART DMA Demo"
        MACOSX_BUNDLE_BUNDLE_NAME "UART DMA Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.example.uart-dma-demo"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )

    add_custom_command(TARGET uart_dma_demo_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:uart_dma_demo_app>/Contents/Resources"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:uart_dma_demo>"
                "$<TARGET_BUNDLE_DIR:uart_dma_demo_app>/Contents/Resources/uart_dma_demo_cli"
        COMMENT "Bundling the FreeRTOS UART DMA demo engine"
    )
endif()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME ble_uart_python_tests
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${CMAKE_CURRENT_SOURCE_DIR}/python/tests"
                    -p "test_*.py"
        )
    endif()
endif()

option(UART_DMA_BUILD_DOCS "Build Doxygen HTML documentation" ON)
if(UART_DMA_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    configure_file(docs/Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
    add_custom_target(docs ALL
        COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generating UART DMA API documentation"
        VERBATIM
    )

    add_dependencies(docs uart_dma)
    foreach(documented_target IN ITEMS
            uart_dma_tests uart_dma_demo ble_uart_server ble_uart_client
            ble_uart_client_ui ble_uart_server_ui uart_dma_demo_app)
        if(TARGET "${documented_target}")
            add_dependencies(docs "${documented_target}")
        endif()
    endforeach()
endif()
