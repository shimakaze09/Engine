# Configures app build settings for the Engine repository.

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

# Web (#138): ship as an .html page from the share shell with the synced
# asset tree preloaded into the wasm FS (cooked bgfx shader binaries
# come from a host cook copied into the build's asset dir).
if(ENGINE_TARGET_PLATFORM STREQUAL "Web")
    set_target_properties(engine_editor_app PROPERTIES SUFFIX ".html")
    target_link_options(engine_editor_app PRIVATE
        "SHELL:--preload-file ${ENGINE_ASSET_OUTPUT_DIR}@assets"
        "SHELL:--shell-file ${ENGINE_ASSET_SOURCE_DIR}/web/shell.html")
    add_dependencies(engine_editor_app copy_assets)
endif()
