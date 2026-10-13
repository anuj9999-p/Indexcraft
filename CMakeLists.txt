cmake_minimum_required(VERSION 3.16)
project(IndexCraft VERSION 1.0.0 LANGUAGES CXX)

# Top-level build entry point. The storage engine lives in engine/.
# Usage:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

enable_testing()
add_subdirectory(engine)
