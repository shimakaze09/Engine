engine_add_executable_target(engine_editor_app
    SOURCES
    main.cpp
    PRIVATE_DEPS
    engine_runtime
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}"
)

# engine_runtime provides the main bootstrap/run/shutdown API.
# engine_editor is pulled in via WHOLE_ARCHIVE so its static initializer
# fires and registers the EditorBridge before bootstrap() is called.
target_link_libraries(engine_editor_app
    PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,engine_editor>
)
