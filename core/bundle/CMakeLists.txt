# Embed the FileDescriptorSet (spec/scanbundle/messages/capturekit.desc) as a byte array at
# configure time; editing the .desc re-runs configure.
set(_desc "${CMAKE_CURRENT_SOURCE_DIR}/../../spec/scanbundle/messages/capturekit.desc")
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/descriptor_set.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_desc}")
set(IN "${_desc}")
set(OUT "${_gen}")
include("${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake")
target_sources(capturekit_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/codec.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/mcap_impl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/stream_writer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/stream_reader.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/finalize.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/validate.cpp
  "${_gen}")
