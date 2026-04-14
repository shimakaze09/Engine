engine_add_executable_target(asset_packer
    SOURCES
    main.cpp
    dependency_graph.cpp
    dependency_graph.h
    PRIVATE_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/math/include
    ${CMAKE_SOURCE_DIR}/physics/include
    ${ENGINE_CGLTF_INCLUDE_DIR}
    ${ENGINE_STB_INCLUDE_DIR}
    PRIVATE_DEPS
    engine_physics
    engine_math
)

if(WIN32)
    target_compile_definitions(asset_packer PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
