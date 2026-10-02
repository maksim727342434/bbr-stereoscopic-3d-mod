@echo off
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\beach buggy remix"
if not exist "%GAME%\PurpleWindowsStore.exe" (
  echo Game folder not found: %GAME%
  echo Usage: install.bat "C:\path\to\game"
  pause & exit /b 1
)
if not exist "%GAME%\SDL2_orig.dll" (
  ren "%GAME%\SDL2.dll" SDL2_orig.dll
)
copy /y "%~dp0SDL2_stage0.dll" "%GAME%\SDL2.dll" >nul
copy /y "%SystemRoot%\System32\d3dcompiler_47.dll" "%GAME%\d3dcompiler_47.dll" >nul
echo Installed STAGE 0 (pure forwarder, no hooks). Start the game from the desktop shortcut.
echo Log + config: %%LOCALAPPDATA%%\Packages\VectorUnit.BeachBuggyRacing_*\LocalState\VectorUnit\BBRAnaglyph\
pause
