@echo off
setlocal EnableExtensions

echo ==========================================
echo Build + Deploy Ironwail (Release x64)
echo ==========================================

REM ========== Basis ==========

REM Ordner, in dem dieses Script liegt
set "BASE=%~dp0"
cd /d "%BASE%"

REM Projektstruktur relativ vom BASE-Pfad
set "SLN=Windows\VisualStudio\ironwail.sln"

set "SRC_SHADERS=Quake\shaders"
set "SRC_GAME=Quake\game"

REM Deploy-Ziel: FIX nach C:\Quake\rerelease
set "DST_DIR=C:\Quake\rerelease"
set "DST_SHADERS=%DST_DIR%\id1\shaders"
set "DST_GAME=%DST_DIR%\id1\game"

REM Release-EXE Pfad relativ
set "SRC_EXE=Windows\VisualStudio\Build-ironwail\bin\x64\Release\ironwail.exe"
set "DST_EXE=%DST_DIR%\ironwail.exe"

REM (optional) DLL-Pfade relativ
set "SRC_BASE=Windows\VisualStudio"
set "SRC_CODECS=%SRC_BASE%\..\codecs\x64"
set "SRC_SDL2=%SRC_BASE%\..\SDL2\lib64"
set "SRC_CURL=%SRC_BASE%\..\curl\lib\x64"
set "SRC_ZLIB=%SRC_BASE%\..\zlib\x64"


REM ========== MSBuild finden ==========
set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`where vswhere 2^>nul`) do set "VSWHERE=%%i"
if defined VSWHERE (
  for /f "usebackq tokens=*" %%p in (`
    "%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
  `) do set "MSBUILD=%%p"
)

REM Fester Standard-Installationspfad VS 2022 Community
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" (
  set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
)

REM Fallback: MSBuild.exe aus PATH
if not defined MSBUILD for /f "usebackq tokens=*" %%m in (`where MSBuild.exe 2^>nul`) do set "MSBUILD=%%m"

if not defined MSBUILD (
  echo [ERROR] MSBuild.exe nicht gefunden.
  exit /b 2
)

echo [1/5] Clean + Build "%SLN%" (Release x64)...
"%MSBUILD%" "%SLN%" /t:Clean;Build /p:Configuration=Release;Platform=x64 /m /v:m
if errorlevel 1 (
  echo [ERROR] Build fehlgeschlagen.
  exit /b 3
)


REM ========== Ordner anlegen ==========
if not exist "%DST_DIR%\id1"       mkdir "%DST_DIR%\id1"
if not exist "%DST_SHADERS%"       mkdir "%DST_SHADERS%"
if not exist "%DST_GAME%"          mkdir "%DST_GAME%"


REM ========== EXE ==========
echo [2/5] Kopiere Executable...
if not exist "%SRC_EXE%" (
  echo [ERROR] EXE fehlt: %SRC_EXE%
  exit /b 4
)
copy /Y "%SRC_EXE%" "%DST_EXE%" >nul
if errorlevel 1 (
  echo [ERROR] Kopieren von ironwail.exe fehlgeschlagen.
  exit /b 5
)


REM ========== DLLs (optional) ==========
echo [3/5] Kopiere Runtime-DLLs (optional)...
if exist "%SRC_CODECS%\libFLAC-8.dll"     copy /Y "%SRC_CODECS%\*.dll" "%DST_DIR%" >nul
if exist "%SRC_SDL2%\SDL2.dll"            copy /Y "%SRC_SDL2%\SDL2.dll" "%DST_DIR%" >nul
if exist "%SRC_CURL%\libcurl.dll"         copy /Y "%SRC_CURL%\libcurl.dll" "%DST_DIR%" >nul
if exist "%SRC_ZLIB%\zlib1.dll"           copy /Y "%SRC_ZLIB%\zlib1.dll" "%DST_DIR%" >nul


REM ========== Shader ==========
echo [4/5] Kopiere Shader...
robocopy "%SRC_SHADERS%" "%DST_SHADERS%" *.* /E /R:1 /W:1 >nul
if errorlevel 16 echo [ERROR] Robocopy Shader: Schwerer Fehler& exit /b 16
if errorlevel 8  echo [ERROR] Robocopy Shader: Kopierfehler       & exit /b 8


REM ========== Game ==========
echo [5/5] Kopiere Game...
if not exist "%SRC_GAME%" (
  echo [WARN] Game-Ordner fehlt, ueberspringe: %SRC_GAME%
  goto :done
)
robocopy "%SRC_GAME%" "%DST_GAME%" *.* /E /R:1 /W:1 >nul
if errorlevel 16 echo [ERROR] Robocopy Game: Schwerer Fehler& exit /b 16
if errorlevel 8  echo [ERROR] Robocopy Game: Kopierfehler       & exit /b 8


:done
echo Fertig. Deploy unter: %DST_DIR%
pause
exit /b 0
