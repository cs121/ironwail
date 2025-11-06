@echo off
setlocal enableextensions

echo ==========================================
echo Build + Deploy Ironwail (Release x64)
echo ==========================================

rem --- Pfade ---
set "SLN=C:\Quake\source\ironwail\Windows\VisualStudio\ironwail.sln"

set "SRC_SHADERS=C:\Quake\Source\ironwail\Quake\shaders"
set "SRC_GAME=C:\Quake\Source\ironwail\Quake\game"

set "DST_DIR=C:\Quake\rerelease"
set "DST_SHADERS=%DST_DIR%\id1\shaders"
set "DST_GAME=%DST_DIR%\id1\game"

set "SRC_EXE=C:\Quake\Source\ironwail\Windows\VisualStudio\Build-ironwail\bin\x64\Release\ironwail.exe"
set "DST_EXE=%DST_DIR%\ironwail.exe"

rem (optional) Abhängigkeiten aus dem Build-Log
set "SRC_BASE=C:\Quake\source\ironwail\Windows\VisualStudio"
set "SRC_CODECS=%SRC_BASE%\..\codecs\x64"
set "SRC_SDL2=%SRC_BASE%\..\SDL2\lib64"
set "SRC_CURL=%SRC_BASE%\..\curl\lib\x64"
set "SRC_ZLIB=%SRC_BASE%\..\zlib\x64"

echo [1/6] Git Pull...
git pull
if errorlevel 1 echo [WARN] git pull meldete einen Fehler. Weiter...

rem --- MSBuild finden ---
set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`where vswhere 2^>nul`) do set "VSWHERE=%%i"
if defined VSWHERE (
  for /f "usebackq tokens=*" %%p in (`
    "%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
  `) do set "MSBUILD=%%p"
)
if not defined MSBUILD if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD for /f "usebackq tokens=*" %%m in (`where MSBuild.exe 2^>nul`) do set "MSBUILD=%%m"

if not defined MSBUILD (
  echo [ERROR] MSBuild.exe nicht gefunden.
  exit /b 2
)

echo [2/6] Clean + Build "%SLN%" (Release x64)...
"%MSBUILD%" "%SLN%" /t:Clean;Build /p:Configuration=Release;Platform=x64 /m /v:m
if errorlevel 1 (
  echo [ERROR] Build fehlgeschlagen.
  exit /b 3
)

rem --- Zielordner sicherstellen ---
if not exist "%DST_DIR%\id1"       mkdir "%DST_DIR%\id1"
if not exist "%DST_SHADERS%"       mkdir "%DST_SHADERS%"
if not exist "%DST_GAME%"          mkdir "%DST_GAME%"

rem --- EXE zuerst kopieren ---
echo [3/6] Kopiere Executable...
if not exist "%SRC_EXE%" (
  echo [ERROR] EXE fehlt: %SRC_EXE%
  exit /b 4
)
copy /Y "%SRC_EXE%" "%DST_EXE%" >nul
if errorlevel 1 (
  echo [ERROR] Kopieren von ironwail.exe fehlgeschlagen.
  exit /b 5
)

rem --- (optional) Runtime-DLLs mitkopieren (falls vorhanden) ---
echo [3a] Kopiere Runtime-DLLs (optional)...
if exist "%SRC_CODECS%\libFLAC-8.dll"     copy /Y "%SRC_CODECS%\*.dll" "%DST_DIR%" >nul
if exist "%SRC_SDL2%\SDL2.dll"            copy /Y "%SRC_SDL2%\SDL2.dll" "%DST_DIR%" >nul
if exist "%SRC_CURL%\libcurl.dll"         copy /Y "%SRC_CURL%\libcurl.dll" "%DST_DIR%" >nul
if exist "%SRC_ZLIB%\zlib1.dll"           copy /Y "%SRC_ZLIB%\zlib1.dll" "%DST_DIR%" >nul

rem --- Shader/Game erst NACH der EXE ---
echo [4/6] Kopiere Shader...
robocopy "%SRC_SHADERS%" "%DST_SHADERS%" *.* /E /R:1 /W:1 >nul
if errorlevel 16 echo [ERROR] Robocopy Shader: Schwerer Fehler& exit /b 16
if errorlevel 8  echo [ERROR] Robocopy Shader: Kopierfehler       & exit /b 8

echo [5/6] Kopiere Game...
robocopy "%SRC_GAME%" "%DST_GAME%" *.* /E /R:1 /W:1 >nul
if errorlevel 16 echo [ERROR] Robocopy Game: Schwerer Fehler& exit /b 16
if errorlevel 8  echo [ERROR] Robocopy Game: Kopierfehler       & exit /b 8

echo [6/6] Fertig. Deploy unter: %DST_DIR%
pause
exit /b 0
