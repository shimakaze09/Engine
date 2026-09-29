# Configures the player: the executable that runs a game without the
# editor. It links engine_runtime only; tools/check_module_deps.py rejects
# an editor include or an engine_editor link here.

# GUI: a windowed application that runs without a terminal on every desktop
# platform (see engine_add_executable_target). Built beside the editor so
# both find the engine's content and the sample project there.
engine_add_executable_target(engine_player
    GUI
    SOURCES
    main.cpp
    PRIVATE_DEPS
    engine_runtime
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}"
)

# The web page a game is shared as: the player, with the project's content
# preloaded.
if(ENGINE_TARGET_PLATFORM STREQUAL "Web")
    engine_package_web_page(engine_player
        "${ENGINE_ENGINE_ASSET_SOURCE_DIR}/web/shell.html")
endif()
