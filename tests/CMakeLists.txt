add_executable(seek-cra-test seek-cra.cc)

target_link_libraries(seek-cra-test PRIVATE de265)

add_test(
  NAME seek-cra
  COMMAND seek-cra-test "${CMAKE_SOURCE_DIR}/testdata/paris-ra-wpp.h265"
)
