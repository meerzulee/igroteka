add_executable(zhrun zhrun.cpp)
target_link_libraries(zhrun zhelezo peload k32web)
target_compile_options(zhrun PRIVATE -Wall -Wextra)
