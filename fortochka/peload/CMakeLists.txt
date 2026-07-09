add_library(peload src/peload.cpp)
target_include_directories(peload PUBLIC include)
target_compile_options(peload PRIVATE -Wall -Wextra)
