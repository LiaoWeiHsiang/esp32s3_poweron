@echo off
REM ============================================================================
REM  MicroLink v2 - Windows build & flash helper
REM  Windows equivalent of build_and_flash.sh / build_and_flash_mac.sh
REM
REM  Usage:
REM    build_and_flash_windows.cmd                     (defaults: COM58, basic_connect)
REM    build_and_flash_windows.cmd -p COM12
REM    build_and_flash_windows.cmd -p COM58 -e cellular_connect
REM    build_and_flash_windows.cmd --clean --erase
REM    build_and_flash_windows.cmd --no-monitor
REM    build_and_flash_windows.cmd -h
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

REM If launched from Git Bash / MSYS, these leak in and idf_tools.py hard-refuses
REM with "MSys/Mingw is not supported". Clear them; we are genuinely in cmd.exe.
set "MSYSTEM="
set "MSYS="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="

REM The ESP-IDF Windows installer does not run on Windows-on-ARM, so ESP-IDF is a
REM plain git clone and its tools live on D: (C: has almost no free space).
if not defined IDF_TOOLS_PATH if exist "D:\Espressif\tools\idf-env.json" set "IDF_TOOLS_PATH=D:\Espressif\tools"

set "PROJ=%~dp0"
set "SELF=%~nx0"
set "PORT=COM58"
set "EXAMPLE=basic_connect"
set "TARGET=esp32s3"
set "DO_CLEAN=0"
set "DO_ERASE=0"
set "DO_MONITOR=1"

REM ---------------------------------------------------------------- args ----
:parse
if "%~1"=="" goto parse_done
if /i "%~1"=="-p"           ( set "PORT=%~2"      & shift & shift & goto parse )
if /i "%~1"=="--port"       ( set "PORT=%~2"      & shift & shift & goto parse )
if /i "%~1"=="-e"           ( set "EXAMPLE=%~2"   & shift & shift & goto parse )
if /i "%~1"=="--example"    ( set "EXAMPLE=%~2"   & shift & shift & goto parse )
if /i "%~1"=="-t"           ( set "TARGET=%~2"    & shift & shift & goto parse )
if /i "%~1"=="--target"     ( set "TARGET=%~2"    & shift & shift & goto parse )
if /i "%~1"=="--clean"      ( set "DO_CLEAN=1"    & shift & goto parse )
if /i "%~1"=="--erase"      ( set "DO_ERASE=1"    & shift & goto parse )
if /i "%~1"=="--no-monitor" ( set "DO_MONITOR=0"  & shift & goto parse )
if /i "%~1"=="-h"           goto usage
if /i "%~1"=="--help"       goto usage
echo [ERROR] Unknown argument: %~1
goto usage
:parse_done

set "EXDIR=%PROJ%examples\%EXAMPLE%"
if not exist "%EXDIR%\CMakeLists.txt" (
    echo [ERROR] No such example: %EXAMPLE%
    echo         Looked in: %EXDIR%
    echo         Available:
    for /d %%D in ("%PROJ%examples\*") do echo           - %%~nxD
    exit /b 1
)

echo ============================================================
echo  MicroLink v2  ^|  example=%EXAMPLE%  target=%TARGET%  port=%PORT%
echo ============================================================

REM -------------------------------------------- step 1: symlink -^> junction ----
REM components\wireguard_lwip is a git symlink. Windows checks it out as a 35-byte
REM text file unless core.symlinks=true + Developer Mode, so the component is
REM invisible to CMake. A directory junction is the equivalent and needs no admin.
set "WG_LINK=%PROJ%components\wireguard_lwip"
set "WG_REAL=%PROJ%components\microlink\components\wireguard_lwip"

if not exist "%WG_REAL%\CMakeLists.txt" (
    echo [ERROR] Missing real component dir: %WG_REAL%
    exit /b 1
)
if exist "%WG_LINK%\CMakeLists.txt" (
    echo [ok]    components\wireguard_lwip resolves correctly
) else (
    echo [fix]   components\wireguard_lwip is not a usable directory - repairing
    if exist "%WG_LINK%\" (
        rmdir "%WG_LINK%" >nul 2>&1
    ) else (
        if exist "%WG_LINK%" del /f /q "%WG_LINK%" >nul 2>&1
    )
    mklink /J "%WG_LINK%" "%WG_REAL%" >nul
    if errorlevel 1 (
        echo [ERROR] Could not create junction at %WG_LINK%
        exit /b 1
    )
    REM Stop git from reporting the junction as a modified symlink.
    git -C "%PROJ%." update-index --skip-worktree components/wireguard_lwip >nul 2>&1
    echo [fix]   junction created
)

REM ------------------------------------------- step 2: verify the COM port ----
REM Cheap check, so do it before the slow ESP-IDF export.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "if (Get-CimInstance Win32_SerialPort | Where-Object { $_.DeviceID -eq '%PORT%' }) { exit 0 } else { exit 1 }"
if errorlevel 1 (
    echo.
    echo [ERROR] %PORT% not present. Ports currently available:
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "Get-CimInstance Win32_SerialPort | ForEach-Object { '          ' + $_.DeviceID + '  ' + $_.Description }"
    echo.
    echo   Pass the right one with:  %SELF% -p COMxx
    exit /b 1
)
echo [ok]    %PORT% present

