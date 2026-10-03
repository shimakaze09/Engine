# Configure-only probes for ENGINE_TARGET_PLATFORM's support-matrix gate
# (audit #178): each test re-invokes CMake configure as a fresh subprocess
# with a specific platform override and asserts it is rejected. No compiler
# ever runs and no FetchContent network cost is paid — the root
# CMakeLists.txt rejects every case here BEFORE project(), so the probes
# skip compiler detection entirely and fail (correctly) in well under a
# second even on loaded Windows CI runners (whose per-probe compiler
# detection previously pushed these past their timeout).
# The positive case (the host's own native platform configures cleanly) is
# already proven by every other test in this suite, which all depend on
# this same top-level configure having succeeded — a redundant full
# re-configure here would only add a second expensive SDL3 fetch/build.

function(engine_add_configure_reject_probe name target_platform)
    set(_probe_binary_dir "${CMAKE_BINARY_DIR}/cmake_configure_probe/${name}")
    add_test(
        NAME cmake_configure_${name}
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR}
            -B ${_probe_binary_dir}
            -DENGINE_TARGET_PLATFORM=${target_platform}
            -DENGINE_BUILD_TESTS=OFF
            -DENGINE_BUILD_TOOLS=OFF
    )
    set_tests_properties(cmake_configure_${name} PROPERTIES
        WILL_FAIL TRUE
        LABELS "cmake_configure"
        TIMEOUT 60
    )
endfunction()

# No toolchain, renderer backend, or dependency graph exists for these yet
# (gated on the v0.5 bgfx replatform); configure must fail fast rather than
# silently proceed into a build that cannot succeed.
engine_add_configure_reject_probe(rejects_android Android)
engine_add_configure_reject_probe(rejects_ios iOS)
engine_add_configure_reject_probe(rejects_web Web)

# A target platform that does not match the host CMake is actually running
# on (with no cross-compilation toolchain supplied) is always a host/target
# mistake in this natively-built-only repo, not a supported combination.
if(WIN32)
    engine_add_configure_reject_probe(rejects_host_mismatch Linux)
elseif(APPLE)
    engine_add_configure_reject_probe(rejects_host_mismatch Win64)
else()
    engine_add_configure_reject_probe(rejects_host_mismatch Win64)
endif()

# An outright invalid value must still be rejected too (pre-existing
# behavior; covered here so the whole support-matrix gate has one home).
engine_add_configure_reject_probe(rejects_unknown_value NintendoSwitch)

# Tests are opt-in: the option defaults OFF, every user
# preset leaves it OFF and every preset a test preset runs against turns
# it ON. A script over CMakeLists.txt and CMakePresets.json, so it costs
# no configure. The doctored copy proves the check can fail, and it must
# fail for the doctored reason: any other error (a script fault, nothing
# found to doctor) does not match and fails the probe.
add_test(
    NAME cmake_configure_tests_are_opt_in
    COMMAND ${CMAKE_COMMAND}
        -DENGINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_tests_opt_in.cmake
)
add_test(
    NAME cmake_configure_tests_opt_in_rejects_default_on
    COMMAND ${CMAKE_COMMAND}
        -DENGINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DDOCTORED_DIR=${CMAKE_BINARY_DIR}/cmake_configure_probe/tests_default_on
        -P ${CMAKE_CURRENT_SOURCE_DIR}/doctor_tests_default.cmake
)
set_tests_properties(cmake_configure_tests_opt_in_rejects_default_on
    PROPERTIES PASS_REGULAR_EXPRESSION "ENGINE_BUILD_TESTS does not default OFF")
set_tests_properties(
    cmake_configure_tests_are_opt_in
    cmake_configure_tests_opt_in_rejects_default_on
    PROPERTIES LABELS "cmake_configure" TIMEOUT 60)

# The editor and the player each depend on the targets that put the
# content they load at startup beside them, so building one alone, as an
# IDE's run button does, still produces it. Read from the configured
# targets; no build runs.
set(_engine_startup_content copy_engine_assets copy_sample_project)
if(TARGET bgfx_shader_cook)
    list(APPEND _engine_startup_content bgfx_shader_cook)
endif()
foreach(_engine_app IN ITEMS engine_editor_app engine_player)
    if(NOT TARGET ${_engine_app})
        continue()
    endif()
    get_target_property(_engine_app_dependencies ${_engine_app}
        MANUALLY_ADDED_DEPENDENCIES)
    if(NOT _engine_app_dependencies)
        set(_engine_app_dependencies "")
    endif()
    string(REPLACE ";" "|" _engine_app_dependencies
        "${_engine_app_dependencies}")
    string(REPLACE ";" "|" _engine_app_expected "${_engine_startup_content}")
    add_test(
        NAME cmake_configure_${_engine_app}_builds_its_content
        COMMAND ${CMAKE_COMMAND}
            "-DTARGET_NAME=${_engine_app}"
            "-DDEPENDENCIES=${_engine_app_dependencies}"
            "-DEXPECTED=${_engine_app_expected}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_app_content_dependencies.cmake
    )
    set_tests_properties(cmake_configure_${_engine_app}_builds_its_content
        PROPERTIES LABELS "cmake_configure" TIMEOUT 60)
endforeach()
