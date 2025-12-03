@echo off
setlocal

echo === neo_sftp NRO build script (via devkitPro MSYS2) ===

REM Path to devkitPro MSYS2 bash (default Windows install)
set "MSYS_BASH=C:\devkitPro\msys2\usr\bin\bash.exe"

if not exist "%MSYS_BASH%" (
    echo ERROR: Could not find bash at:
    echo   %MSYS_BASH%
    echo Make sure devkitPro is installed in C:\devkitPro
    echo or update MSYS_BASH in build_nro.bat.
    exit /b 1
)

REM This is your repo path *as seen from MSYS2*
REM C:\aiwork\neo_sftp.nro  ->  /c/aiwork/neo_sftp.nro
set "REPO_MSYS=/c/aiwork/neo_sftp.nro"

echo Using MSYS bash: %MSYS_BASH%
echo Repo (MSYS path): %REPO_MSYS%
echo.

REM Run the README build commands inside the devkitPro MSYS2 environment:
REM   mkdir -p build
REM   cd build
REM   /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake ..
REM   make -j4
"%MSYS_BASH%" -lc "cd '%REPO_MSYS%' && mkdir -p build && cd build && /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake .. && make -j4"

if errorlevel 1 (
    echo.
    echo Build FAILED inside MSYS2. Check the errors above.
    exit /b 1
)

echo.
echo Build finished successfully.
echo You should now have: build/neo_sftp.nro
exit /b 0
