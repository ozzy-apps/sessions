cmake_minimum_required(VERSION 3.16)
project(Sessions VERSION 0.45.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)
find_package(Qt6 QUIET COMPONENTS Test)
find_package(Threads REQUIRED)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(PULSE IMPORTED_TARGET libpulse-simple libpulse)
    pkg_check_modules(ALSA IMPORTED_TARGET alsa)
endif()

set(SESSIONS_SOURCES
    src/core/Model.cpp
    src/core/SetFile.cpp
    src/core/Recovery.cpp
    src/core/Presets.cpp
    src/core/Sample.cpp
    src/core/SampleEdit.cpp
    src/core/Tempo.cpp
    src/core/BeatGrid.cpp
    src/core/Import.cpp
    src/core/Mapping.cpp
    src/core/ControlSurface.cpp
    src/core/ApcKey25.cpp
    src/core/DeviceSpecs.cpp
    src/core/OsmiumPresets.cpp
    src/core/ArrangementOps.cpp
    src/core/Transport.cpp
    src/core/Theme.cpp
    src/core/Grooves.cpp
    src/audio/Engine.cpp
    src/audio/Devices.cpp
    src/audio/Instruments.cpp
    src/audio/Monolith.cpp
    src/audio/DrumBuss.cpp
    src/audio/DrumStudio.cpp
    src/audio/DrumSamples.cpp
    src/audio/Effects.cpp
    src/audio/Wavetables.cpp
    src/audio/Guitar.cpp
    src/audio/Convolver.cpp
    src/audio/Stretch.cpp
    src/audio/Resample.cpp
    src/audio/Nam.cpp
    src/audio/Builtin.cpp
    src/audio/Driver.cpp
    src/audio/JackDriver.cpp
    src/audio/MidiInput.cpp
    src/widgets/DragValue.cpp
    src/widgets/Knob.cpp
    src/widgets/Fader.cpp
    src/widgets/Meter.cpp
    src/widgets/LevelFader.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindowRecovery.cpp
    src/ui/RecoveryDialog.cpp
    src/ui/ControlBar.cpp
    src/ui/Browser.cpp
    src/ui/BrowserIcons.cpp
    src/ui/SvgIcons.cpp
    src/ui/SessionView.cpp
    src/ui/MixerStrip.cpp
    src/ui/ArrangementView.cpp
    src/ui/ClipView.cpp
    src/ui/SampleEditor.cpp
    src/ui/TakesEditor.cpp
    src/ui/EnvelopeEditor.cpp
    src/ui/MapOverlay.cpp
    src/ui/Preferences.cpp
    src/ui/AppIcon.cpp
    src/ui/ThemeEditor.cpp
    src/ui/OverviewBar.cpp
    src/ui/PianoRoll.cpp
    src/ui/DeviceView.cpp
    src/ui/DeviceDrag.cpp
    src/ui/devices/DeviceBody.cpp
    src/ui/devices/EffectBodies.cpp
    src/ui/devices/InstrumentBodies.cpp
    src/ui/devices/OsmiumBody.cpp
    src/ui/devices/DrumStudioBody.cpp
    src/ui/devices/MidiBodies.cpp
    src/ui/devices/RackBodies.cpp
    src/ui/devices/GuitarBodies.cpp
    src/ui/InfoView.cpp
    src/ui/InlineEdit.cpp
    assets/resources.qrc
)

