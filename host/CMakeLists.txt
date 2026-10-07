# SPDX-License-Identifier: GPL-3.0-only
# sloopy-sloop-32: native Linux build of the SLOOP core (the same sources as the ESP32 firmware),
# the host app (web panel + audio) and the port's host tests.
#   cmake -S host -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host
cmake_minimum_required(VERSION 3.16)
project(sloopy_host C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/sloop_core.cmake)
sloop_core_prepare(${CMAKE_BINARY_DIR}/sloop_gen SLOOP_COMMIT SLOOP_CORE_PRIV_INCS)

find_package(Threads REQUIRED)

# ---- the SLOOP core (upstream SLOOP + the virtual FM-1 HAL), one compilation unit
add_library(sloop_core STATIC ${SLOOP_CORE_DIR}/port/sloop_unity.c)
target_include_directories(sloop_core PUBLIC ${SLOOP_CORE_DIR}/include PRIVATE ${SLOOP_CORE_PRIV_INCS})
target_compile_definitions(sloop_core PRIVATE SLOOP_UPSTREAM_COMMIT="${SLOOP_COMMIT}")
# upstream builds with -Wall -Wno-unused-function (tools/build.py); its fm1_xip.h (pulled in by
# eng_sample.c, unused by the port) casts a 32-bit address to a pointer, harmless only on 64-bit
target_compile_options(sloop_core PRIVATE -O2 -Wall -Wno-unused-function -Wno-int-to-pointer-cast)
target_link_libraries(sloop_core PUBLIC m)

# ---- the Linux platform
add_library(sloop_platform_host STATIC platform_host.c)
target_include_directories(sloop_platform_host PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${SLOOP_CORE_DIR}/include)
target_compile_options(sloop_platform_host PRIVATE -Wall -Wextra)
target_link_libraries(sloop_platform_host PUBLIC Threads::Threads)

# ---- the web panel protocol (shared with the ESP32 web server)
add_library(sloop_proto STATIC ${SLOOP_PROJECT_ROOT}/components/sloop_proto/sloop_proto.c)
target_include_directories(sloop_proto PUBLIC ${SLOOP_PROJECT_ROOT}/components/sloop_proto/include)
target_compile_options(sloop_proto PRIVATE -Wall -Wextra)
target_link_libraries(sloop_proto PUBLIC sloop_core)

# ---- the USB-MIDI device's bridge (portable half of components/usb_midi)
add_library(usb_midi_bridge STATIC ${SLOOP_PROJECT_ROOT}/components/usb_midi/usb_midi_bridge.c)
target_include_directories(usb_midi_bridge PUBLIC ${SLOOP_PROJECT_ROOT}/components/usb_midi/include)
target_compile_options(usb_midi_bridge PRIVATE -Wall -Wextra)
target_link_libraries(usb_midi_bridge PUBLIC sloop_core)

# ---- the host app: SLOOP + the web panel server + audio (ALSA when available)
# (the upstream web editor, served next to the panel: build-host/web-gen)
sloop_editor_prepare(${CMAKE_BINARY_DIR}/web-gen)
find_package(ALSA)
add_executable(sloop_host sloop_host.c ws_server.c)
target_compile_options(sloop_host PRIVATE -Wall -Wextra)
target_compile_definitions(sloop_host PRIVATE SLOOPY_WEB_GEN="${CMAKE_BINARY_DIR}/web-gen")
target_link_libraries(sloop_host PRIVATE sloop_proto sloop_core sloop_platform_host)
if(ALSA_FOUND)
    target_compile_definitions(sloop_host PRIVATE SLOOPY_HAVE_ALSA=1)
    target_link_libraries(sloop_host PRIVATE ALSA::ALSA)
else()
    message(STATUS "ALSA not found (alsa-lib-devel): sloop_host plays to the null output only")
endif()

# ---- host tests of the port
enable_testing()
set(SLOOP_TEST_OUT ${CMAKE_BINARY_DIR}/out)
file(MAKE_DIRECTORY ${SLOOP_TEST_OUT})
function(sloop_host_test name)
    add_executable(${name} ${SLOOP_PROJECT_ROOT}/tests/host/${name}.c)
    target_include_directories(${name} PRIVATE ${SLOOP_PROJECT_ROOT}/tests/host)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-function)
    target_link_libraries(${name} PRIVATE sloop_proto sloop_core sloop_platform_host)
    add_test(NAME ${name} COMMAND ${name} ${SLOOP_TEST_OUT})
endfunction()
sloop_host_test(test_port_boot)
sloop_host_test(test_port_selftest)
sloop_host_test(test_proto)
sloop_host_test(test_port_features)
sloop_host_test(test_port_soak)
target_compile_definitions(test_port_features PRIVATE SLOOP_TESTS_DIR="${SLOOP_PROJECT_ROOT}/tests/host")
sloop_host_test(test_store_backup)
sloop_host_test(test_usb_midi)
target_link_libraries(test_usb_midi PRIVATE usb_midi_bridge)
sloop_host_test(test_ws_server)
target_sources(test_ws_server PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ws_server.c)
# end to end: sloop_host + a WebSocket client speaking the panel protocol (tests/web)
find_program(SLOOP_PY3 python3)
if(SLOOP_PY3)
    add_test(NAME web_protocol_e2e COMMAND ${SLOOP_PY3} ${SLOOP_PROJECT_ROOT}/tests/web/test_ws_e2e.py
             --host-app $<TARGET_FILE:sloop_host> --web ${SLOOP_PROJECT_ROOT}/web)
endif()
find_program(SLOOP_NODE node)
if(SLOOP_NODE)
    add_test(NAME web_panel COMMAND ${SLOOP_NODE} ${SLOOP_PROJECT_ROOT}/tests/web/test_panel.mjs)
endif()
if(SLOOP_PY3)
    # the panel and SLOOP's editor in headless Chrome (skips without Chrome / Chromium)
    add_test(NAME web_browser COMMAND ${SLOOP_PY3} ${SLOOP_PROJECT_ROOT}/tests/web/test_browser.py
             --host-app $<TARGET_FILE:sloop_host> --web ${SLOOP_PROJECT_ROOT}/web)
endif()
