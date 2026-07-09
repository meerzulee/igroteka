add_library(runtime src/machine.cpp)
target_include_directories(runtime PUBLIC include)
target_link_libraries(runtime PUBLIC zhelezo peload)
target_compile_options(runtime PRIVATE -Wall -Wextra)

add_executable(zhrun zhrun.cpp)
target_link_libraries(zhrun runtime k32web u32web d9web)
target_compile_options(zhrun PRIVATE -Wall -Wextra)
