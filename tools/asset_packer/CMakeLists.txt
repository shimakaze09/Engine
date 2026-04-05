add_executable(asset_packer
    main.cpp
)

target_compile_features(asset_packer PRIVATE cxx_std_20)

if(WIN32)
    target_compile_definitions(asset_packer PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_include_directories(asset_packer
    PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ENGINE_CGLTF_INCLUDE_DIR}
)
