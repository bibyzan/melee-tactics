# Native Windows build from an MSYS2 UCRT64 shell (no cross compiler needed).
#   cmake -B build/win -G Ninja --toolchain ../../cmake/msys2-ucrt64.cmake
# CMake resolves a relative toolchain path from the build directory.
# MSYS2's GCC uses the POSIX thread model, which aurora's std::thread needs.

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_RC_COMPILER windres)

# Same Windows 10 target as cmake/x86_64-w64-mingw32.cmake: aurora needs
# SetThreadDescription and abseil cctz needs the WinRT string API, which
# mingw-w64 declares only at _WIN32_WINNT >= 0x0A00. Set through the flag
# initialisers so FetchContent dependencies (abseil via Dawn) see them too.
set(_win10 "-DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
set(CMAKE_C_FLAGS_INIT "${_win10}")
set(CMAKE_CXX_FLAGS_INIT "${_win10}")
