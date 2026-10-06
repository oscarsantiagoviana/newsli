cmake_minimum_required(VERSION 3.24)
project(new-sli LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# new-sli — DLSS-NR (neural denoise) offload to a second GPU, composed
# around the game's own DLSS Super Resolution at Present (same-frame).
#
# BUILD LAYOUT RULE (learned the hard way): the host proxy DLLs (dxgi.dll,
# nvngx.dll) must NOT live in the same directory as the tools/tests — any exe
# there loads OUR dxgi.dll instead of the system one and dies pre-main with
# 0xC0000409 (no exports to forward). Deployables go to build/deploy/.
#
# Targets:
#   nvngx      (DLL)  NGX proxy dropped next to the game (load vector: dxgi.dll)
#   sli_dxgi   (DLL)  load vector that forwards real dxgi and loads nvngx
#   sli_engine (EXE)  NR worker on the secondary GPU (gain-only output)
#   sli_panel  (EXE)  ImGui control panel (Win32 + DX11)
#   sli_ctl    (EXE)  CLI tuning setter
#   replay_feeder (EXE) offline harness: feeds real captures through the ABI
#                     into a locally spawned engine (no game required)
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Treat warnings as errors in our own code (protocol P5); third-party (imgui)
# is compiled with warnings allowed via SYSTEM includes.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /EHsc")
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/WX> $<$<COMPILE_LANGUAGE:CXX>:/analyze->)
# /analyze- disables the optional static analyzer pass that requires the
# analyzer components; MSVC core /W4 /WX stays enforced.

