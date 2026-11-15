find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.80 gio-2.0>=2.80)
pkg_check_modules(JSON REQUIRED IMPORTED_TARGET json-glib-1.0>=1.8)
if(DS_GUI)
    pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4>=4.14 cairo>=1.18)
endif()

include(FetchContent)
FetchContent_Declare(sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(sqlite)
add_library(ds_sqlite STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(ds_sqlite SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(ds_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0 SQLITE_DEFAULT_TRUSTED_SCHEMA=0 SQLITE_TEMP_STORE=2)
find_package(Threads REQUIRED)
target_link_libraries(ds_sqlite PUBLIC Threads::Threads)
if(UNIX)
    target_compile_definitions(ds_sqlite PRIVATE _GNU_SOURCE)
    target_link_libraries(ds_sqlite PUBLIC m)
endif()
