add_library(u32web src/u32web.cpp)
target_include_directories(u32web PUBLIC include)
target_link_libraries(u32web PUBLIC runtime)
target_compile_options(u32web PRIVATE -Wall -Wextra)

add_executable(window_test test/window_test.cpp)
target_link_libraries(window_test runtime u32web k32web)
target_compile_options(window_test PRIVATE -Wall -Wextra)
