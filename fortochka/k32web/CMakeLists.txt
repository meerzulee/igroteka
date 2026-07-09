add_library(k32web src/k32web.cpp)
target_include_directories(k32web PUBLIC include)
target_link_libraries(k32web PUBLIC zhelezo)
target_compile_options(k32web PRIVATE -Wall -Wextra)
