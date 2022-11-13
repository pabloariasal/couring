cmake_minimum_required(VERSION 3.2)
project(couring)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "-fcoroutines")
endif()

include(${CMAKE_SOURCE_DIR}/cmake/fetch_thread_pool.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/fetch_tinyobjloader.cmake)

add_executable(couring couring.cpp)
target_link_libraries(couring PRIVATE threadpool tinyobjloader uring)
