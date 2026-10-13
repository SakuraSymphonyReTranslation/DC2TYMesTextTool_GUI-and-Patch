@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"

if exist build rmdir /s /q build
mkdir build
cd build

echo Configuring CMake...
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 exit /b %errorlevel%

echo Compiling...
nmake
if %errorlevel% neq 0 exit /b %errorlevel%

echo Build SUCCESS
