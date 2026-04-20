@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ========== Basis ==========
set "BASE=%~dp0"
cd /d "%BASE%"

set "SLN=Windows\VisualStudio\ironwail.sln"
set "VCXPROJ=Windows\VisualStudio\ironwail.vcxproj"

set "SRC_PAKFILES_ROOT=pakfiles"
set "ID1_ASSET_DIRS=bots shaders sounddefs materials particles music"
set "ROOT_ASSET_FILES=ironwail.pak"

REM Deploy-Ziel: FIX nach C:\Quake\rerelease
set "DST_DIR=C:\Quake\rerelease"
set "DST_ID1_ASSET_ROOT=%DST_DIR%\id1"
set "DST_IW_ASSET_ROOT=%DST_DIR%\ironwail"

REM Release-EXE Pfad relativ
set "SRC_EXE=Windows\VisualStudio\Build-ironwail\bin\x64\Release\ironwail.exe"
set "DST_EXE=%DST_DIR%\ironwail.exe"
set "SRC_BUILD_BIN=Windows\VisualStudio\Build-ironwail\bin\x64\Release"
set "SRC_REF_GL=%SRC_BUILD_BIN%\ref_gl.dll"
set "SRC_REF_VK=%SRC_BUILD_BIN%\ref_vk.dll"
set "SRC_REF_DX12=%SRC_BUILD_BIN%\ref_dx12.dll"

REM (optional) DLL-Pfade relativ
set "SRC_BASE=Windows\VisualStudio"
set "SRC_CODECS=%SRC_BASE%\..\codecs\x64"
set "SRC_SDL2=%SRC_BASE%\..\SDL2\lib64"
set "SRC_CURL=%SRC_BASE%\..\curl\lib\x64"
set "SRC_ZLIB=%SRC_BASE%\..\zlib\x64"

REM ========== Logs ==========
set "LOG_DIR=%BASE%build-logs"
set "LOG_FILE=%LOG_DIR%\latest-build.log"
set "DIAG_FILE=%LOG_DIR%\latest-diagnostics.log"

if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
break > "%LOG_FILE%"
break > "%DIAG_FILE%"

call :log_banner "Build + Deploy Ironwail (Release x64)"
call :log "[INFO] Repo-Basis: %BASE%"
call :log "[INFO] Build-Log: %LOG_FILE%"
call :log "[INFO] Diagnose-Log: %DIAG_FILE%"
call :log "[INFO] Arbeitsverzeichnis: %CD%"

call :assert_exists "%SLN%" "Solution-Datei"
if errorlevel 1 exit /b 10
call :assert_exists "%VCXPROJ%" "Projektdatei"
if errorlevel 1 exit /b 11

call :collect_diagnostics

REM ========== MSBuild finden ==========
set "MSBUILD="
set "VSWHERE="

if exist "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" (
  set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
)
if not defined VSWHERE if exist "C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe" (
  set "VSWHERE=C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe"
)
if not defined VSWHERE (
  for /f "usebackq tokens=*" %%i in (`where vswhere 2^>nul`) do set "VSWHERE=%%i"
)

if defined VSWHERE (
  call :log "[INFO] vswhere gefunden: %VSWHERE%"
  >> "%DIAG_FILE%" echo ==== vswhere products ====
  "%VSWHERE%" -products * -format text >> "%DIAG_FILE%" 2>&1
  for /f "usebackq delims=" %%p in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%p"
) else (
  call :log "[WARN] vswhere wurde nicht gefunden."
)

if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

if not defined MSBUILD (
  for /f "usebackq tokens=*" %%m in (`where MSBuild.exe 2^>nul`) do set "MSBUILD=%%m"
)

if not defined MSBUILD (
  call :log "[ERROR] MSBuild.exe nicht gefunden."
  call :log "[INFO] Erwartete Installationen: Visual Studio 2022/2019 oder Build Tools mit MSBuild-Komponente."
  call :log "[INFO] Details stehen in %DIAG_FILE%"
  exit /b 2
)

