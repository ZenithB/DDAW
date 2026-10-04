cmake_minimum_required(VERSION 3.25)
project(DDAW VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "")

option(DDAW_BUILD_APP   "Build the JUCE application" ON)
option(DDAW_BUILD_TESTS "Build tests"                ON)
option(DDAW_BUILD_DDSP  "Build the DDSP decoder (B1)" ON)

include(cmake/Dependencies.cmake)
if(DDAW_BUILD_TESTS)
    enable_testing()
endif()

include(cmake/DeviceRegistry.cmake)
ddaw_generate_device_registry(DDAW_DEVICE_SOURCES ${CMAKE_BINARY_DIR}/generated/DeviceRegistry.cpp)

# Engine core: no JUCE dependency, so tests and offline render stay light.
add_library(ddaw_core STATIC
    src/devices/Registry.cpp
    ${CMAKE_BINARY_DIR}/generated/DeviceRegistry.cpp
    ${DDAW_DEVICE_SOURCES}
    src/dsp/CompKernel.cpp
    src/engine/OfflineRender.cpp
    src/engine/Engine.cpp
    src/engine/Graph.cpp
    src/engine/GraphBuilder.cpp
    src/engine/MidiFx.cpp
    src/engine/Scheduler.cpp
    src/engine/Modulation.cpp
    src/engine/AudioClips.cpp
    src/dsp/Limiter.cpp
    src/dsp/Loudness.cpp
    src/dsp/Yin.cpp
    src/dsp/Tracker.cpp
    src/dsp/PolyPitch.cpp
    src/project/SynthyyImport.cpp
    src/project/SampleBank.cpp
    src/project/ProjectJson.cpp
    src/document/NativeFormat.cpp
    src/document/Document.cpp
    src/document/Session.cpp
    src/engine/Recorder.cpp
    src/engine/PolyInput.cpp
    src/engine/GraphService.cpp)
target_include_directories(ddaw_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(ddaw_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
target_compile_features(ddaw_core PUBLIC cxx_std_20)
target_compile_options(ddaw_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# App model: catalog, command builders and the shared UI state. JUCE-free so it is unit tested.
add_library(ddaw_appmodel STATIC
    src/app/model/Catalog.cpp
    src/app/model/Edit.cpp
    src/app/model/Demo.cpp
    src/app/model/Stress.cpp
    src/app/model/Recording.cpp
    src/app/model/Settings.cpp
    src/app/model/ExportPlan.cpp
    src/app/model/CurveRecord.cpp
    src/app/model/AppModel.cpp)
target_link_libraries(ddaw_appmodel PUBLIC ddaw_core)
target_compile_options(ddaw_appmodel PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# Fixture/parity harness (WAV I/O, metrics). Not linked into the app.
add_library(ddaw_harness STATIC
    src/harness/Wav.cpp
    src/harness/Metrics.cpp
    src/harness/FixtureRunner.cpp)
target_link_libraries(ddaw_harness PUBLIC ddaw_core PRIVATE nlohmann_json::nlohmann_json)

# Long-run soak (A6): the engine under random edits for N minutes of audio. Not a ctest (it takes minutes);
# run it directly or through scripts/soak.sh.
add_executable(ddaw_soak tools/soak.cpp tests/AllocGuard.cpp)
target_link_libraries(ddaw_soak PRIVATE ddaw_appmodel)
if(DDAW_BUILD_DDSP)
    target_link_libraries(ddaw_soak PRIVATE ddaw_ddsp)
    target_compile_definitions(ddaw_soak PRIVATE DDAW_SOAK_DDSP=1)
endif()

add_executable(ddaw_devbench tools/devbench.cpp)
target_link_libraries(ddaw_devbench PRIVATE ddaw_core)
add_executable(ddaw_swapbench tools/swapbench.cpp)
target_link_libraries(ddaw_swapbench PRIVATE ddaw_appmodel)
if(DDAW_BUILD_DDSP)
    target_link_libraries(ddaw_swapbench PRIVATE ddaw_ddsp)
    target_compile_definitions(ddaw_swapbench PRIVATE DDAW_SWAPBENCH_DDSP=1)
endif()

# Parity CLI: ddaw_parity <fixturesDir> [--report out.json] [--strict]
add_executable(ddaw_parity tools/parity.cpp)
target_link_libraries(ddaw_parity PRIVATE ddaw_harness)
target_compile_options(ddaw_harness PRIVATE -Wall -Wextra -Wpedantic)

if(DDAW_BUILD_DDSP)
    add_subdirectory(src/ddsp)
endif()

if(DDAW_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(DDAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
