# Verifies tests behavior for the Engine test suite.

engine_add_test_executable(engine_smoke
    SOURCES
    engine_smoke.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    gpu
)

# Mirror the editor application's bridge registration and GL lifecycle.
target_link_libraries(engine_smoke PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,engine_editor>
)
