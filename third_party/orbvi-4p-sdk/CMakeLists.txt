cmake_minimum_required(VERSION 3.16)

project(orbvi_4p_sdk VERSION 0.1.0 LANGUAGES CXX)

option(ORBVI_BUILD_TESTS "Build host tests" ON)
option(ORBVI_BUILD_EXAMPLES "Build examples" ON)
option(ORBVI_BUILD_STATIC "Build a static SDK library in addition to shared" OFF)
option(ORBVI_WARNINGS_AS_ERRORS "Treat SDK warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libswscale)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/orbvi_sdk/version.hpp"
  @ONLY)

set(ORBVI_SDK_PUBLIC_HEADERS
  include/orbvi_sdk/client.hpp
  include/orbvi_sdk/compressed_frame.hpp
  include/orbvi_sdk/image_stream.hpp
  include/orbvi_sdk/temperature_stream.hpp
  include/orbvi_sdk/contract.hpp
  include/orbvi_sdk/device.hpp
  include/orbvi_sdk/diagnostics.hpp
  include/orbvi_sdk/export.hpp
  include/orbvi_sdk/fake.hpp
  include/orbvi_sdk/frame.hpp
  include/orbvi_sdk/imu.hpp
  include/orbvi_sdk/options.hpp
  include/orbvi_sdk/replay.hpp
  include/orbvi_sdk/rtsp.hpp
  include/orbvi_sdk/runtime.hpp
  include/orbvi_sdk/status.hpp
  include/orbvi_sdk/subscription.hpp
  include/orbvi_sdk/sync.hpp
  include/orbvi_sdk/transport.hpp
  include/orbvi_sdk/udp_imu.hpp)

set(ORBVI_SDK_SOURCES
  src/client/client.cpp
  src/frame.cpp
  src/runtime/fake_runtime.cpp
  src/runtime/image_stream_runtime.cpp
  src/runtime/temperature_stream_runtime.cpp
  src/runtime/replay_runtime.cpp
  src/runtime/rtsp_runtime.cpp
  src/runtime/udp_imu_runtime.cpp
  src/runtime/unsupported_runtime.cpp
  src/status.cpp)

function(orbvi_configure_library target)
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_compile_definitions(${target} PRIVATE ORBVI_SDK_BUILDING_LIBRARY)
  target_link_libraries(${target} PUBLIC Threads::Threads)
  target_link_libraries(${target} PRIVATE PkgConfig::FFMPEG)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
  if(ORBVI_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>)
  endif()
endfunction()

add_library(orbvi_sdk_shared SHARED ${ORBVI_SDK_SOURCES})
add_library(orbvi_sdk::shared ALIAS orbvi_sdk_shared)
orbvi_configure_library(orbvi_sdk_shared)
set_target_properties(orbvi_sdk_shared PROPERTIES
  OUTPUT_NAME orbvi_sdk
  EXPORT_NAME shared
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

if(ORBVI_BUILD_STATIC)
  add_library(orbvi_sdk_static STATIC ${ORBVI_SDK_SOURCES})
  add_library(orbvi_sdk::static ALIAS orbvi_sdk_static)
  orbvi_configure_library(orbvi_sdk_static)
  set_target_properties(orbvi_sdk_static PROPERTIES
    OUTPUT_NAME orbvi_sdk
    EXPORT_NAME static)
endif()

install(TARGETS orbvi_sdk_shared
  EXPORT orbvi_sdkTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(ORBVI_BUILD_STATIC)
  install(TARGETS orbvi_sdk_static
    EXPORT orbvi_sdkTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/orbvi_sdk/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/orbvi_sdk)

set(ORBVI_SDK_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/orbvi_sdk")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/orbvi_sdkConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/orbvi_sdkConfig.cmake"
  INSTALL_DESTINATION "${ORBVI_SDK_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/orbvi_sdkConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(EXPORT orbvi_sdkTargets
  FILE orbvi_sdkTargets.cmake
  NAMESPACE orbvi_sdk::
  DESTINATION "${ORBVI_SDK_CMAKE_INSTALL_DIR}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/orbvi_sdkConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/orbvi_sdkConfigVersion.cmake"
  DESTINATION "${ORBVI_SDK_CMAKE_INSTALL_DIR}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/orbvi_sdk.pc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/orbvi_sdk.pc"
  @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/orbvi_sdk.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(ORBVI_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(ORBVI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
