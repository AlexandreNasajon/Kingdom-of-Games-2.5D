@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "C:\Users\User\Desktop\KOG-2.5D-Editor-de-mapa\build_cmake"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. 2>&1
cmake --build . --config Release -j4 2>&1
echo BUILD_EXIT=%errorlevel%