call :log "[INFO] Verwende MSBuild: %MSBUILD%"
call :log "[INFO] MSBuild-Version:"
"%MSBUILD%" -version >> "%DIAG_FILE%" 2>&1
for /f "usebackq delims=" %%v in (`"%MSBUILD%" -version 2^>nul`) do call :log "        %%v"

call :log "[1/6] Clean + Build \"%SLN%\" (Release x64)..."
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "& '%MSBUILD%' '%SLN%' '/t:Clean;Build' '/p:Configuration=Release;Platform=x64' '/m' '/v:m' 2>&1 | Tee-Object -FilePath '%LOG_FILE%'" 
if errorlevel 1 (
  call :log "[ERROR] Build fehlgeschlagen."
  call :log "[INFO] Vollstaendiges Build-Log: %LOG_FILE%"
  call :log "[INFO] Diagnose-Log: %DIAG_FILE%"
  findstr /C:"error MSB8020" "%LOG_FILE%" >nul 2>&1 && call :log "[HINT] Die installierte MSBuild-Version hat die benoetigten C++ Buildtools/PlatformToolsets nicht."
  exit /b 3
)

REM ========== Ordner anlegen ==========
call :log "[2/6] Stelle Deploy-Ordner sicher..."
if not exist "%DST_ID1_ASSET_ROOT%" mkdir "%DST_ID1_ASSET_ROOT%"
if not exist "%DST_IW_ASSET_ROOT%" mkdir "%DST_IW_ASSET_ROOT%"

REM ========== EXE ==========
call :log "[3/6] Kopiere Executable..."
if not exist "%SRC_EXE%" (
  call :log "[ERROR] EXE fehlt: %SRC_EXE%"
  exit /b 4
)
copy /Y "%SRC_EXE%" "%DST_EXE%" >nul
if errorlevel 1 (
  call :log "[ERROR] Kopieren von ironwail.exe fehlgeschlagen."
  exit /b 5
)

REM ========== DLLs (optional) ==========
call :log "[4/6] Kopiere Runtime-DLLs (optional)..."
if exist "%SRC_CODECS%\libFLAC-8.dll" copy /Y "%SRC_CODECS%\*.dll" "%DST_DIR%" >nul
if exist "%SRC_SDL2%\SDL2.dll" copy /Y "%SRC_SDL2%\SDL2.dll" "%DST_DIR%" >nul
if exist "%SRC_CURL%\libcurl.dll" copy /Y "%SRC_CURL%\libcurl.dll" "%DST_DIR%" >nul
if exist "%SRC_ZLIB%\zlib1.dll" copy /Y "%SRC_ZLIB%\zlib1.dll" "%DST_DIR%" >nul

REM ========== Renderer-Plugins (pflicht) ==========
call :log "[4a/6] Kopiere Renderer-Plugin-DLLs..."
call :copy_required_file "%SRC_REF_GL%" "%DST_DIR%\ref_gl.dll" "ref_gl.dll"
if errorlevel 1 exit /b !errorlevel!
call :copy_required_file "%SRC_REF_VK%" "%DST_DIR%\ref_vk.dll" "ref_vk.dll"
if errorlevel 1 exit /b !errorlevel!
call :copy_required_file "%SRC_REF_DX12%" "%DST_DIR%\ref_dx12.dll" "ref_dx12.dll"
if errorlevel 1 exit /b !errorlevel!

REM ========== Runtime Assets ==========
call :log "[5/6] Kopiere id1-Assets..."
for %%D in (%ID1_ASSET_DIRS%) do (
  call :copy_tree "%SRC_PAKFILES_ROOT%\%%D" "%DST_ID1_ASSET_ROOT%\%%D" "id1/%%D"
  if errorlevel 1 exit /b !errorlevel!
)

