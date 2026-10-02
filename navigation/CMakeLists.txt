# Configures navigation build settings for the Engine repository: the
# navigation mesh bake and its path queries, over math and core only.

engine_add_module_library(engine_navigation
    SOURCES
    src/nav_bake.cpp
    src/nav_query.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_math
    engine_core
)
