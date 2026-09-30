cmake_minimum_required(VERSION 3.20)

project(ServiceClassRegistry
  VERSION 1.0.0
  DESCRIPTION "Canonical registry of facility service classes for the DCCP control plane"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SCR_BUILD_TESTS "Build the Service Class Registry test suite" ON)
option(SCR_BUILD_CLI "Build the scr command line tool" ON)
option(SCR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SCR_ENABLE_ASAN "Build with AddressSanitizer when the toolchain supports it" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# An unrecognised build type would silently produce a build with no optimisation
# and no configuration flags at all, so it is refused outright.
if(NOT CMAKE_CONFIGURATION_TYPES)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
  if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR
      "CMAKE_BUILD_TYPE must be Debug, Release, RelWithDebInfo, or MinSizeRel (got '${CMAKE_BUILD_TYPE}')")
  endif()
endif()

set(SCR_LIBRARY_SOURCES
  src/status.cpp
  src/digest.cpp
  src/types.cpp
  src/bytes.cpp
  src/obligation.cpp
  src/revision.cpp
  src/canonical.cpp
  src/contradiction.cpp
  src/composition.cpp
  src/registry.cpp
  src/store.cpp
  src/text.cpp
  src/report.cpp
)

if(WIN32)
  list(APPEND SCR_LIBRARY_SOURCES src/platform_win.cpp)
else()
  list(APPEND SCR_LIBRARY_SOURCES src/platform_posix.cpp)
endif()

# One strict warning policy for every first-party target.
function(scr_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(SCR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-qual -Wconversion -Wsign-conversion)
    if(SCR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(SCR_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

add_library(ServiceClassRegistry STATIC ${SCR_LIBRARY_SOURCES})
add_library(Summon::ServiceClassRegistry ALIAS ServiceClassRegistry)

target_include_directories(ServiceClassRegistry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(ServiceClassRegistry PUBLIC cxx_std_20)
set_target_properties(ServiceClassRegistry PROPERTIES POSITION_INDEPENDENT_CODE ON)
scr_strict_options(ServiceClassRegistry)

if(SCR_BUILD_CLI)
  add_executable(scr src/main.cpp src/cli.cpp src/bench.cpp)
  target_link_libraries(scr PRIVATE ServiceClassRegistry)
  target_compile_features(scr PRIVATE cxx_std_20)
  scr_strict_options(scr)
  if(MSVC)
    target_link_libraries(scr PRIVATE shell32)
  endif()
  install(TARGETS scr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(SCR_BUILD_TESTS)
  enable_testing()

  add_executable(scr_tests
    tests/harness.cpp
    tests/test_main.cpp
    tests/test_core.cpp
    tests/test_store.cpp
    tests/test_property.cpp
    tests/test_adversarial.cpp)
  target_include_directories(scr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(scr_tests PRIVATE ServiceClassRegistry)
  scr_strict_options(scr_tests)
  add_test(NAME unit COMMAND scr_tests)

  add_executable(scr_process_tests
    tests/harness.cpp
    tests/process_main.cpp
    tests/test_process.cpp
    tests/process_util.cpp)
  target_include_directories(scr_process_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(scr_process_tests PRIVATE ServiceClassRegistry)
  scr_strict_options(scr_process_tests)
  if(SCR_BUILD_CLI)
    add_test(NAME process COMMAND scr_process_tests --cli $<TARGET_FILE:scr>)
  endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ServiceClassRegistry
  EXPORT ServiceClassRegistryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/scr DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ServiceClassRegistryTargets
  FILE ServiceClassRegistryTargets.cmake
  NAMESPACE Summon::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ServiceClassRegistry)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ServiceClassRegistryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ServiceClassRegistryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ServiceClassRegistry)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ServiceClassRegistryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ServiceClassRegistryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ServiceClassRegistryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ServiceClassRegistry)
