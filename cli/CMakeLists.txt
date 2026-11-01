add_executable(FgcCli main.cpp)
target_link_libraries(FgcCli PRIVATE FgcCore)
target_compile_definitions(FgcCli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(FgcCli PRIVATE /W4 /permissive- /utf-8)