add_library(sessions_core STATIC ${SESSIONS_SOURCES})
target_include_directories(sessions_core PUBLIC src)
target_compile_definitions(sessions_core PRIVATE SESSIONS_DRUM_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/drums")
target_link_libraries(sessions_core PUBLIC Qt6::Widgets Qt6::Svg Threads::Threads ${CMAKE_DL_LIBS})   # dl: libsndfile at run time
if(PULSE_FOUND)
    target_compile_definitions(sessions_core PRIVATE SESSIONS_HAVE_PULSE)
    target_link_libraries(sessions_core PRIVATE PkgConfig::PULSE)
else()
    message(WARNING "libpulse-simple not found (sudo apt install libpulse-dev): Sessions will build without sound output")
endif()
if(ALSA_FOUND)
    target_compile_definitions(sessions_core PUBLIC SESSIONS_HAVE_ALSA)   # the smoke test talks to the sequencer too
    target_link_libraries(sessions_core PUBLIC PkgConfig::ALSA)
else()
    message(WARNING "ALSA not found (sudo apt install libasound2-dev): Sessions will build without MIDI input")
endif()

target_compile_definitions(sessions_core PRIVATE SESSIONS_DRUM_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content/drums")

add_executable(sessions src/main.cpp)
target_link_libraries(sessions PRIVATE sessions_core Qt6::Network)   # Network: the one-instance socket
target_compile_definitions(sessions PRIVATE SESSIONS_VERSION="${PROJECT_VERSION}")

option(SESSIONS_BUILD_TESTS "Build the smoke test" ON)
if(SESSIONS_BUILD_TESTS AND Qt6Test_FOUND)
    add_executable(sessions_smoke tests/smoke.cpp)
    target_link_libraries(sessions_smoke PRIVATE sessions_core Qt6::Test)
    enable_testing()
    add_test(NAME smoke COMMAND sessions_smoke)
    set_tests_properties(smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(sessions_recovery tests/recovery.cpp)
    target_link_libraries(sessions_recovery PRIVATE sessions_core Qt6::Test)
    add_test(NAME recovery COMMAND sessions_recovery)
    set_tests_properties(recovery PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(sessions_osmium tests/osmium.cpp)
    target_link_libraries(sessions_osmium PRIVATE sessions_core Qt6::Test)
    add_test(NAME osmium COMMAND sessions_osmium)
    set_tests_properties(osmium PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(sessions_drum_buss tests/drum_buss.cpp)
    target_link_libraries(sessions_drum_buss PRIVATE sessions_core Qt6::Test)
    add_test(NAME drum_buss COMMAND sessions_drum_buss)
    set_tests_properties(drum_buss PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(sessions_drum_studio tests/drum_studio.cpp)
    target_link_libraries(sessions_drum_studio PRIVATE sessions_core Qt6::Test)
    add_test(NAME drum_studio COMMAND sessions_drum_studio)
    set_tests_properties(drum_studio PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(sessions_studio_expansion tests/studio_expansion.cpp)
    target_link_libraries(sessions_studio_expansion PRIVATE sessions_core Qt6::Test)
    add_test(NAME studio_expansion COMMAND sessions_studio_expansion)
    set_tests_properties(studio_expansion PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RUN_SERIAL TRUE)

    add_executable(sessions_drum_studio_redesign tests/drum_studio_redesign.cpp)
    target_link_libraries(sessions_drum_studio_redesign PRIVATE sessions_core Qt6::Test)
    add_test(NAME drum_studio_redesign COMMAND sessions_drum_studio_redesign)
    set_tests_properties(drum_studio_redesign PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(sessions_osmium_bank tests/osmium_bank.cpp)
    target_link_libraries(sessions_osmium_bank PRIVATE sessions_core)
    add_test(NAME osmium_bank COMMAND sessions_osmium_bank)
    foreach(synthcheck IN ITEMS wavetable_bank osmium_dsp)
        add_executable(sessions_${synthcheck} tests/${synthcheck}.cpp)
        target_link_libraries(sessions_${synthcheck} PRIVATE sessions_core)
        add_test(NAME ${synthcheck} COMMAND sessions_${synthcheck})
    endforeach()
    add_executable(sessions_osmium_ui tests/osmium_ui.cpp)
    target_link_libraries(sessions_osmium_ui PRIVATE sessions_core Qt6::Test)
    add_test(NAME osmium_ui COMMAND sessions_osmium_ui)
    set_tests_properties(osmium_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(sessions_warp_dsp tests/warp_dsp.cpp)
    target_link_libraries(sessions_warp_dsp PRIVATE sessions_core)
    add_test(NAME warp_dsp COMMAND sessions_warp_dsp)
    add_executable(sessions_warp_live tests/warp_live.cpp)
    target_link_libraries(sessions_warp_live PRIVATE sessions_core)
    add_test(NAME warp_live COMMAND sessions_warp_live)
    add_executable(sessions_warp_off tests/warp_off.cpp)
    target_link_libraries(sessions_warp_off PRIVATE sessions_core)
    add_test(NAME warp_off COMMAND sessions_warp_off)
    add_executable(sessions_tempo_warp tests/tempo_warp.cpp)
    target_link_libraries(sessions_tempo_warp PRIVATE sessions_core)
    add_test(NAME tempo_warp COMMAND sessions_tempo_warp)
    add_executable(sessions_warp_ui tests/warp_ui.cpp)
    target_link_libraries(sessions_warp_ui PRIVATE sessions_core Qt6::Test)
    add_test(NAME warp_ui COMMAND sessions_warp_ui)
    set_tests_properties(warp_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(sessions_resample tests/resample.cpp)
    target_link_libraries(sessions_resample PRIVATE sessions_core)
    add_test(NAME resample COMMAND sessions_resample)
    add_executable(sessions_group_fades tests/group_fades.cpp)
    target_link_libraries(sessions_group_fades PRIVATE sessions_core)
    add_test(NAME group_fades COMMAND sessions_group_fades)
    foreach(analysis IN ITEMS tempo_detection beat_grid stem_import)
        add_executable(sessions_${analysis} tests/${analysis}.cpp)
        target_link_libraries(sessions_${analysis} PRIVATE sessions_core)
        add_test(NAME ${analysis} COMMAND sessions_${analysis})
    endforeach()
    foreach(workflow IN ITEMS arrangement_workflow clip_follow control_surface tempo_repair_ui warp_preferences clip_inspector apc_key25)
        add_executable(sessions_${workflow} tests/${workflow}.cpp)
        target_link_libraries(sessions_${workflow} PRIVATE sessions_core Qt6::Test)
        add_test(NAME ${workflow} COMMAND sessions_${workflow})
        set_tests_properties(${workflow} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()

install(TARGETS sessions RUNTIME DESTINATION bin COMPONENT Runtime)
install(DIRECTORY assets/drums/ DESTINATION share/sessions/drums COMPONENT Runtime)
install(DIRECTORY content/drums/ DESTINATION share/sessions/drum-content COMPONENT DrumContent)
install(FILES assets/sessions.desktop DESTINATION share/applications COMPONENT Runtime)
# .sessions files: the MIME type (the desktop entry opens it with %f), with the app icon as its icon.
install(FILES assets/sessions-mime.xml DESTINATION share/mime/packages RENAME sessions.xml COMPONENT Runtime)
install(FILES assets/icon/sessions.svg DESTINATION share/icons/hicolor/scalable/apps COMPONENT Runtime)
install(FILES assets/icon/sessions.svg DESTINATION share/icons/hicolor/scalable/mimetypes RENAME application-x-sessions-set.svg COMPONENT Runtime)
foreach(size 16 32 48 64 128 256 512)
    install(FILES assets/icon/sessions-${size}.png
            DESTINATION share/icons/hicolor/${size}x${size}/apps
            RENAME sessions.png COMPONENT Runtime)
    install(FILES assets/icon/sessions-${size}.png
            DESTINATION share/icons/hicolor/${size}x${size}/mimetypes
            RENAME application-x-sessions-set.png COMPONENT Runtime)
endforeach()

# ---- .deb: rebuilt automatically whenever the app changes, written to dist/
option(SESSIONS_BUILD_DEB "Package a .deb into dist/ on every build" ON)
set(SESSIONS_DEB_MAINTAINER "Sessions <sessions@localhost>" CACHE STRING "Maintainer field of the .deb")
find_program(DPKG_DEB dpkg-deb)
if(SESSIONS_BUILD_DEB AND DPKG_DEB)
    file(GLOB_RECURSE drum_assets CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/drums/*")
    set(deb_stamp "${CMAKE_BINARY_DIR}/deb.stamp")
    add_custom_command(
        OUTPUT "${deb_stamp}"
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DOUT_DIR=${CMAKE_SOURCE_DIR}/dist
            -DVERSION=${PROJECT_VERSION}
            -DMAINTAINER=${SESSIONS_DEB_MAINTAINER}
            -DSTAMP=${deb_stamp}
            -P ${CMAKE_SOURCE_DIR}/cmake/MakeDeb.cmake
        DEPENDS sessions ${drum_assets}
                ${CMAKE_SOURCE_DIR}/cmake/MakeDeb.cmake
                ${CMAKE_SOURCE_DIR}/assets/sessions.desktop
                ${CMAKE_SOURCE_DIR}/assets/sessions-mime.xml
                ${CMAKE_SOURCE_DIR}/assets/icon/sessions.svg
        COMMENT "Packaging dist/sessions_${PROJECT_VERSION}+<timestamp>.deb"
        VERBATIM)
    add_custom_target(deb ALL DEPENDS "${deb_stamp}")
elseif(SESSIONS_BUILD_DEB)
    message(STATUS "dpkg-deb not found: skipping the .deb package")
endif()

# The optional multi-library content package is independent of app-only rebuilds.
if(DPKG_DEB)
    add_custom_target(drum-content-deb
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DOUT_DIR=${CMAKE_SOURCE_DIR}/dist
            -DVERSION=${PROJECT_VERSION} -P ${CMAKE_SOURCE_DIR}/cmake/MakeDrumContent.cmake
        VERBATIM)
endif()