# Windows SDK paths (BuildTools layout on this machine; override with
# -DSDK_INC=/ -DSDK_LIB= if it moves).
if(NOT DEFINED SDK_ROOT)
  set(SDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
endif()
if(NOT DEFINED SDK_VER)
  set(SDK_VER 10.0.26100.0)
endif()

# MSVC toolchain is located by CMake's own detection (Visual Studio BuildTools
# generator probing); we only pin the HLSL compiler explicitly.
find_program(DXC_EXE dxc
  HINTS "${SDK_ROOT}/bin/${SDK_VER}/x64/dxc.exe"
        "$ENV{WindowsSdkVerBinPath}/x64/dxc.exe")
if(NOT DXC_EXE)
  message(FATAL_ERROR "dxc.exe not found — HLSL compilation requires the Windows SDK")
endif()

# ---------------------------------------------------------------------------
# Shared module: single-source ABI (scalars/handshake/tuning), ctl protocol,
# logging with rotation, crash handler.
# ---------------------------------------------------------------------------
add_library(sli_shared INTERFACE)
target_include_directories(sli_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Embedded shader headers (dxc custom command → cso → C header).
# embed_shader = compute kernel (cs_6_0); embed_ps_shader = pixel shader
# (ps_6_0) — the Present-gate compose stage is a fixed-function draw,
# not a dispatch.
# ---------------------------------------------------------------------------
function(embed_shader TARGET HLSL ENTRY VARIABLE)
  set(HDR ${CMAKE_CURRENT_BINARY_DIR}/${VARIABLE}_cso.h)
  add_custom_command(
    OUTPUT ${HDR}
    COMMAND ${DXC_EXE} -T cs_6_0 -E ${ENTRY} -O3 -HV 2021
            -Fh ${HDR} -Vn ${VARIABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${HLSL}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${HLSL}
    COMMENT "dxc ${HLSL}:${ENTRY}")
  target_sources(${TARGET} PRIVATE ${HDR})
  target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  set_property(TARGET ${TARGET} APPEND PROPERTY OBJECT_DEPENDS ${HDR})
endfunction()

function(embed_ps_shader TARGET HLSL ENTRY VARIABLE)
  set(HDR ${CMAKE_CURRENT_BINARY_DIR}/${VARIABLE}_cso.h)
  add_custom_command(
    OUTPUT ${HDR}
    COMMAND ${DXC_EXE} -T ps_6_0 -E ${ENTRY} -O3 -HV 2021
            -Fh ${HDR} -Vn ${VARIABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${HLSL}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${HLSL}
    COMMENT "dxc ${HLSL}:${ENTRY} (ps)")
  target_sources(${TARGET} PRIVATE ${HDR})
  target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  set_property(TARGET ${TARGET} APPEND PROPERTY OBJECT_DEPENDS ${HDR})
endfunction()

function(embed_vs_shader TARGET HLSL ENTRY VARIABLE)
  set(HDR ${CMAKE_CURRENT_BINARY_DIR}/${VARIABLE}_cso.h)
  add_custom_command(
    OUTPUT ${HDR}
    COMMAND ${DXC_EXE} -T vs_6_0 -E ${ENTRY} -O3 -HV 2021
            -Fh ${HDR} -Vn ${VARIABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${HLSL}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${HLSL}
    COMMENT "dxc ${HLSL}:${ENTRY} (vs)")
  target_sources(${TARGET} PRIVATE ${HDR})
  target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  set_property(TARGET ${TARGET} APPEND PROPERTY OBJECT_DEPENDS ${HDR})
endfunction()

# ---------------------------------------------------------------------------
# nvngx proxy DLL
# ---------------------------------------------------------------------------
add_library(nvngx SHARED
  src/host/nvngx_host.cpp
  src/host/offload_session.cpp
  src/host/nvngx.def
  src/shared/log.cpp)
target_link_libraries(nvngx PRIVATE dbghelp)
target_link_options(nvngx PRIVATE "/MAP:$<TARGET_FILE_DIR:nvngx>/nvngx.map")
target_link_libraries(nvngx PRIVATE d3d12 dxgi user32 advapi32 psapi)
target_include_directories(nvngx PRIVATE src external/nvngx_dlss_sdk)
# The Present-gate compose stage (R79): a pixel-shader fullscreen-triangle
# additive draw on the swapchain backbuffer. The load hook stays OUT of
# this target: it belongs to sli_dxgi only.
embed_ps_shader(nvngx shaders/present_add.hlsl PSMain g_presentAddPso)
embed_vs_shader(nvngx shaders/present_add.hlsl VSMain g_presentAddVs)
set_target_properties(nvngx PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/deploy)

# ---------------------------------------------------------------------------
# dxgi load-vector DLL
# ---------------------------------------------------------------------------
add_library(sli_dxgi SHARED
  src/host/dxgi_proxy.cpp
  src/host/load_hook.cpp
  src/shared/log.cpp
  src/host/dxgi.def)
set_target_properties(sli_dxgi PROPERTIES OUTPUT_NAME "dxgi" ARCHIVE_OUTPUT_NAME "sli_dxgi")
target_link_libraries(sli_dxgi PRIVATE d3d12 dxgi user32 ole32)
target_include_directories(sli_dxgi PRIVATE src external/detours)
target_link_libraries(sli_dxgi PRIVATE "${CMAKE_SOURCE_DIR}/external/detours/detours.lib" dbghelp)
target_include_directories(sli_dxgi PRIVATE src external/nvngx_dlss_sdk)
set_target_properties(sli_dxgi PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/deploy)

# ---------------------------------------------------------------------------
# NR engine (worker on GPU 2)
# ---------------------------------------------------------------------------
add_executable(sli_engine
  src/engine/main.cpp
  src/engine/loop.cpp
  src/shared/log.cpp)
target_link_libraries(sli_engine PRIVATE dbghelp)
target_link_libraries(sli_engine PRIVATE d3d12 dxgi user32 advapi32)
target_include_directories(sli_engine PRIVATE src external/nvngx_dlss_sdk)
# Embedded compute shaders: HDR->BGRA8 encode + display-domain delta decode.
embed_shader(sli_engine shaders/nr_encode.hlsl CSMain g_nrEncodeCso)
embed_shader(sli_engine shaders/nr_delta.hlsl CSDecodeDelta g_nrDeltaCso)
embed_shader(sli_engine shaders/nr_stabdepth.hlsl CSMain g_nrStabDepthCso)
set_target_properties(sli_engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/deploy)

# ---------------------------------------------------------------------------
# Control panel (ImGui + Win32 + DX11)
# ---------------------------------------------------------------------------
add_executable(sli_panel WIN32
  src/panel/panel.cpp
  src/panel/panel_state.cpp
  src/panel/panel_host.cpp
  src/panel/panel_ui.cpp
  src/shared/log.cpp
  external/imgui/imgui.cpp
  external/imgui/imgui_draw.cpp
  external/imgui/imgui_tables.cpp
  external/imgui/imgui_widgets.cpp
  external/imgui/imgui_demo.cpp
  external/imgui/backends/imgui_impl_win32.cpp
  external/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(sli_panel SYSTEM PRIVATE external/imgui external/imgui/backends)
target_include_directories(sli_panel PRIVATE src)
target_link_libraries(sli_panel PRIVATE d3d11 dxgi d2d1 dwmapi user32 gdi32 dbghelp)
set_target_properties(sli_panel PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/deploy)

# ---------------------------------------------------------------------------
# (R95: the sli_viewer target is gone — the NR-view window process was the
#  pre-SR VIEW line's painter, extirpated with the mode.)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# CLI tuning tool
# ---------------------------------------------------------------------------
add_executable(sli_ctl src/tools/ctl.cpp)
target_include_directories(sli_ctl PRIVATE src)
target_link_libraries(sli_ctl PRIVATE user32)

# ---------------------------------------------------------------------------
# Offline replay harness
# ---------------------------------------------------------------------------
add_executable(replay_feeder src/tools/replay_feeder.cpp)
target_link_libraries(replay_feeder PRIVATE d3d12 dxgi user32)
target_include_directories(replay_feeder PRIVATE src)

# ---------------------------------------------------------------------------
# Host smoke gate (F3): lifecycle + ctl protocol against the real session
# code with a mock evaluate (no game / no NGX core required).
# ---------------------------------------------------------------------------
add_executable(host_smoke
  src/tools/host_smoke.cpp
  src/host/offload_session.cpp
  src/shared/log.cpp)
target_link_libraries(host_smoke PRIVATE dbghelp d3d12 dxgi user32 advapi32 psapi)
target_include_directories(host_smoke PRIVATE src ${CMAKE_BINARY_DIR} external/nvngx_dlss_sdk)
embed_ps_shader(host_smoke shaders/present_add.hlsl PSMain g_presentAddPso)
embed_vs_shader(host_smoke shaders/present_add.hlsl VSMain g_presentAddVs)

enable_testing()
add_test(NAME host_smoke COMMAND host_smoke)


# ---------------------------------------------------------------------------
# Unit tests (F1): log rotation (must pass on its own asserts) + crash
# handler probe (its deliberate AV IS the test; WILL_FAIL is scoped to that
# target only, so an assertion failure in rotation can no longer hide
# behind the inverted result — R82l).
# ---------------------------------------------------------------------------
add_executable(t_log src/tools/test_log.cpp src/shared/log.cpp)
target_link_libraries(t_log PRIVATE dbghelp)
target_include_directories(t_log PRIVATE src)
target_compile_definitions(t_log PRIVATE T_LOG_NO_CRASH)
add_test(NAME log_rotation COMMAND t_log)

add_executable(t_crash src/tools/test_log.cpp src/shared/log.cpp)
target_link_libraries(t_crash PRIVATE dbghelp)
target_include_directories(t_crash PRIVATE src)
# the test IS the crash: the wrapper runs it and passes only when the
# filter leaves the minidump behind (a silent death FAILS)
add_test(NAME crash_handler
  COMMAND ${CMAKE_COMMAND}
          -DPROG=$<TARGET_FILE:t_crash>
          -DLOGDIR=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunCrashTest.cmake)

# ---------------------------------------------------------------------------
# Installer (single self-contained exe). The deployables are embedded as
# RCDATA at build time (installer_payload.rc, generated below) so ONE exe
# carries dxgi/nvngx/engine/panel/ctl + the default sli.ini. The ~165 MB
# vendor NR runtime is deliberately NOT embedded (user-provided; it is
# copied from beside the installer when present).
# ---------------------------------------------------------------------------
set(INSTALLER_RC ${CMAKE_CURRENT_BINARY_DIR}/installer_payload.rc)
add_custom_command(
  OUTPUT ${INSTALLER_RC}
  COMMAND ${CMAKE_COMMAND}
          -DOUT=${INSTALLER_RC}
          -DDXGI=$<TARGET_FILE:sli_dxgi>
          -DNVNGX=$<TARGET_FILE:nvngx>
          -DENGINE=$<TARGET_FILE:sli_engine>
          -DPANEL=$<TARGET_FILE:sli_panel>
          -DCTL=$<TARGET_FILE:sli_ctl>
          -DINI=${CMAKE_CURRENT_SOURCE_DIR}/tools/sli.default.ini
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MakeInstallerPayload.cmake
  DEPENDS sli_dxgi nvngx sli_engine sli_panel sli_ctl
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/sli.default.ini
  COMMENT "generating installer payload (RCDATA)")
set_source_files_properties(${INSTALLER_RC} PROPERTIES GENERATED ON)

add_executable(sli_installer WIN32 src/tools/installer.cpp ${INSTALLER_RC})
target_link_libraries(sli_installer PRIVATE shell32 advapi32 ole32 user32)
set_target_properties(sli_installer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/package)

# console twin of the installer — same payload, stdout + the --smoke gate
# (headless install+uninstall into a temp dir; ctest runs it). Console
# subsystem + wWinMain needs the explicit entry point.
add_executable(sli_installer_cmd src/tools/installer.cpp ${INSTALLER_RC})
target_compile_definitions(sli_installer_cmd PRIVATE INSTALLER_CONSOLE)
target_link_options(sli_installer_cmd PRIVATE /ENTRY:wWinMainCRTStartup)
target_link_libraries(sli_installer_cmd PRIVATE shell32 advapi32 ole32 user32)

add_test(NAME installer_smoke
  COMMAND ${CMAKE_COMMAND}
          -DPROG=$<TARGET_FILE:sli_installer_cmd>
          -DDIR=${CMAKE_CURRENT_BINARY_DIR}/installer_smoke
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunInstallerSmoke.cmake)

# 'package' convenience target: the installer plus (when present on this
# machine) the user-provided vendor runtime, in build/package/ — ready to
# zip and ship. The vendor DLLs are NEVER part of the repo.
add_custom_target(package
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sli_installer>
          ${CMAKE_BINARY_DIR}/package/sli_installer.exe
  COMMAND ${CMAKE_COMMAND}
          -DPKG=${CMAKE_BINARY_DIR}/package
          -PSRC=${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyVendorRuntime.cmake
  DEPENDS sli_installer sli_dxgi nvngx sli_engine sli_panel sli_ctl
  COMMENT "packaging: sli_installer.exe (+ vendor runtime if present)")

