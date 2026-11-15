add_library(ds_store STATIC
    src/store/schema.c src/store/writer.c src/store/query.c src/store/pending.c src/store/volume_info.c
    src/core/aggregate.c src/core/hardlinks.c src/store/output.c src/store/snapshot.c)
target_link_libraries(ds_store PUBLIC ds_core ds_sqlite)
target_compile_definitions(ds_store PRIVATE DS_VERSION="${PROJECT_VERSION}")
ds_c_target(ds_store)

add_library(ds_local STATIC src/session/local.c)
target_link_libraries(ds_local PUBLIC ds_core)
target_include_directories(ds_local PUBLIC src/session)
if(WIN32)
    target_link_libraries(ds_local PUBLIC advapi32)
endif()
ds_c_target(ds_local)

add_library(ds_session STATIC src/session/session.c)
target_link_libraries(ds_session PUBLIC ds_protocol ds_store ds_local)
ds_c_target(ds_session)

if(WIN32)
    enable_language(RC)
    set(ds_platform_dir src/platform/windows)
    set(ds_platform_specific ${ds_platform_dir}/path.c)
    set(ds_manifest resources/diskscope.rc)
else()
    set(ds_platform_dir src/platform/linux)
    set(ds_platform_specific ${ds_platform_dir}/mounts.c)
endif()
add_library(ds_platform STATIC ${ds_platform_dir}/walk.c ${ds_platform_dir}/metadata.c
            ${ds_platform_dir}/volume.c ${ds_platform_specific})
target_link_libraries(ds_platform PUBLIC ds_core)
ds_c_target(ds_platform)

# The worker talks over inherited pipes and does not need a Windows console.
add_executable(diskscope-worker WIN32 src/worker/main.c ${ds_manifest})
target_link_libraries(diskscope-worker PRIVATE ds_protocol ds_platform ds_store ds_local)
ds_c_target(diskscope-worker)

add_executable(diskscope-cli src/cli/main.c src/cli/scan_command.c ${ds_manifest})
target_compile_definitions(diskscope-cli PRIVATE DS_VERSION="${PROJECT_VERSION}")
target_link_libraries(diskscope-cli PRIVATE ds_session)
ds_c_target(diskscope-cli)

install(TARGETS diskscope-cli diskscope-worker RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    ds_test(unit.aggregate tests/unit/test_aggregate.c)
    target_link_libraries(test_unit_aggregate PRIVATE ds_store)
    ds_test(integration.store tests/integration/test_store.c)
    target_link_libraries(test_integration_store PRIVATE ds_store)
    add_executable(test_integration_cli tests/integration/test_cli.c)
    target_link_libraries(test_integration_cli PRIVATE ds_store)
    ds_c_target(test_integration_cli)
    add_test(NAME integration.cli COMMAND test_integration_cli $<TARGET_FILE:diskscope-cli>)
    set_tests_properties(integration.cli PROPERTIES TIMEOUT 60)
endif()
