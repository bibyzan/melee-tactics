@echo off
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
set "EXE=%~dp0build\win\melee.exe"
if not exist "%EXE%" (
    echo Build first. From this folder:
    echo   cmake -B build\win -G Ninja --toolchain ..\..\cmake\msys2-ucrt64.cmake
    echo   cmake --build build\win -j 8
    exit /b 1
)
set MELEE_BOOT_SCENE=tactics
"%EXE%" %*
