cmake_minimum_required(VERSION 3.22)
project(capturekit_core VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CAPTUREKIT_BUILD_TESTS "Build capturekit_core unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CAPTUREKIT_BUILD_PYTHON "Build the _ckcore nanobind module" OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CAPTUREKIT_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../third_party" CACHE PATH "third_party root")
foreach(dep eigen sophus mcap/cpp/mcap/include)
  if(NOT EXISTS "${CAPTUREKIT_THIRD_PARTY}/${dep}")
    message(FATAL_ERROR "Missing ${CAPTUREKIT_THIRD_PARTY}/${dep}. Run: git submodule update --init --recursive")
  endif()
endforeach()

add_library(capturekit_core STATIC)
add_library(capturekit::core ALIAS capturekit_core)

add_subdirectory(geometry)
add_subdirectory(bundle)
add_subdirectory(quality)
add_subdirectory(governor)
add_subdirectory(util)
add_subdirectory(replay)
add_subdirectory(guide)
add_subdirectory(session)
add_subdirectory(capi)

target_include_directories(capturekit_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  SYSTEM PUBLIC
    "${CAPTUREKIT_THIRD_PARTY}/eigen"
    "${CAPTUREKIT_THIRD_PARTY}/sophus"
    "${CAPTUREKIT_THIRD_PARTY}/mcap/cpp/mcap/include")

# Compression backends for MCAP (lz4/zstd) are not wired yet; disable until added.
target_compile_definitions(capturekit_core PUBLIC MCAP_COMPRESSION_NO_LZ4 MCAP_COMPRESSION_NO_ZSTD)
target_compile_features(capturekit_core PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(capturekit_core PUBLIC Threads::Threads)

if(ANDROID)
  # JNI bridge (dev.capturekit.nativebridge.CaptureKitNative), loaded as libcapturekit_jni.so.
  add_library(capturekit_jni SHARED bindings/jni/ck_jni.cpp)
  target_link_libraries(capturekit_jni PRIVATE capturekit_core log)
  set_target_properties(capturekit_jni PROPERTIES CXX_VISIBILITY_PRESET hidden)
endif()

if(CAPTUREKIT_BUILD_PYTHON)
  # The C API as a shared library: lets ckit drive the real C ABI through ctypes (simulated phone).
  add_library(capturekit_capi SHARED capi/shared_stub.cpp)
  add_dependencies(capturekit_capi capturekit_core)
  target_link_libraries(capturekit_capi PRIVATE
    "-Wl,--whole-archive" $<TARGET_FILE:capturekit_core> "-Wl,--no-whole-archive" Threads::Threads)
  set_target_properties(capturekit_capi PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/py")
  add_subdirectory(bindings/python)
endif()

if(CAPTUREKIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
