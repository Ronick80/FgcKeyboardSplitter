add_library(FgcCore SHARED
    include/fgc_core.h
    src/api.cpp
    src/context.h
    src/backends/keyboard_backend.h
    src/backends/pad_backend.h
    src/backends/interception_keyboard.h
    src/backends/interception_keyboard.cpp
    src/backends/scpvbus_pad.h
    src/backends/scpvbus_pad.cpp
)

target_include_directories(FgcCore
    PUBLIC include
    PRIVATE src
)

target_compile_definitions(FgcCore PRIVATE FGC_CORE_BUILD NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(FgcCore PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(FgcCore PRIVATE setupapi version advapi32)
