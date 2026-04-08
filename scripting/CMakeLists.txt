add_library(engine_scripting STATIC
    src/scripting.cpp
)

target_include_directories(engine_scripting
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/include
)

target_compile_features(engine_scripting PUBLIC cxx_std_20)

target_link_libraries(engine_scripting
    PUBLIC
    engine_core
    engine_math
    engine_lua
)

add_dependencies(engine_scripting engine_lua)
