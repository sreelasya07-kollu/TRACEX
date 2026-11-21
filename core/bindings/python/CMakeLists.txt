# nanobind module `_ckcore`, built into ${CMAKE_BINARY_DIR}/py (ckit adds it to sys.path).
find_package(Python 3.11 COMPONENTS Interpreter Development.Module REQUIRED)
add_subdirectory("${CAPTUREKIT_THIRD_PARTY}/nanobind" "${CMAKE_BINARY_DIR}/nanobind" EXCLUDE_FROM_ALL)
file(GLOB CK_PY_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
nanobind_add_module(_ckcore NB_STATIC ${CK_PY_SOURCES})
target_link_libraries(_ckcore PRIVATE capturekit::core)
set_target_properties(_ckcore PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/py")
