# Magenta DDSP solo-instrument decoder (B1). RTNeural layers + custom LayerNorm.
add_library(ddaw_ddsp STATIC
    Weights.cpp
    Decoder.cpp
    HarmonicSynth.cpp
    Synth16k.cpp
    DdspInstrument.cpp)
target_include_directories(ddaw_ddsp PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ddaw_ddsp PUBLIC ddaw_core PRIVATE RTNeural)
target_compile_features(ddaw_ddsp PUBLIC cxx_std_20)
target_compile_options(ddaw_ddsp PRIVATE -Wall -Wextra -Wpedantic -O3)
target_compile_definitions(ddaw_ddsp PRIVATE DDAW_MODELS_DIR="${CMAKE_SOURCE_DIR}/models")

# B1 verification and timing tool: ddaw_b1 {verify|bench} <instrument> [models dir]
add_executable(ddaw_b1 ${CMAKE_SOURCE_DIR}/tools/b1/ddaw_b1.cpp)
target_link_libraries(ddaw_b1 PRIVATE ddaw_ddsp ddaw_harness)
target_compile_definitions(ddaw_b1 PRIVATE DDAW_MODELS_DIR="${CMAKE_SOURCE_DIR}/models")

# Model-dependent checks run only when the checkpoints have been fetched and exported
# (scripts/b1.sh). They compare against the TF reference; see ARCH 13.
if(DDAW_BUILD_TESTS)
    find_program(DDAW_B1_PYTHON python3 PATHS ${CMAKE_SOURCE_DIR}/.venv-b1/bin NO_DEFAULT_PATH)
    foreach(inst violin flute tenor_saxophone trumpet)
        if(EXISTS ${CMAKE_SOURCE_DIR}/models/export/${inst}.ddspw AND EXISTS ${CMAKE_SOURCE_DIR}/models/ref/${inst}/decoder_raw.bin)
            add_test(NAME b1_verify_${inst} COMMAND ddaw_b1 verify ${inst})
            if(DDAW_B1_PYTHON)
                add_test(NAME b1_synth_spec_${inst}
                         COMMAND ${DDAW_B1_PYTHON} ${CMAKE_SOURCE_DIR}/tools/b1/harmonic_f64.py ${inst})
                set_tests_properties(b1_synth_spec_${inst} PROPERTIES DEPENDS b1_verify_${inst})
            endif()
        endif()
    endforeach()
endif()
