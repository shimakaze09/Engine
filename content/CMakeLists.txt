# Configures the generic content/asset module: identity, metadata,
# streaming, staleness, and LRU bookkeeping with no renderer dependency.

engine_add_module_library(engine_content
    SOURCES
    src/asset_catalog.cpp
    src/asset_identity.cpp
    src/asset_request_queue.cpp
    src/asset_stale_check.cpp
    src/asset_streaming.cpp
    src/asset_type_table.cpp
    src/lru_cache.cpp
    src/metadata_store.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_core
)
