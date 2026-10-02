@echo off
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\beach buggy remix"
if exist "%GAME%\SDL2_orig.dll" (
  del /f "%GAME%\SDL2.dll"
  ren "%GAME%\SDL2_orig.dll" SDL2.dll
  echo Restored original SDL2.dll
) else (
  echo Nothing to restore.
)
pause
