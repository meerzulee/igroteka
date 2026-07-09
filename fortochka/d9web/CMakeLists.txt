add_library(d9web src/d9web.cpp)
target_include_directories(d9web PUBLIC include)
target_link_libraries(d9web PUBLIC runtime)
target_compile_options(d9web PRIVATE -Wall -Wextra)
