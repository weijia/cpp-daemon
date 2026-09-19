@echo off
setlocal
cd /d "%~dp0"

echo [1/5] Stopping any running instance (avoid locking cpp-daemon.exe during link)...
taskkill /F /IM cpp-daemon.exe >nul 2>&1

echo [2/5] Setting up MSVC x64 environment...
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
  echo [ERROR] vcvarsall.bat not found. Please check VS 2022 BuildTools installation.
  pause
  exit /b 1
)

set "CMAKE_BIN=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if exist "%CMAKE_BIN%\cmake.exe" set "PATH=%CMAKE_BIN%;%PATH%"

echo [3/5] Detecting Conan default profile (ensure MSVC compiler is present)...
where uvx >nul 2>&1
if %errorlevel%==0 (set "CONAN=uvx conan") else (set "CONAN=conan")
%CONAN% profile detect --force
if errorlevel 1 (
  echo [ERROR] Conan profile detect failed.
  pause
  exit /b 1
)

echo [4/5] Installing Conan dependencies...
%CONAN% install . --output-folder=build --build=missing
if errorlevel 1 (
  echo [ERROR] Conan dependency install failed.
  pause
  exit /b 1
)

echo [5/5] Configuring and building (Release)...
cmake --preset conan-default
if errorlevel 1 (
  echo [ERROR] CMake configure failed.
  pause
  exit /b 1
)
cmake --build --preset conan-release
if errorlevel 1 (
  echo [ERROR] Build failed.
  pause
  exit /b 1
)

echo.
echo [DONE] Artifacts:
echo   build\Release\cpp-daemon.exe
echo   build\plugins\Release\reporter.dll
pause
