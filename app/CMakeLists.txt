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
# asset tree preloaded into the wasm FS. Cooked bgfx shader binaries
# come from a host cook: shaderc cannot build under Emscripten, so
# ENGINE_WEB_COOKED_DIR names a native build's cooked output
# (<native-build>/assets/shaders/bgfx/cooked), which is copied into the
# packaged asset tree before every link — the .data archive only
# regenerates at link, so the copy stamp is a link dependency and a
# recook repackages instead of silently shipping stale binaries.
if(ENGINE_TARGET_PLATFORM STREQUAL "Web")
    set(ENGINE_WEB_COOKED_DIR "" CACHE PATH
        "Host-cooked bgfx shader dir packaged into the web build")
    if(ENGINE_WEB_COOKED_DIR STREQUAL "")
        message(FATAL_ERROR
            "ENGINE_TARGET_PLATFORM=Web needs ENGINE_WEB_COOKED_DIR: the "
            "path to a native build's cooked shaders "
            "(<native-build>/assets/shaders/bgfx/cooked); shaderc cannot "
            "build under Emscripten, so the web package consumes a host "
            "cook.")
    endif()
    set(_ENGINE_WEB_COOK_STAMP
        "${CMAKE_BINARY_DIR}/web_cooked_shaders.stamp")
    add_custom_command(
        OUTPUT "${_ENGINE_WEB_COOK_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ENGINE_WEB_COOKED_DIR}"
            "${ENGINE_ASSET_OUTPUT_DIR}/shaders/bgfx/cooked"
        COMMAND ${CMAKE_COMMAND} -E touch "${_ENGINE_WEB_COOK_STAMP}"
        DEPENDS "${ENGINE_WEB_COOKED_DIR}/bgfx_shaders.cookstamp"
        COMMENT "Packaging host-cooked bgfx shaders into the web assets")
    add_custom_target(web_cooked_shaders DEPENDS "${_ENGINE_WEB_COOK_STAMP}")
    set_target_properties(engine_editor_app PROPERTIES
        SUFFIX ".html"
        LINK_DEPENDS "${_ENGINE_WEB_COOK_STAMP}")
    # ENV is how the shell requests player mode (ENGINE_PLAYER=1) before
    # main runs; without the export the preRun hook cannot reach it.
    target_link_options(engine_editor_app PRIVATE
        "SHELL:--preload-file ${ENGINE_ASSET_OUTPUT_DIR}@assets"
        "SHELL:--shell-file ${ENGINE_ASSET_SOURCE_DIR}/web/shell.html"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=ENV")
    add_dependencies(engine_editor_app copy_assets web_cooked_shaders)
endif()
