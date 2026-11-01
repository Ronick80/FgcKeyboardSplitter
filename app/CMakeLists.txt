include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE NOMINMAX)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

add_executable(FgcApp WIN32
    src/main.cpp
    src/ui.cpp
    src/ui.h
)
target_link_libraries(FgcApp PRIVATE FgcCore imgui fgc_version)
fgc_add_version_resource(FgcApp VFT_APP "FgcApp.exe" "${FGC_PROJECT_NAME}")
target_compile_options(FgcApp PRIVATE /W4 /permissive- /utf-8)

# The Drivers section of the UI points at this folder.
add_custom_command(TARGET FgcApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/drivers" "$<TARGET_FILE_DIR:FgcApp>/drivers"
)
