# Configures tools build settings for the Engine repository.

engine_add_executable_target(asset_packer
    SOURCES
    main.cpp
    cook_stamp.cpp
    mesh_cook.cpp
    thumbnail_generate.cpp
    packer_shared.h
    animation_import.cpp
    anim_cook.cpp
    anim_cook.h
    animation_import.h
    cgltf_impl.cpp
    dependency_graph.cpp
    dependency_graph.h
    skeleton_import.cpp
    skeleton_import.h
    thumbnail_resample.cpp
    thumbnail_resample.h
    PRIVATE_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/math/include
    ${CMAKE_SOURCE_DIR}/physics/include
    ${ENGINE_CGLTF_INCLUDE_DIR}
    ${ENGINE_STB_INCLUDE_DIR}
    PRIVATE_DEPS
    engine_core
    engine_physics
    engine_math
)

if(WIN32)
    target_compile_definitions(asset_packer PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
