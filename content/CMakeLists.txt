# Configures the generic content/asset module (#171): identity, metadata,
# streaming, staleness, and LRU bookkeeping with no renderer dependency.

engine_add_module_library(engine_content
    SOURCES
    src/asset_stale_check.cpp
    src/asset_streaming.cpp
    src/lru_cache.cpp
    src/metadata_store.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_core
)
