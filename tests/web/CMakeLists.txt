# Builds the browser lifecycle harness page and registers the headless
# Chromium runs that drive it (#341, #652). Web builds only: the host
# suites stay host-side. The driver needs this directory's pinned
# Playwright (npm ci) and its Chromium (npx playwright install
# chromium-headless-shell).

engine_add_executable_target(engine_web_lifecycle
    SOURCES
    web_lifecycle_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_audio
    engine_scripting
    engine_renderer
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}"
)
# The same editor registration the shipped page carries, so player mode
# displaces a real bridge and each shutdown has to hand it back.
target_link_libraries(engine_web_lifecycle
    PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,engine_editor>
)
engine_package_web_page(engine_web_lifecycle
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle_shell.html")

find_program(ENGINE_NODE_EXECUTABLE node REQUIRED)

foreach(web_case frames max_frames_1 quit fatal_stage page_boots save_persists)
    if(web_case STREQUAL "page_boots")
        set(web_test engine_web_page_boots)
    else()
        set(web_test engine_web_lifecycle_${web_case})
    endif()
    add_test(NAME ${web_test}
        COMMAND "${ENGINE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/run_lifecycle.mjs"
            --dir "${CMAKE_BINARY_DIR}" --case ${web_case})
    set_tests_properties(${web_test} PROPERTIES LABELS "web" TIMEOUT 300)
endforeach()
