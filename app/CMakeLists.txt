add_executable(engine_editor_app
    main.cpp
)

target_compile_features(engine_editor_app PRIVATE cxx_std_20)

# engine_runtime provides the main bootstrap/run/shutdown API.
# engine_editor is pulled in via WHOLE_ARCHIVE so its static initializer
# fires and registers the EditorBridge before bootstrap() is called.
target_link_libraries(engine_editor_app
    PRIVATE
    engine_runtime
    $<LINK_LIBRARY:WHOLE_ARCHIVE,engine_editor>
)

# Output the executable directly into the build root so it sits next to
# the build/assets/ directory that CMake copies there automatically.
set_target_properties(engine_editor_app PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
