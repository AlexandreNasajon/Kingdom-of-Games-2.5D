cmake_minimum_required(VERSION 3.16)
project(KOG25DEditor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
cmake_policy(SET CMP0135 NEW)   # use extraction time for downloaded archives

# ── raylib 5.5 (zip download – no git clone needed) ──────────────────────────
FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.zip
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

# ── Dear ImGui v1.91.6 (zip download) ────────────────────────────────────────
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.zip
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR})

# ── rlImGui – raylib ↔ ImGui bridge (pinned to last commit compatible with ImGui 1.91.x)
FetchContent_Declare(
    rlimgui
    URL https://github.com/raylib-extras/rlImGui/archive/ceceffd5e8148d4781f5f58cffe1641cf50f8f68.zip
)
FetchContent_MakeAvailable(rlimgui)

add_library(rlimgui_lib STATIC
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp
)
target_include_directories(rlimgui_lib PUBLIC ${rlimgui_SOURCE_DIR})
target_link_libraries(rlimgui_lib PUBLIC raylib imgui_lib)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(KOG25DEditor src/main.cpp)

if(MSVC)
    target_compile_options(KOG25DEditor PRIVATE /wd4576)
endif()

target_include_directories(KOG25DEditor PRIVATE
    ${imgui_SOURCE_DIR}
    ${rlimgui_SOURCE_DIR}
)
target_link_libraries(KOG25DEditor PRIVATE raylib imgui_lib rlimgui_lib)

if(WIN32)
    target_link_libraries(KOG25DEditor PRIVATE winmm)
endif()

# ── Post-build: copy assets & create maps dir ────────────────────────────────
add_custom_command(TARGET KOG25DEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:KOG25DEditor>/assets
    COMMENT "Copying assets..."
)
add_custom_command(TARGET KOG25DEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:KOG25DEditor>/maps
    COMMENT "Creating maps dir..."
)
