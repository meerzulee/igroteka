cmake_minimum_required(VERSION 3.20)
project(fortochka CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(zhelezo)
add_subdirectory(peload)
add_subdirectory(k32web)
add_subdirectory(runtime)
