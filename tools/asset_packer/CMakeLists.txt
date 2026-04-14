add_executable(asset_packer
    main.cpp
    dependency_graph.cpp
    dependency_graph.h
)

target_compile_features(asset_packer PRIVATE cxx_std_20)

if(WIN32)
    target_compile_definitions(asset_packer PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_include_directories(asset_packer
    PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/math/include
    ${CMAKE_SOURCE_DIR}/physics/include
    ${ENGINE_CGLTF_INCLUDE_DIR}
    ${ENGINE_STB_INCLUDE_DIR}
)

target_link_libraries(asset_packer PRIVATE engine_physics engine_math)
