# Configures scripting build settings for the Engine repository.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(ENGINE_BINDGEN_SCRIPT ${CMAKE_SOURCE_DIR}/tools/binding_generator/generate_bindings.py)
set(ENGINE_BINDGEN_INPUT engine/scripting/bindable_api.h)
set(ENGINE_BINDGEN_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/generated_bindings.cpp)

add_custom_command(
    OUTPUT ${ENGINE_BINDGEN_OUTPUT}
    COMMAND ${Python3_EXECUTABLE} ${ENGINE_BINDGEN_SCRIPT} ${ENGINE_BINDGEN_INPUT} -o ${ENGINE_BINDGEN_OUTPUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include
    DEPENDS
    ${ENGINE_BINDGEN_SCRIPT}
    ${CMAKE_CURRENT_SOURCE_DIR}/include/engine/scripting/bindable_api.h
    COMMENT "Generating Lua bindings (generated_bindings.cpp)"
    VERBATIM
)

engine_add_module_library(engine_scripting
    SOURCES
    src/scripting.cpp
    src/asset_bindings.cpp
    src/audio_bindings.cpp
    src/binding_util.cpp
    src/body_bindings.cpp
    src/camera_bindings.cpp
    src/entity_lifecycle_bindings.cpp
    src/light_bindings.cpp
    src/mesh_material_bindings.cpp
    src/physics_bindings.cpp
    src/cheat_bindings.cpp
    src/collision_bindings.cpp
    src/coroutine_bindings.cpp
    src/debug_bindings.cpp
    src/deferred_mutations.cpp
    src/entity_handle.cpp
    src/entity_pool_bindings.cpp
    src/entity_script_bindings.cpp
    src/game_bindings.cpp
    src/input_bindings.cpp
    src/lua_state.cpp
    src/persist_bindings.cpp
    src/runtime_binding.cpp
    src/scene_bindings.cpp
    src/timer_bindings.cpp
    src/touch_bindings.cpp
    src/dap_server.cpp
    ${ENGINE_BINDGEN_OUTPUT}
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/runtime/include
    PUBLIC_DEPS
    engine_core
    engine_math
    engine_lua
    PRIVATE_DEPS
    engine_physics
    PCH src/pch.h
)

if(WIN32)
    target_link_libraries(engine_scripting PRIVATE ws2_32)
endif()

add_dependencies(engine_scripting engine_lua)
