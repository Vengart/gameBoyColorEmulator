@echo off
chcp 65001 >nul
rem Сборка одного самодостаточного exe (SDL2 и рантайм C++ линкуются статически).
rem Результат: dist\GBCEmu.exe
rem Использование: build.bat            - релизная сборка
rem                build.bat test       - релизная сборка и прогон тестов
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Не найден vswhere.exe: установите Visual Studio 2019/2022 с компонентом "Разработка классических приложений на C++".
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio с компилятором C++ не найдена.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

cmake -S . -B out\build\x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe || exit /b 1
cmake --build out\build\x64-release --target gameBoyColorEmulator || exit /b 1

if /i "%~1"=="test" (
    cmake --build out\build\x64-release --target timing_test ppu_test mbc_test dma_test apu_test || exit /b 1
    ctest --test-dir out\build\x64-release --output-on-failure || exit /b 1
)

if not exist dist mkdir dist
copy /y out\build\x64-release\app\GBCEmu.exe dist\GBCEmu.exe >nul || exit /b 1
echo.
echo Готово: %~dp0dist\GBCEmu.exe
endlocal
