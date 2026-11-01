include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(FgcCore SHARED
    include/fgc_core.h
    src/api.cpp
    src/context.h
    src/profiles/profile_store.h
    src/profiles/profile_store.cpp
    src/backends/keyboard_backend.h
    src/backends/pad_backend.h
    src/backends/interception_keyboard.h
    src/backends/interception_keyboard.cpp
    src/backends/scpvbus_pad.h
    src/backends/scpvbus_pad.cpp
    src/backends/controller_counter.h
    src/backends/controller_counter.cpp
    src/engine/controller_limit.h
    src/engine/controller_limit.cpp
    src/engine/event_queue.h
    src/engine/mapping_session.h
    src/engine/mapping_session.cpp
    src/engine/mapping_table.h
    src/engine/mapping_table.cpp
    src/engine/pad_state.h
    src/engine/pad_state.cpp
)

target_include_directories(FgcCore
    PUBLIC include
    PRIVATE src
)

target_compile_definitions(FgcCore PRIVATE FGC_CORE_BUILD NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(FgcCore PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(FgcCore PRIVATE nlohmann_json::nlohmann_json setupapi version advapi32 avrt shell32 ole32 xinput)