call :log "[5b/6] Kopiere ironwail-Assets..."
for %%D in (%ID1_ASSET_DIRS%) do (
  call :copy_tree "%SRC_PAKFILES_ROOT%\%%D" "%DST_IW_ASSET_ROOT%\%%D" "ironwail/%%D"
  if errorlevel 1 exit /b !errorlevel!
)

call :log "[6/6] Kopiere Root-Assets..."
for %%F in (%ROOT_ASSET_FILES%) do (
  call :copy_file "%SRC_PAKFILES_ROOT%\%%F" "%DST_DIR%\%%F" "%%F"
  if errorlevel 1 exit /b !errorlevel!
)

:done
call :log "[OK] Fertig. Deploy unter: %DST_DIR%"
exit /b 0

:copy_tree
if not exist "%~1" (
  call :log "[WARN] Asset-Ordner fehlt, ueberspringe: %~1"
  exit /b 0
)
if not exist "%~2" mkdir "%~2"
call :log "[INFO] Kopiere %~3..."
robocopy "%~1" "%~2" *.* /E /R:1 /W:1 >nul
if errorlevel 16 (
  call :log "[ERROR] Robocopy %~3: Schwerer Fehler"
  exit /b 16
)
if errorlevel 8 (
  call :log "[ERROR] Robocopy %~3: Kopierfehler"
  exit /b 8
)
exit /b 0

:copy_file
if not exist "%~1" (
  call :log "[WARN] Asset-Datei fehlt, ueberspringe: %~1"
  exit /b 0
)
call :log "[INFO] Kopiere %~3..."
copy /Y "%~1" "%~2" >nul
if errorlevel 1 (
  call :log "[ERROR] Kopieren von %~3 fehlgeschlagen."
  exit /b 5
)
exit /b 0

:copy_required_file
if not exist "%~1" (
  call :log "[ERROR] Pflichtdatei fehlt: %~1"
  exit /b 6
)
call :log "[INFO] Kopiere %~3..."
copy /Y "%~1" "%~2" >nul
if errorlevel 1 (
  call :log "[ERROR] Kopieren von %~3 fehlgeschlagen."
  exit /b 5
)
exit /b 0

:assert_exists
if exist "%~1" (
  call :log "[INFO] %~2 gefunden: %~1"
  exit /b 0
)
call :log "[ERROR] %~2 fehlt: %~1"
exit /b 1

:collect_diagnostics
call :log "[INFO] Sammle Diagnosedaten..."
>> "%DIAG_FILE%" echo ==== Environment ====
>> "%DIAG_FILE%" echo DATE=%DATE%
>> "%DIAG_FILE%" echo TIME=%TIME%
>> "%DIAG_FILE%" echo CD=%CD%
>> "%DIAG_FILE%" echo PROCESSOR_ARCHITECTURE=%PROCESSOR_ARCHITECTURE%
>> "%DIAG_FILE%" echo VisualStudioVersion=%VisualStudioVersion%
>> "%DIAG_FILE%" echo VSINSTALLDIR=%VSINSTALLDIR%
>> "%DIAG_FILE%" echo PATH=%PATH%
>> "%DIAG_FILE%" echo.
>> "%DIAG_FILE%" echo ==== Toolset hints from %VCXPROJ% ====
findstr /N /C:"<PlatformToolset>" /C:"<WindowsTargetPlatformVersion>" "%VCXPROJ%" >> "%DIAG_FILE%" 2>&1
>> "%DIAG_FILE%" echo.
>> "%DIAG_FILE%" echo ==== where cl ====
where cl >> "%DIAG_FILE%" 2>&1
>> "%DIAG_FILE%" echo.
>> "%DIAG_FILE%" echo ==== where link ====
where link >> "%DIAG_FILE%" 2>&1
exit /b 0

:log_banner
echo ==========================================
echo %~1
echo ==========================================
>> "%DIAG_FILE%" echo ==========================================
>> "%DIAG_FILE%" echo %~1
>> "%DIAG_FILE%" echo ==========================================
exit /b 0

:log
echo %~1
>> "%DIAG_FILE%" echo %~1
exit /b 0
