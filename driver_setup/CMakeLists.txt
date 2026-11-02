option(FGC_DRIVER_SETUP_REQUIRE_ADMIN "Make FgcDriverSetup.exe ask for administrator rights when it starts" ON)

add_executable(FgcDriverSetup
    main.cpp
    diagnostics.h
    diagnostics.cpp
    log.h
    log.cpp
    scpvbus_installer.h
    scpvbus_installer.cpp
    interception_installer.h
    interception_installer.cpp
)
target_link_libraries(FgcDriverSetup PRIVATE FgcCore fgc_version setupapi newdev cfgmgr32 advapi32 winhttp bcrypt shell32 ole32)
fgc_add_version_resource(FgcDriverSetup VFT_APP "FgcDriverSetup.exe" "${FGC_PROJECT_NAME} driver setup")
target_compile_definitions(FgcDriverSetup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(FgcDriverSetup PRIVATE /W4 /permissive- /utf-8)

if(FGC_DRIVER_SETUP_REQUIRE_ADMIN)
    target_link_options(FgcDriverSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

# The tool looks for the ScpVBus files in a drivers folder next to it.
add_custom_command(TARGET FgcDriverSetup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/drivers" "$<TARGET_FILE_DIR:FgcDriverSetup>/drivers"
)
