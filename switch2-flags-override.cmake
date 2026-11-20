# Loaded through CMAKE_USER_MAKE_RULES_OVERRIDE, after devkitPro's platform file has set the
# *_FLAGS_INIT values. newlib hides POSIX declarations (fileno, fdopen, ...) in strict
# -std=c++NN mode; _DEFAULT_SOURCE exposes them. Not _GNU_SOURCE: spdlog then reads
# struct tm::tm_gmtoff, which newlib does not have. Kodi's own targets get _GNU_SOURCE from
# cmake/scripts/switch2/ArchSetup.cmake.
string(APPEND CMAKE_C_FLAGS_INIT " -D_DEFAULT_SOURCE")
string(APPEND CMAKE_CXX_FLAGS_INIT " -D_DEFAULT_SOURCE")
# Our static deps must win over same-named devkit portlibs (the portlibs ship FFmpeg 7.1;
# Kodi 21 needs our 6.0.1). Bare -l flags resolve against -L dirs in order.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L/k/prefix/lib ${CMAKE_EXE_LINKER_FLAGS_INIT}")