REM ------------------------------------------------ step 3: locate ESP-IDF ----
REM Corporate TLS proxies re-sign HTTPS. git uses schannel (Windows cert store) and
REM is fine, but Python/pip use certifi and fail. If we exported the Windows roots
REM to a PEM, point Python at it so idf.py can fetch managed_components.
if not defined IDF_CA_BUNDLE set "IDF_CA_BUNDLE=D:\Espressif\corp-ca-bundle.pem"
if exist "%IDF_CA_BUNDLE%" (
    if not defined SSL_CERT_FILE     set "SSL_CERT_FILE=%IDF_CA_BUNDLE%"
    if not defined REQUESTS_CA_BUNDLE set "REQUESTS_CA_BUNDLE=%IDF_CA_BUNDLE%"
    if not defined PIP_CERT          set "PIP_CERT=%IDF_CA_BUNDLE%"
    echo [ok]    CA bundle: %IDF_CA_BUNDLE%
)

set "EXPORT_BAT="
if defined IDF_PATH if exist "%IDF_PATH%\export.bat" set "EXPORT_BAT=%IDF_PATH%\export.bat"

REM This machine is Windows-on-ARM: the ESP-IDF Windows installer refuses to run,
REM so ESP-IDF lives in a plain git clone. C: is nearly full, hence D:.
if not defined EXPORT_BAT (
    for /d %%D in ("D:\Espressif\esp-idf-*") do (
        if exist "%%~fD\export.bat" set "EXPORT_BAT=%%~fD\export.bat"
    )
)
if not defined EXPORT_BAT (
    for /d %%D in ("%USERPROFILE%\esp\*") do (
        if exist "%%~fD\esp-idf\export.bat" set "EXPORT_BAT=%%~fD\esp-idf\export.bat"
    )
)
if not defined EXPORT_BAT if exist "%USERPROFILE%\esp\esp-idf\export.bat" set "EXPORT_BAT=%USERPROFILE%\esp\esp-idf\export.bat"
if not defined EXPORT_BAT (
    for /d %%D in ("C:\Espressif\frameworks\*") do (
        if exist "%%~fD\export.bat" set "EXPORT_BAT=%%~fD\export.bat"
    )
)
if not defined EXPORT_BAT if exist "C:\esp\esp-idf\export.bat" set "EXPORT_BAT=C:\esp\esp-idf\export.bat"

if not defined EXPORT_BAT (
    echo.
    echo [ERROR] ESP-IDF not found. Searched:
    echo           %%IDF_PATH%%\export.bat
    echo           D:\Espressif\esp-idf-*\export.bat
    echo           %USERPROFILE%\esp\*\esp-idf\export.bat
    echo           C:\Espressif\frameworks\*\export.bat
    echo           C:\esp\esp-idf\export.bat
    echo.
    echo   See docs\WINDOWS_SETUP.md for the install steps, or set IDF_PATH yourself:
    echo     set IDF_PATH=D:\Espressif\esp-idf-v5.3.1
    exit /b 1
)

echo [ok]    ESP-IDF: %EXPORT_BAT%

REM idf.py may already be on PATH (running from an ESP-IDF prompt); skip re-export.
where idf.py >nul 2>&1
if errorlevel 1 (
    echo [run]   call export.bat
    call "%EXPORT_BAT%" >nul
    if errorlevel 1 (
        echo [ERROR] export.bat failed - run it manually to see why
        exit /b 1
    )
    where idf.py >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] idf.py still not on PATH after export.bat
        exit /b 1
    )
)
echo [ok]    idf.py on PATH

REM ------------------------------------------- step 4: free the COM port ----
REM No sudo/fuser/chmod on Windows. Instead kill anything still holding the port.
echo [run]   releasing %PORT%
taskkill /f /im idf_monitor.exe >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'idf_monitor|esp_idf_monitor' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1

REM ----------------------------------------------------- step 5: build/flash ----
pushd "%EXDIR%" || exit /b 1

if "%DO_CLEAN%"=="1" (
    echo [run]   fullclean
    call idf.py fullclean || goto fail
)

echo [run]   set-target %TARGET%
call idf.py set-target %TARGET% || goto fail

if "%DO_ERASE%"=="1" (
    echo [run]   erase-flash
    call idf.py -p %PORT% erase-flash || goto fail
)

echo [run]   build
call idf.py build || goto fail

if "%DO_MONITOR%"=="1" (
    echo [run]   flash monitor   ^(Ctrl-] to exit^)
    call idf.py -p %PORT% flash monitor || goto fail
) else (
    echo [run]   flash
    call idf.py -p %PORT% flash || goto fail
)

popd
echo.
echo [done]  OK
exit /b 0

:fail
set "RC=%ERRORLEVEL%"
popd
echo.
echo [FAIL]  step exited with code %RC%
exit /b %RC%

:usage
echo.
echo Usage: %SELF% [options]
echo.
echo   -p,  --port COMxx      serial port          (default COM58)
echo   -e,  --example NAME    examples\NAME        (default basic_connect)
echo   -t,  --target CHIP     idf target           (default esp32s3)
echo        --clean           idf.py fullclean first
echo        --erase           idf.py erase-flash before build
echo        --no-monitor      flash without opening the monitor
echo   -h,  --help            this message
echo.
exit /b 0
