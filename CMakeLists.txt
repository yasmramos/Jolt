cmake_minimum_required(VERSION 3.15)

project(jolt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(jolt
  src/value.cpp
  src/lexer.cpp
  src/parser.cpp
  src/bytecode.cpp
  src/compiler.cpp
  src/vm.cpp
  src/interpreter.cpp
  src/builtins.cpp
  src/diagnostic.cpp
  src/main.cpp
)

target_include_directories(jolt PRIVATE src include)

if(MSVC)
  target_compile_options(jolt PRIVATE /W4)
else()
  target_compile_options(jolt PRIVATE -Wall -Wextra)
endif()

install(TARGETS jolt RUNTIME DESTINATION bin)
