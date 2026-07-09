cmake_minimum_required(VERSION 3.20)
project(zhelezo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ZHELEZO_SANITIZE "Build with ASan/UBSan (native test builds)" OFF)

add_library(zhelezo
    src/decode.cpp
    src/exec.cpp
)
target_include_directories(zhelezo PUBLIC include)
target_compile_options(zhelezo PRIVATE -Wall -Wextra)

if(ZHELEZO_SANITIZE)
    target_compile_options(zhelezo PUBLIC -fsanitize=address,undefined -g)
    target_link_options(zhelezo PUBLIC -fsanitize=address,undefined)
endif()

add_executable(zhtest src/zhtest.cpp)
target_link_libraries(zhtest zhelezo)
target_compile_options(zhtest PRIVATE -Wall -Wextra)
