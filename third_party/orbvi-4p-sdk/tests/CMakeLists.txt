set(ORBVI_UNIT_TEST_NAMES
  test_status
  test_client_state
  test_client_lifecycle
  test_fake
  test_fake_faults
  test_replay
  test_replay_errors
  test_rtsp
  test_h264_frame_metadata
  test_rtsp_frame_conversion
  test_image_stream
  test_udp_imu
  test_temperature_stream
  test_subscription
  test_diagnostics
  test_client_concurrency)

foreach(test_name IN LISTS ORBVI_UNIT_TEST_NAMES)
  add_executable(${test_name} unit/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE orbvi_sdk::shared)
  if(test_name STREQUAL "test_replay")
    target_compile_definitions(${test_name} PRIVATE
      ORBVI_REPLAY_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay/4p-basic-v1.replay")
  endif()
  if(test_name STREQUAL "test_rtsp_frame_conversion" OR test_name STREQUAL "test_h264_frame_metadata")
    target_include_directories(${test_name} PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
    target_link_libraries(${test_name} PRIVATE PkgConfig::FFMPEG)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

set_tests_properties(
  test_subscription
  test_diagnostics
  test_client_concurrency
  PROPERTIES TIMEOUT 10)

set(ORBVI_STANDALONE_HEADERS ${ORBVI_SDK_PUBLIC_HEADERS}
  include/orbvi_sdk/version.hpp)

foreach(header IN LISTS ORBVI_STANDALONE_HEADERS)
  get_filename_component(header_stem "${header}" NAME_WE)
  string(REGEX REPLACE "^include/" "" header_include "${header}")
  set(generated_source "${CMAKE_CURRENT_BINARY_DIR}/standalone_${header_stem}.cpp")
  file(WRITE "${generated_source}"
    "#include \"${header_include}\"\nint main() { return 0; }\n")
  add_executable(orbvi_header_${header_stem} "${generated_source}")
  target_link_libraries(orbvi_header_${header_stem} PRIVATE orbvi_sdk::shared)
  add_test(NAME header_${header_stem} COMMAND orbvi_header_${header_stem})
endforeach()
