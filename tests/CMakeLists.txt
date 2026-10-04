include(Catch)

add_executable(ddaw_tests
    AllocGuard.cpp
    test_device_contract.cpp
    test_metrics.cpp
    test_fixture_harness.cpp
    test_import.cpp
    test_offline_render.cpp
    test_fixture_runner.cpp
    test_no_alloc.cpp
    test_fifo.cpp
    test_limiter.cpp
    test_transport.cpp
    test_engine.cpp
    test_dsp_util.cpp
    test_project_json.cpp
    document_test.cpp
    app/appmodel_test.cpp)
# One test file per device: tests/devices/<type>_test.cpp. Picked up automatically, so a device task
# never edits this file.
file(GLOB DDAW_DEVICE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/devices/*_test.cpp)
target_sources(ddaw_tests PRIVATE ${DDAW_DEVICE_TESTS})

# Engine-module tests owned by a single task: tests/engine/<name>_test.cpp (also globbed).
file(GLOB DDAW_ENGINE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/engine/*_test.cpp)
target_sources(ddaw_tests PRIVATE ${DDAW_ENGINE_TESTS})

# Shared DSP utility tests: tests/dsp/<name>_test.cpp (also globbed).
file(GLOB DDAW_DSP_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dsp/*_test.cpp)
target_sources(ddaw_tests PRIVATE ${DDAW_DSP_TESTS})
target_link_libraries(ddaw_tests PRIVATE ddaw_harness ddaw_appmodel Catch2::Catch2WithMain)
target_compile_definitions(ddaw_tests PRIVATE DDAW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(ddaw_tests PRIVATE nlohmann_json::nlohmann_json)
catch_discover_tests(ddaw_tests)

if(DDAW_BUILD_APP)
    target_sources(ddaw_tests PRIVATE app/ui_test.cpp)
    target_link_libraries(ddaw_tests PRIVATE ddaw_ui)
endif()

if(DDAW_BUILD_DDSP)
    target_sources(ddaw_tests PRIVATE test_ddsp.cpp test_synth16k.cpp test_ddsp_instrument.cpp)
    target_compile_definitions(ddaw_tests PRIVATE DDAW_MODELS_DIR="${CMAKE_SOURCE_DIR}/models")
    target_link_libraries(ddaw_tests PRIVATE ddaw_ddsp)
endif()

# Parity of every ported device against synthyy's goldens, gated on the Rust port's baseline scores.
# Runs only when the fixtures were synced (scripts/sync_synthyy_fixtures.sh). Unported fixtures are
# reported, not failed; add --strict to fail on them.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/synthyy/projects)
    add_test(NAME parity_synthyy
             COMMAND ddaw_parity ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/synthyy --limiter tone --no-trim
                     --report ${CMAKE_BINARY_DIR}/parity-report.json)
endif()
