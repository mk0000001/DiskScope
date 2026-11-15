target_sources(ds_core PRIVATE src/core/treemap.c)
if(BUILD_TESTING)
    ds_test(unit.treemap tests/unit/test_treemap.c)
endif()
