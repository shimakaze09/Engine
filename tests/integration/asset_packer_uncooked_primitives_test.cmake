# Verifies a glTF cook names what it leaves out: a .mesh holds one
# primitive, so a file of 2 meshes x 2 primitives cooks one and says, on
# stderr, which three are not imported and how to choose another; an
# out-of-range selection says which part was cooked instead; and a
# one-primitive file cooks without either warning.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED MULTI_GLTF OR NOT DEFINED
   SINGLE_GLTF OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR
        "ASSET_PACKER, MULTI_GLTF, SINGLE_GLTF, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
file(COPY "${MULTI_GLTF}" "${SINGLE_GLTF}" DESTINATION "${WORKDIR}")
get_filename_component(multi_name "${MULTI_GLTF}" NAME)
get_filename_component(single_name "${SINGLE_GLTF}" NAME)
set(multi "${WORKDIR}/${multi_name}")
set(single "${WORKDIR}/${single_name}")

function(cook input output out_error)
    execute_process(
        COMMAND "${ASSET_PACKER}" "${input}" "${output}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE cook_output
        ERROR_VARIABLE cook_error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "cook of ${input} failed: ${cook_error}")
    endif()
    set(${out_error} "${cook_error}" PARENT_SCOPE)
endfunction()

function(expect text needle what)
    string(FIND "${text}" "${needle}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${what}: no '${needle}' in:\n${text}")
    endif()
endfunction()

function(expect_not text needle what)
    string(FIND "${text}" "${needle}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "${what}: unexpected '${needle}' in:\n${text}")
    endif()
endfunction()

# The default selection: mesh 0 primitive 0 is cooked, the rest named.
cook("${multi}" "${WORKDIR}/default.mesh" err)
expect("${err}" "holds 2 meshes with 4 primitives" "the file's content")
expect("${err}" "mesh 0 primitive 0 is cooked and the other 3 are not imported"
       "what was cooked")
expect("${err}" "importSettings.meshIndex and primitiveIndex" "how to choose")
expect("${err}" "not imported: mesh 0 'Body' primitive 1 (material 'Hair')"
       "the second primitive of the first mesh")
expect("${err}" "not imported: mesh 1 'Props' primitive 0 (material 'Wood')"
       "the second mesh's first primitive")
expect("${err}" "not imported: mesh 1 'Props' primitive 1 (material 'Metal')"
       "the second mesh's second primitive")
expect_not("${err}" "(material 'Skin')" "the cooked primitive")
expect_not("${err}" "out of range" "an in-range selection")

# A selection out of range says what is cooked instead.
file(WRITE "${multi}.meta"
     "{\"schemaVersion\":1,"
     "\"guid\":\"6b0f2a57-8d1e-4c3a-9f6e-2d7b5c1e8a40\","
     "\"importSettings\":{\"meshIndex\":1,\"primitiveIndex\":7}}\n")
cook("${multi}" "${WORKDIR}/selected.mesh" err)
expect("${err}"
       "importSettings.primitiveIndex 7 is out of range (mesh 1 holds 2 primitives); primitive 0 is cooked"
       "an out-of-range primitive")
expect("${err}" "not imported: mesh 0 'Body' primitive 0 (material 'Skin')"
       "the first mesh, not selected")
expect_not("${err}" "(material 'Wood')" "the cooked primitive")

# One primitive in the file: nothing is left out, and nothing is said.
cook("${single}" "${WORKDIR}/single.mesh" err)
expect_not("${err}" "not imported" "a one-primitive file")
expect_not("${err}" "out of range" "a one-primitive file")

file(REMOVE_RECURSE "${WORKDIR}")
