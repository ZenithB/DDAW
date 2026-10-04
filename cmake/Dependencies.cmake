include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)

if(DDAW_BUILD_APP)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.15
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(DDAW_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# B1: neural decoder. RTNeural pinned to a commit (it has no release tags).
if(DDAW_BUILD_DDSP)
    set(RTNEURAL_BACKEND "XSIMD" CACHE STRING "RTNeural backend: XSIMD (NEON) or STL")
    if(RTNEURAL_BACKEND STREQUAL "XSIMD")
        set(RTNEURAL_XSIMD ON CACHE BOOL "" FORCE)
    elseif(RTNEURAL_BACKEND STREQUAL "STL")
        set(RTNEURAL_STL ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(RTNeural
        GIT_REPOSITORY https://github.com/jatinchowdhury18/RTNeural.git
        GIT_TAG        95c3c0f987a6fe903e7eec71e797405dbed7caf7)
    FetchContent_MakeAvailable(RTNeural)
endif()
