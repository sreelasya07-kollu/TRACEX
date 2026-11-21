include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB CK_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
add_executable(capturekit_core_tests ${CK_TEST_SOURCES})
target_link_libraries(capturekit_core_tests PRIVATE capturekit::core GTest::gtest_main)
target_compile_definitions(capturekit_core_tests PRIVATE CK_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/tmp" CK_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

include(GoogleTest)
gtest_discover_tests(capturekit_core_tests DISCOVERY_TIMEOUT 60)
