cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(INDEXCRAFT_BUILD_TESTS "Build the C++ unit tests (needs GoogleTest)" ON)
option(INDEXCRAFT_FETCH_GTEST "Download GoogleTest with FetchContent if not installed" OFF)

# Build information, embedded in the binaries and shown in the Diagnostics page.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _IC_BT_UPPER)
set(INDEXCRAFT_BUILD_TYPE_STR "${CMAKE_BUILD_TYPE}")
set(INDEXCRAFT_CXX_FLAGS_STR "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_IC_BT_UPPER}}")
string(STRIP "${INDEXCRAFT_CXX_FLAGS_STR}" INDEXCRAFT_CXX_FLAGS_STR)
set(INDEXCRAFT_COMPILER_STR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/indexcraft/build_config.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/indexcraft/build_config.hpp
  @ONLY)

set(IC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)

set(IC_CORE_SOURCES
  src/crc32.cpp
  src/heap_file.cpp
  src/bplus_tree.cpp
  src/database.cpp
  src/query.cpp
  src/json.cpp
  src/build_info.cpp
)

# Object library: compiled once, linked into the shared lib, CLI, bench and tests.
add_library(indexcraft_core OBJECT ${IC_CORE_SOURCES})
target_include_directories(indexcraft_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(indexcraft_core PRIVATE ${IC_WARNINGS})

# Shared library with the C interface used by the FastAPI backend (ctypes).
add_library(indexcraft SHARED src/c_api.cpp $<TARGET_OBJECTS:indexcraft_core>)
target_include_directories(indexcraft PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(indexcraft PRIVATE ${IC_WARNINGS})

# Command-line tool: init data store, generate sample data, run queries.
add_executable(indexcraft_cli src/cli_main.cpp src/c_api.cpp $<TARGET_OBJECTS:indexcraft_core>)
target_include_directories(indexcraft_cli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(indexcraft_cli PRIVATE ${IC_WARNINGS})

# Benchmark runner.
add_executable(indexcraft_bench src/bench_main.cpp $<TARGET_OBJECTS:indexcraft_core>)
target_include_directories(indexcraft_bench PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(indexcraft_bench PRIVATE ${IC_WARNINGS})

# Tests.
if(INDEXCRAFT_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND AND INDEXCRAFT_FETCH_GTEST)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
    set(GTEST_LIBS GTest::gtest GTest::gtest_main)
    set(GTest_FOUND TRUE)
  elseif(GTest_FOUND)
    set(GTEST_LIBS GTest::gtest GTest::gtest_main)
  endif()

  if(GTest_FOUND)
    file(GLOB IC_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    add_executable(indexcraft_tests ${IC_TEST_SOURCES} $<TARGET_OBJECTS:indexcraft_core>)
    target_include_directories(indexcraft_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(indexcraft_tests PRIVATE ${GTEST_LIBS} pthread)
    target_compile_options(indexcraft_tests PRIVATE ${IC_WARNINGS})
    include(GoogleTest)
    add_test(NAME indexcraft_tests COMMAND indexcraft_tests)
  else()
    message(WARNING "GoogleTest not found: C++ tests are NOT built. "
                    "Install libgtest-dev (apt) or configure with -DINDEXCRAFT_FETCH_GTEST=ON.")
  endif()
endif()
