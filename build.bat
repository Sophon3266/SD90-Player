@echo off
setlocal
set CXX=g++
set CFLAGS=-O2 -std=c++17 -m64 -Wall -Wextra -Wno-unused-parameter
if not exist build mkdir build
%CXX% %CFLAGS% -shared -o build\SD90Player.dll src\plugin.cpp src\sd90.cpp -luser32 -lgdi32 -static -static-libgcc -static-libstdc++ || exit /b 1
%CXX% %CFLAGS% -o build\sd90render.exe src\render.cpp src\sd90.cpp -lole32 -static -static-libgcc -static-libstdc++ || exit /b 1
%CXX% %CFLAGS% -o build\vsthost_test.exe src\render.cpp src\sd90.cpp -lole32 -static -static-libgcc -static-libstdc++ || exit /b 1
%CXX% %CFLAGS% -o build\sd90play.exe src\play.cpp src\sd90.cpp -lwinmm -lole32 -static -static-libgcc -static-libstdc++ || exit /b 1
if exist tools\dump_mem.cpp (
  %CXX% %CFLAGS% -o build\dump_mem.exe tools\dump_mem.cpp -static -static-libgcc -static-libstdc++ || exit /b 1
)
echo built.
