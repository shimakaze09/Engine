add_executable(engine_smoke
    engine_smoke.cpp
)

target_compile_features(engine_smoke PRIVATE cxx_std_20)

target_link_libraries(engine_smoke
    PRIVATE
        engine_runtime
)

add_test(
    NAME engine_smoke
    COMMAND engine_smoke
)
