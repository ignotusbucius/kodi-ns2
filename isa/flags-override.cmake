# Same as ../switch2-flags-override.cmake for compile flags: newlib hides POSIX declarations in
# strict -std=c++NN mode; _DEFAULT_SOURCE exposes them. Kodi's own code is built -O3.
string(APPEND CMAKE_C_FLAGS_INIT " -D_DEFAULT_SOURCE")
string(APPEND CMAKE_CXX_FLAGS_INIT " -D_DEFAULT_SOURCE")
