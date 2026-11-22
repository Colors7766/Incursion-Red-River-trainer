cmake_minimum_required(VERSION 3.20)
project(PQOverlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(IMGUI third_party/imgui)
set(MINHOOK third_party/minhook)

# Keep shared layouts consistent even if localized MSVC /showIncludes is not parsed.
set_source_files_properties(src/game.cpp src/dllmain.cpp src/weapon_hooks.cpp
    PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game.h")

add_library(PQOverlay SHARED
    src/dllmain.cpp src/game.cpp src/injury_protection.cpp src/weapon_hooks.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp ${IMGUI}/backends/imgui_impl_win32.cpp
    ${MINHOOK}/src/buffer.c ${MINHOOK}/src/hook.c ${MINHOOK}/src/trampoline.c ${MINHOOK}/src/hde/hde64.c)
target_include_directories(PQOverlay PRIVATE ${IMGUI} ${IMGUI}/backends ${MINHOOK}/include ${MINHOOK}/src)
target_compile_definitions(PQOverlay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(PQOverlay PRIVATE /utf-8)
target_link_libraries(PQOverlay PRIVATE d3d11 dxgi dcomp user32 gdi32)

add_executable(PQLoader src/loader.cpp)
target_compile_definitions(PQLoader PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(PQLoader PRIVATE /utf-8)
target_link_libraries(PQLoader PRIVATE user32)
