# Verifies tests behavior for the Engine test suite.

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(smoke)
add_subdirectory(benchmark)
add_subdirectory(cmake_configure)

# Every test reads and writes per-user data (editor layout, recent lists,
# per-project save slots) under a directory of its own in the build tree,
# never the developer's: the platform save base follows XDG_DATA_HOME on
# Linux, APPDATA on Windows and HOME on macOS (build_save_base in
# core/src/platform_os_posix.cpp and platform_os_windows.cpp). Applied
# after every suite here is registered, so no test in any of them can miss
# it; APPEND keeps an ENVIRONMENT_MODIFICATION a suite sets itself. A test
# binary run outside ctest still uses the real directory.
if(APPLE)
    set(ENGINE_TEST_USER_DATA_VARIABLE HOME)
elseif(WIN32)
    set(ENGINE_TEST_USER_DATA_VARIABLE APPDATA)
else()
    set(ENGINE_TEST_USER_DATA_VARIABLE XDG_DATA_HOME)
endif()
get_property(test_suites DIRECTORY PROPERTY SUBDIRECTORIES)
foreach(suite IN LISTS test_suites)
    get_property(suite_tests DIRECTORY ${suite} PROPERTY TESTS)
    foreach(test_name IN LISTS suite_tests)
        set_property(TEST ${test_name} DIRECTORY ${suite} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION
            "${ENGINE_TEST_USER_DATA_VARIABLE}=set:${CMAKE_BINARY_DIR}/test_user_data/${test_name}")
    endforeach()
endforeach()
