# Plugin hosting (L1): VST3 and AudioUnit instruments and effects as devices. Kept out of ddaw_core (which has no JUCE);
# the app, the UI snapshot tool and the tests link it.
add_library(ddaw_plugins STATIC PluginHost.cpp PluginDevice.cpp PluginProvider.cpp)
target_include_directories(ddaw_plugins PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ddaw_plugins PUBLIC
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
if(APPLE)
    target_compile_definitions(ddaw_plugins PUBLIC JUCE_PLUGINHOST_AU=1)
    target_link_libraries(ddaw_plugins PUBLIC "-framework AudioUnit" "-framework CoreAudioKit")   # AudioUnit hosting and its generic view
endif()
target_link_libraries(ddaw_plugins
    PUBLIC  ddaw_core juce::juce_audio_processors juce::juce_audio_basics juce::juce_events juce::juce_core juce::juce_gui_basics
    PRIVATE juce::juce_recommended_config_flags)
