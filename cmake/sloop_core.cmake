# SPDX-License-Identifier: GPL-3.0-only
# sloopy-sloop-32: the SLOOP core's sources, include directories and generated headers.
# Shared by the native host build (host/CMakeLists.txt) and the ESP-IDF component
# (components/sloop_core/CMakeLists.txt), so both compile exactly the same SLOOP.
#
# After include():
#   SLOOP_PROJECT_ROOT, SLOOP_UPSTREAM_DIR, SLOOP_CORE_DIR
#   sloop_core_prepare(<gen_dir> <commit_var> <include_dirs_var>)
#     runs upstream's header generators (tools/gen_*.py: font, icons, tables, samples, drum kits,
#     logo) into <gen_dir> when the upstream checkout changed, and returns the upstream commit and
#     the private include directories of sloop_unity.c (port HAL first).

get_filename_component(SLOOP_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT SLOOP_UPSTREAM_DIR)
    set(SLOOP_UPSTREAM_DIR "${SLOOP_PROJECT_ROOT}/upstream/sloop-fm1")
endif()
set(SLOOP_CORE_DIR "${SLOOP_PROJECT_ROOT}/components/sloop_core")

# upstream's generators need Pillow; the ESP-IDF Python environment usually has none, so look
# for an interpreter that does (SLOOP_PYTHON overrides)
function(_sloop_find_python out)
    set(cands "$ENV{SLOOP_PYTHON}")
    find_program(_sloop_py3 NAMES python3 NO_CACHE)
    list(APPEND cands "${_sloop_py3}" /usr/bin/python3 /usr/local/bin/python3)
    foreach(py IN LISTS cands)
        if(py AND EXISTS "${py}")
            execute_process(COMMAND "${py}" -c "import PIL" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
            if(rc EQUAL 0)
                set(${out} "${py}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
    message(FATAL_ERROR "sloop_core: no Python 3 with Pillow found (upstream's asset generators need it).\n"
                        "Install it (Nobara/Fedora: sudo dnf install python3-pillow) or set SLOOP_PYTHON.")
endfunction()

function(sloop_core_prepare gen_dir commit_var incs_var)
    if(NOT EXISTS "${SLOOP_UPSTREAM_DIR}/firmware/src/felucca.c")
        message(FATAL_ERROR "sloop_core: upstream SLOOP not found in ${SLOOP_UPSTREAM_DIR}.\n"
                            "Run scripts/fetch-sloop.sh first.")
    endif()
    execute_process(COMMAND git -C "${SLOOP_UPSTREAM_DIR}" rev-parse HEAD
                    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0)
        set(commit "unknown")
    endif()
    execute_process(COMMAND git -C "${SLOOP_UPSTREAM_DIR}" status --porcelain
                    OUTPUT_VARIABLE dirty RESULT_VARIABLE rc ERROR_QUIET)
    string(MD5 dirty_md5 "${dirty}")
    # the generators and their inputs are part of the checkout: commit + local changes identify them
    set(stamp "${commit} ${dirty_md5}")
    set(gens font:felucca_font icons:felucca_icons tables:felucca_tables samples:felucca_samples
             drumkits:felucca_drumkits logo:sloop_logo)
    set(need FALSE)
    if(EXISTS "${gen_dir}/stamp.txt")
        file(READ "${gen_dir}/stamp.txt" old)
        if(NOT old STREQUAL stamp)
            set(need TRUE)
        endif()
    else()
        set(need TRUE)
    endif()
    foreach(g IN LISTS gens)
        string(REPLACE ":" ";" g "${g}")
        list(GET g 1 hdr)
        if(NOT EXISTS "${gen_dir}/${hdr}.h")
            set(need TRUE)
        endif()
    endforeach()
    if(need)
        _sloop_find_python(py)
        file(MAKE_DIRECTORY "${gen_dir}")
        message(STATUS "sloop_core: generating SLOOP headers (upstream ${commit}) with ${py}")
        foreach(g IN LISTS gens)
            string(REPLACE ":" ";" g "${g}")
            list(GET g 0 tool)
            list(GET g 1 hdr)
            execute_process(COMMAND "${py}" "${SLOOP_UPSTREAM_DIR}/tools/gen_${tool}.py" "${gen_dir}/${hdr}.h"
                            WORKING_DIRECTORY "${SLOOP_UPSTREAM_DIR}"
                            RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "sloop_core: tools/gen_${tool}.py failed:\n${out}\n${err}")
            endif()
        endforeach()
        file(WRITE "${gen_dir}/stamp.txt" "${stamp}")
    endif()
    # rerun cmake (and the check above) when upstream moves to another commit
    if(EXISTS "${SLOOP_UPSTREAM_DIR}/.git/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SLOOP_UPSTREAM_DIR}/.git/HEAD")
    endif()
    string(SUBSTRING "${commit}" 0 12 short)
    set(${commit_var} "${short}" PARENT_SCOPE)
    set(${incs_var}
        "${SLOOP_CORE_DIR}/port/hal"            # the virtual FM-1 HAL (replaces firmware/hal)
        "${SLOOP_CORE_DIR}/port"
        "${SLOOP_UPSTREAM_DIR}/firmware/src"    # upstream SLOOP, unmodified
        "${gen_dir}"                            # upstream's generated headers
        PARENT_SCOPE)
endfunction()

# SLOOP's web editor (upstream web/editor.html, GPL-3.0) for the panel's server: the same file with
# one line added to its <head>, the bridge that carries its Web MIDI SysEx over the WebSocket
# (web/editor-bridge.js), plus its icon font (Fukiai, MIT, with its license). Written to <out_dir>.
function(sloop_editor_prepare out_dir)
    set(src "${SLOOP_UPSTREAM_DIR}/web/editor.html")
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR "sloop_core: ${src} not found (scripts/fetch-sloop.sh)")
    endif()
    file(READ "${src}" html)
    string(FIND "${html}" "<meta charset=\"utf-8\">" at)
    if(at LESS 0)
        message(FATAL_ERROR "sloop_core: upstream editor.html has no <meta charset=\"utf-8\"> to add the bridge after")
    endif()
    string(REPLACE "<meta charset=\"utf-8\">"
           "<meta charset=\"utf-8\">\n<script src=\"editor-bridge.js\"></script><!-- sloopy-sloop-32: Web MIDI over the WebSocket -->"
           html "${html}")
    file(MAKE_DIRECTORY "${out_dir}")
    file(WRITE "${out_dir}/editor.html.tmp" "${html}")
    file(COPY_FILE "${out_dir}/editor.html.tmp" "${out_dir}/editor.html" ONLY_IF_DIFFERENT)
    file(REMOVE "${out_dir}/editor.html.tmp")
    file(COPY_FILE "${SLOOP_UPSTREAM_DIR}/web/fukiai.ttf" "${out_dir}/fukiai.ttf" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${SLOOP_UPSTREAM_DIR}/web/FUKIAI-LICENSE.txt" "${out_dir}/FUKIAI-LICENSE.txt" ONLY_IF_DIFFERENT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
endfunction()
