cmake_minimum_required(VERSION 3.20)
project(lob_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Core library (built from src/order_book.cpp)
add_library(lob STATIC src/order_book.cpp)
target_include_directories(lob PUBLIC include)

# Demo executable
add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE lob)

# ---- Tests (Catch2 via FetchContent) ----
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(Catch2)

enable_testing()
add_executable(tests tests/test_matching.cpp)
target_link_libraries(tests PRIVATE lob Catch2::Catch2WithMain)
add_test(NAME matching COMMAND tests)

# On Windows/MinGW, ctest spawns the test without the compiler's runtime DLL
# directory on PATH (libstdc++, libgcc, libwinpthread). Prepend it so ctest can
# launch the binary. No-op on Linux CI, where the runtime is on the system path.
if(WIN32)
  get_filename_component(_lob_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set_tests_properties(matching PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_lob_cxx_dir}")
endif()

# ---- Scenarios ----
add_executable(scenarios scenarios/validate_scenarios.cpp)
target_link_libraries(scenarios PRIVATE lob)

# ---- Google Benchmark ----
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)   # GB 1.8.3 -Werror trips on GCC 15
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

add_executable(bench bench/bench_book.cpp)
target_link_libraries(bench PRIVATE lob benchmark::benchmark)
