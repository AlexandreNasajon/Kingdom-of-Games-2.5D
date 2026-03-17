@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 > /dev/null 2>&1
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cd /d "%~dp0build_cmake"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release -j4
echo BUILD_EXIT=%errorlevel%
