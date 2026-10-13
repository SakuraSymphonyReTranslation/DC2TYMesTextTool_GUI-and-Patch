@echo off
echo ==========================================
echo Building DC2TY Patch (Launcher + DLL)
echo ==========================================

if exist build rmdir /s /q build
mkdir build
cd build

echo [1/2] Configuring CMake...
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 (
    echo [ERROR] CMake failed.
    pause
    exit /b %errorlevel%
)

echo [2/2] Compiling...
nmake
if %errorlevel% neq 0 (
    echo [ERROR] Compilation failed.
    pause
    exit /b %errorlevel%
)

echo.
echo ==========================================
echo [SUCCESS] Build completed!
echo ==========================================
echo.
echo Output files:
echo   - build\DC2TYLauncher.exe
echo   - build\DC2TYPatch.dll
echo.
echo Installation:
echo   1. Copy BOTH files to the DC2TY game folder
echo   2. Run DC2TYLauncher.exe instead of DC2TY.exe
echo.
pause
