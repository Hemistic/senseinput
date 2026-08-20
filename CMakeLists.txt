cmake_minimum_required(VERSION 3.20)

project(sensevoice_stream LANGUAGES C CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

add_library(sensevoice_core STATIC
    src/audio_conditioner.cpp
    src/fsmn_vad_engine.cpp
    src/hotword_boost.cpp
    src/sensevoice_engine.cpp
    src/stability_tracker.cpp
    src/stream_recognizer.cpp
    src/system_audio_mute.cpp
    src/text_processor.cpp
)
target_include_directories(sensevoice_core PUBLIC src)
target_link_libraries(sensevoice_core PUBLIC ggml)

if(MSVC)
    target_compile_definitions(sensevoice_core PRIVATE NOMINMAX _USE_MATH_DEFINES)
    target_compile_options(sensevoice_core PRIVATE /utf-8 /W4 /permissive-)
endif()

add_executable(sensevoice-stream
    src/audio_io.cpp
    src/main.cpp
)
target_include_directories(sensevoice-stream PRIVATE
    src
    third_party/FunASR/runtime/llama.cpp/funasr-common
)
target_link_libraries(sensevoice-stream PRIVATE sensevoice_core)

if(WIN32)
    target_compile_definitions(sensevoice-stream PRIVATE NOMINMAX _USE_MATH_DEFINES)
    target_compile_options(sensevoice-stream PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(sensevoice-stream PRIVATE
        ole32
        winmm
        mfplat
        mfreadwrite
        mfuuid
    )
endif()

if(WIN32)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
    set(CMAKE_AUTORCC ON)

    set(OPENLESS_IME_SOURCE_DIR
        "${CMAKE_SOURCE_DIR}/third_party/openless/openless-all/app/windows-ime/src")
    add_library(OpenLessIme SHARED
        ${OPENLESS_IME_SOURCE_DIR}/class_factory.cpp
        ${OPENLESS_IME_SOURCE_DIR}/dllmain.cpp
        ${OPENLESS_IME_SOURCE_DIR}/edit_session.cpp
        ${OPENLESS_IME_SOURCE_DIR}/ipc_client.cpp
        ${OPENLESS_IME_SOURCE_DIR}/registry.cpp
        ${OPENLESS_IME_SOURCE_DIR}/resource.rc
        ${OPENLESS_IME_SOURCE_DIR}/text_service.cpp
        ${OPENLESS_IME_SOURCE_DIR}/OpenLessIme.def
    )
    target_include_directories(OpenLessIme PRIVATE ${OPENLESS_IME_SOURCE_DIR})
    target_compile_definitions(OpenLessIme PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(OpenLessIme PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(OpenLessIme PRIVATE advapi32 ole32 uuid)
    set_property(TARGET OpenLessIme PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_executable(sensevoice-ui WIN32
        src/audio_io.cpp
        src/qt_ui_main.cpp
        src/windows_text_injector.cpp
        resources/sensevoice.qrc
        resources/sensevoice.rc
    )
    target_include_directories(sensevoice-ui PRIVATE
        src
        third_party/FunASR/runtime/llama.cpp/funasr-common
    )
    target_compile_definitions(sensevoice-ui PRIVATE NOMINMAX _USE_MATH_DEFINES UNICODE _UNICODE)
    target_compile_options(sensevoice-ui PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(sensevoice-ui PRIVATE
        sensevoice_core
        Qt6::Widgets
        psapi
        ole32
        oleaut32
        uiautomationcore
        uuid
        winmm
        mfplat
        mfreadwrite
        mfuuid
    )

    add_executable(windows-text-injector-test
        tests/windows_text_injector_test.cpp
        src/windows_text_injector.cpp
    )
    target_include_directories(windows-text-injector-test PRIVATE src)
    target_compile_definitions(windows-text-injector-test PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(windows-text-injector-test PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(windows-text-injector-test PRIVATE
        ole32
        oleaut32
        uiautomationcore
        uuid
    )
    add_test(NAME windows-text-injector-test COMMAND windows-text-injector-test)

    add_executable(windows-text-target WIN32
        tests/windows_text_target.cpp
    )
    target_compile_definitions(windows-text-target PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(windows-text-target PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(windows-text-target PRIVATE Qt6::Widgets)
    add_dependencies(windows-text-injector-test windows-text-target)

    add_executable(sensevoice-inject
        src/inject_main.cpp
        src/windows_text_injector.cpp
    )
    target_include_directories(sensevoice-inject PRIVATE src)
    target_compile_definitions(sensevoice-inject PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(sensevoice-inject PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(sensevoice-inject PRIVATE
        ole32
        oleaut32
        shell32
        uiautomationcore
        uuid
    )

    add_executable(sensevoice-ui-legacy WIN32
        src/audio_io.cpp
        src/ui_main.cpp
        resources/sensevoice.rc
    )
    target_include_directories(sensevoice-ui-legacy PRIVATE
        src
        third_party/FunASR/runtime/llama.cpp/funasr-common
    )
    target_compile_definitions(sensevoice-ui-legacy PRIVATE NOMINMAX _USE_MATH_DEFINES UNICODE _UNICODE)
    target_compile_options(sensevoice-ui-legacy PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(sensevoice-ui-legacy PRIVATE
        sensevoice_core
        comctl32
        psapi
        ole32
        winmm
        mfplat
        mfreadwrite
        mfuuid
    )

    add_executable(sensevoice-setup-stub WIN32
        packaging/windows/setup_stub.cpp
        resources/sensevoice.rc
    )
    set_property(TARGET sensevoice-setup-stub PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(sensevoice-setup-stub PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(sensevoice-setup-stub PRIVATE /utf-8 /W4 /permissive-)
endif()

enable_testing()
add_executable(stability-tracker-test
    tests/stability_tracker_test.cpp
    src/stability_tracker.cpp
)
target_include_directories(stability-tracker-test PRIVATE src)
if(MSVC)
    target_compile_options(stability-tracker-test PRIVATE /utf-8 /W4 /permissive-)
endif()
add_test(NAME stability-tracker-test COMMAND stability-tracker-test)

add_executable(text-processor-test
    tests/text_processor_test.cpp
    src/text_processor.cpp
)
target_include_directories(text-processor-test PRIVATE src)
target_compile_definitions(text-processor-test PRIVATE
    SENSEVOICE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(text-processor-test PRIVATE /utf-8 /W4 /permissive-)
endif()
add_test(NAME text-processor-test COMMAND text-processor-test)

add_executable(audio-conditioner-test
    tests/audio_conditioner_test.cpp
    src/audio_conditioner.cpp
)
target_include_directories(audio-conditioner-test PRIVATE src)
if(MSVC)
    target_compile_options(audio-conditioner-test PRIVATE /utf-8 /W4 /permissive-)
endif()
add_test(NAME audio-conditioner-test COMMAND audio-conditioner-test)

add_executable(hotword-boost-test
    tests/hotword_boost_test.cpp
    src/hotword_boost.cpp
)
target_include_directories(hotword-boost-test PRIVATE src)
if(MSVC)
    target_compile_options(hotword-boost-test PRIVATE /utf-8 /W4 /permissive-)
endif()
add_test(NAME hotword-boost-test COMMAND hotword-boost-test)
