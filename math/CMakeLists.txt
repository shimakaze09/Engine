# Configures math build settings for the Engine repository.
# engine_math is header-only: every function is defined inline in the public
# headers so hot math stays inlinable in consumer translation units.

engine_add_header_library(engine_math
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_core
)
