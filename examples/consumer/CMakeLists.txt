cmake_minimum_required(VERSION 3.20)

project(ServiceClassRegistryConsumer
  VERSION 1.0.0
  DESCRIPTION "Out-of-tree consumer of the installed Service Class Registry package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only the installed package is used: no source tree and no build tree.
find_package(ServiceClassRegistry CONFIG REQUIRED)

add_executable(scr_consumer main.cpp)
target_link_libraries(scr_consumer PRIVATE Summon::ServiceClassRegistry)

if(MSVC)
  target_compile_options(scr_consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /WX)
else()
  target_compile_options(scr_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
