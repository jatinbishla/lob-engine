cmake_minimum_required(VERSION 3.20)
project(lob_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Core library (built from src/order_book.cpp)
add_library(lob STATIC src/order_book.cpp)
target_include_directories(lob PUBLIC include)

# Demo executable
add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE lob)
