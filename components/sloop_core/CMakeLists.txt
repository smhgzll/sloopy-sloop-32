# SPDX-License-Identifier: GPL-3.0-only
# The SLOOP core as an ESP-IDF component: upstream SLOOP compiled unmodified against the
# virtual FM-1 HAL (port/hal), exactly as the host build does (cmake/sloop_core.cmake).
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(INCLUDE_DIRS include)
    return()
endif()

include(${COMPONENT_DIR}/../../cmake/sloop_core.cmake)
sloop_core_prepare(${CMAKE_BINARY_DIR}/sloop_gen SLOOP_COMMIT SLOOP_CORE_PRIV_INCS)

idf_component_register(
    SRCS "port/sloop_unity.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${SLOOP_CORE_PRIV_INCS}
    LDFRAGMENTS "linker.lf"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SLOOP_UPSTREAM_COMMIT="${SLOOP_COMMIT}")
# upstream builds with -Wall -Wno-unused-function (tools/build.py)
# (and leaves trailing fields of its designated initialisers implicit: ESP-IDF builds with -Wextra)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-missing-field-initializers)
