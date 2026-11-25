cmake_minimum_required(VERSION 3.25)
project(scarface_port LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SCARFACE_BUILD_ANDROID_APP "Build Android APK shared libraries (libSDL3.so, libmain.so)" OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XBE_SDK_DIR "${REPO_ROOT}/sdk")
set(GEN_DIR "${REPO_ROOT}/generated/game")
set(FETCHCONTENT_BASE_DIR "${REPO_ROOT}/out/deps" CACHE PATH "" FORCE)

if(NOT EXISTS "${GEN_DIR}/x86_func_mapping.cpp")
    message(FATAL_ERROR "Generated code missing in ${GEN_DIR}. Run xbe_recomp first.")
endif()

include(FetchContent)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus)

file(STRINGS "${REPO_ROOT}/config/game.toml" GAME_PARTITIONS REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 64)
endif()

set(GEN_SOURCES "${GEN_DIR}/x86_func_mapping.cpp")
math(EXPR LAST_PART "${GAME_PARTITIONS} - 1")
foreach(i RANGE ${LAST_PART})
    list(APPEND GEN_SOURCES "${GEN_DIR}/x86_recomp.${i}.cpp")
endforeach()

# Target 1: Core recompiled game library (libScarface.so)
add_library(Scarface_code SHARED ${GEN_SOURCES})
set_target_properties(Scarface_code PROPERTIES
    OUTPUT_NAME "Scarface"
    CXX_EXTENSIONS OFF
)
target_include_directories(Scarface_code PRIVATE "${XBE_SDK_DIR}/runtime" "${GEN_DIR}")
target_compile_definitions(Scarface_code PRIVATE X86_GAME_CODE)
target_compile_options(Scarface_code PRIVATE
    -O2
    -fno-strict-aliasing
    -fPIC
    -fsemantic-interposition
    -fexceptions
    -frtti
    -w
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(Scarface_code PRIVATE -fno-sanitize=shadow-call-stack)
endif()
target_link_options(Scarface_code PRIVATE
    -Wl,--allow-shlib-undefined
    -Wl,--unresolved-symbols=ignore-all
)
target_link_libraries(Scarface_code PRIVATE m)

# Target 2: Headless Phase 1 host executable
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(FIBER_ASM "${XBE_SDK_DIR}/runtime/cpu/fiber_switch_arm64.S")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(FIBER_ASM "${XBE_SDK_DIR}/runtime/cpu/fiber_switch_x86_64.S")
else()
    message(FATAL_ERROR "Unsupported processor for fiber_switch assembly: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(RUNTIME_SOURCES
    "${XBE_SDK_DIR}/runtime/core/log.cpp"
    "${XBE_SDK_DIR}/runtime/cpu/dispatch.cpp"
    "${XBE_SDK_DIR}/runtime/cpu/fibers.cpp"
    "${FIBER_ASM}"
    "${XBE_SDK_DIR}/runtime/kernel/boot.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/crypto.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/disc.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/files.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/memory.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/objects.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/ordinals.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/rtl.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/threads.cpp"
    "${XBE_SDK_DIR}/runtime/kernel/xboxkrnl.cpp"
    "${XBE_SDK_DIR}/runtime/gpu/history.cpp"
    "${XBE_SDK_DIR}/runtime/gpu/render_thread.cpp"
    "${XBE_SDK_DIR}/runtime/gpu/shaders.cpp"
    "${XBE_SDK_DIR}/runtime/gpu/texture.cpp"
    "${XBE_SDK_DIR}/runtime/gpu/vertex.cpp"
    "${XBE_SDK_DIR}/runtime/xdk/bink.cpp"
    "${XBE_SDK_DIR}/runtime/xdk/crt_math.cpp"
    "${XBE_SDK_DIR}/runtime/xdk/d3d8.cpp"
    "${XBE_SDK_DIR}/runtime/xdk/dsound.cpp"
    "${XBE_SDK_DIR}/runtime/xdk/dsp_image.cpp"
    "${XBE_SDK_DIR}/runtime/xdk/xapi.cpp"
    "${REPO_ROOT}/src/common/game_info.cpp"
    "${REPO_ROOT}/src/game/hooks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/xnet_stub.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/kbm_stub.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/headless_app.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/headless_audio.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/headless_input.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/headless_backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/headless_main.cpp"
)

add_executable(scarface_headless ${RUNTIME_SOURCES})
if(ANDROID)
    set_target_properties(scarface_headless PROPERTIES OUTPUT_NAME "scarface_headless_arm64")
else()
    set_target_properties(scarface_headless PROPERTIES OUTPUT_NAME "scarface_headless_linux")
endif()

target_include_directories(scarface_headless PRIVATE
    "${REPO_ROOT}/src"
    "${XBE_SDK_DIR}/runtime"
    "${GEN_DIR}"
)
target_compile_options(scarface_headless PRIVATE
    -O2
    -fno-strict-aliasing
    -fexceptions
    -frtti
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(scarface_headless PRIVATE -fno-sanitize=shadow-call-stack)
endif()

target_link_options(scarface_headless PRIVATE
    -Wl,--export-dynamic
    "-Wl,-rpath,\$ORIGIN"
)

target_link_libraries(scarface_headless PRIVATE Scarface_code tomlplusplus::tomlplusplus)
if(ANDROID)
    target_link_libraries(scarface_headless PRIVATE android log)
else()
    target_link_libraries(scarface_headless PRIVATE pthread dl)
endif()

if(SCARFACE_BUILD_ANDROID_APP AND ANDROID)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_VIDEO ON CACHE BOOL "" FORCE)
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
    set(SDL_HAPTIC ON CACHE BOOL "" FORCE)
    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_ASSERTIONS release CACHE STRING "" FORCE)

    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
    FetchContent_Declare(oboe GIT_REPOSITORY https://github.com/google/oboe.git GIT_TAG 1.9.3 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3 imgui oboe)

    set(CORE_RUNTIME_SOURCES
        "${XBE_SDK_DIR}/runtime/core/log.cpp"
        "${XBE_SDK_DIR}/runtime/cpu/dispatch.cpp"
        "${XBE_SDK_DIR}/runtime/cpu/fibers.cpp"
        "${FIBER_ASM}"
        "${XBE_SDK_DIR}/runtime/kernel/boot.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/crypto.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/disc.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/files.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/memory.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/objects.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/ordinals.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/rtl.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/threads.cpp"
        "${XBE_SDK_DIR}/runtime/kernel/xboxkrnl.cpp"
        "${XBE_SDK_DIR}/runtime/gpu/history.cpp"
        "${XBE_SDK_DIR}/runtime/gpu/render_thread.cpp"
        "${XBE_SDK_DIR}/runtime/gpu/shaders.cpp"
        "${XBE_SDK_DIR}/runtime/gpu/texture.cpp"
        "${XBE_SDK_DIR}/runtime/gpu/vertex.cpp"
        "${XBE_SDK_DIR}/runtime/xdk/bink.cpp"
        "${XBE_SDK_DIR}/runtime/xdk/crt_math.cpp"
        "${XBE_SDK_DIR}/runtime/xdk/d3d8.cpp"
        "${XBE_SDK_DIR}/runtime/xdk/dsound.cpp"
        "${XBE_SDK_DIR}/runtime/xdk/dsp_image.cpp"
        "${XBE_SDK_DIR}/runtime/xdk/xapi.cpp"
        "${REPO_ROOT}/src/common/game_info.cpp"
        "${REPO_ROOT}/src/game/hooks.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/xnet_stub.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/kbm_stub.cpp"
    )

    set(SHADERC_LIB "${CMAKE_BINARY_DIR}/shaderc/libs/c++_shared/arm64-v8a/libshaderc.a")
    if(NOT EXISTS "${SHADERC_LIB}")
        message(FATAL_ERROR "Cached NDK libshaderc.a missing at ${SHADERC_LIB}. Run build_apk.sh or build libshaderc_combined.")
    endif()

    set(ANDROID_APP_SOURCES
        "${XBE_SDK_DIR}/runtime/audio/oboe.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/android_adpf.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cpu_affinity.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/i18n.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/touch_controls.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/android_input.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/android_app.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vk_tex_neon.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vk_shaders.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vk_upscaler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vk_context.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vk_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/android_main.cpp"
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
    )

    add_library(main SHARED ${CORE_RUNTIME_SOURCES} ${ANDROID_APP_SOURCES})
    target_include_directories(main PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${REPO_ROOT}/src"
        "${XBE_SDK_DIR}/runtime"
        "${GEN_DIR}"
    )
    target_include_directories(main SYSTEM PRIVATE
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
        "/opt/android-ndk/sources/third_party/shaderc/libshaderc/include"
    )
    target_compile_definitions(main PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
    target_compile_options(main PRIVATE
        -O2
        -fno-strict-aliasing
        -fexceptions
        -frtti
        -fno-sanitize=shadow-call-stack
    )
    target_link_options(main PRIVATE -Wl,--export-dynamic)
    target_link_libraries(main PRIVATE
        Scarface_code
        SDL3::SDL3-shared
        oboe
        "${SHADERC_LIB}"
        tomlplusplus::tomlplusplus
        vulkan
        nativewindow
        android
        log
        aaudio
        OpenSLES
    )
endif()
