add_library(sysweb src/sysweb.cpp)
target_include_directories(sysweb PUBLIC include)
target_link_libraries(sysweb PUBLIC runtime)
target_compile_options(sysweb PRIVATE -Wall -Wextra)
