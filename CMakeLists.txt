cmake_minimum_required(VERSION 3.20)
project(QueryLab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(QUERYLAB_BUILD_TESTS "Build the test suite" ON)
option(QUERYLAB_BUILD_SERVER "Build the HTTP server" ON)
option(QUERYLAB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(QUERYLAB_WERROR "Treat warnings as errors" OFF)

# Third-party code is vendored (single headers) so the project builds offline.
add_library(querylab_thirdparty INTERFACE)
target_include_directories(querylab_thirdparty SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)

add_library(querylab_warnings INTERFACE)
if(MSVC)
  target_compile_options(querylab_warnings INTERFACE /W4 /permissive- /utf-8 /bigobj /Zc:__cplusplus)
  if(QUERYLAB_WERROR)
    target_compile_options(querylab_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(querylab_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough -Wformat=2)
  if(QUERYLAB_WERROR)
    target_compile_options(querylab_warnings INTERFACE -Werror)
  endif()
endif()

if(QUERYLAB_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

if(EMSCRIPTEN)
  # Browser build: engine + wasm entry point (no sockets). The engine tests can
  # still be built (-DQUERYLAB_BUILD_TESTS=ON) and run under Node.
  set(QUERYLAB_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=4MB)
endif()

find_package(Threads REQUIRED)

if(QUERYLAB_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(engine)
if(QUERYLAB_BUILD_SERVER)
  add_subdirectory(server)
endif()
if(EMSCRIPTEN)
  add_subdirectory(wasm)
endif()
