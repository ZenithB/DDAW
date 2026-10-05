# Audio host: JUCE device manager driving the Engine. Shared by the app and the headless audio test.
add_library(ddaw_audiohost STATIC AudioHost.cpp)
target_include_directories(ddaw_audiohost PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ddaw_audiohost
    PUBLIC  ddaw_core juce::juce_audio_devices juce::juce_audio_basics juce::juce_events juce::juce_core
    PRIVATE juce::juce_recommended_config_flags)

juce_add_console_app(ddaw_audiotest PRODUCT_NAME "ddaw_audiotest")
target_sources(ddaw_audiotest PRIVATE audiotest.cpp)
target_compile_definitions(ddaw_audiotest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(ddaw_audiotest PRIVATE ddaw_audiohost juce::juce_recommended_config_flags)

# The views (JUCE components). Picked up by glob so adding a view never touches this file.
file(GLOB DDAW_UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp)
if(APPLE)
    list(APPEND DDAW_UI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform/Gamepad.mm)
endif()
add_library(ddaw_ui STATIC ${DDAW_UI_SOURCES})
if(APPLE)
    target_link_libraries(ddaw_ui PUBLIC "-framework GameController" "-framework CoreHaptics")
endif()
target_include_directories(ddaw_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ddaw_ui PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(ddaw_ui
    PUBLIC  ddaw_appmodel ddaw_plugins juce::juce_gui_basics juce::juce_graphics juce::juce_events juce::juce_core juce::juce_audio_formats juce::juce_audio_devices
    PRIVATE juce::juce_recommended_config_flags)

# Headless UI snapshots: renders the window (and each view) to PNG without showing it.
juce_add_console_app(ddaw_uishot PRODUCT_NAME "ddaw_uishot")
target_sources(ddaw_uishot PRIVATE uishot.cpp)
target_compile_definitions(ddaw_uishot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(ddaw_uishot PRIVATE ddaw_ui juce::juce_recommended_config_flags)

juce_add_gui_app(DDAW
    PRODUCT_NAME "DDAW"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "DDAW records audio from your input device onto armed audio tracks."
    COMPANY_NAME "DDAW"
    BUNDLE_ID    "org.ddaw.DDAW")

target_sources(DDAW PRIVATE Main.cpp AppController.cpp)

target_compile_definitions(DDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DDAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

if(TARGET ddaw_ddsp)
    target_link_libraries(DDAW PRIVATE ddaw_ddsp)
    target_compile_definitions(DDAW PRIVATE DDAW_HAVE_DDSP=1)
endif()

target_link_libraries(DDAW PRIVATE
    ddaw_audiohost
    ddaw_plugins
    ddaw_ui
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
