@echo off
setlocal enabledelayedexpansion

:: Set colors (2 = Green text, 0 = Black background)
color 02

echo ====================================================================
echo  ____  ____  ____       _____                                  _     
echo ^| __ ^)^| __ ^)^|  _ \     ^| ____^|_ __   __ _  __ _ ^| _^| ^| ^| _^| ^|_ 
echo ^|  _ \ ^|  _ \ ^| ^|_) ^|____^|  _^|  ^| '_ \ / _` ^|/ _` ^| ^| ^| ^| ^| ^| ^| __^|
echo ^| ^|_) ^)^| ^|_) ^)^|  _ ^/_____^| ^|___ ^| ^| ^| ^| (_^| ^| (_^| ^| ^| ^| ^| ^| ^| ^| ^|_ 
echo ^|____/ ^|____/ ^|_^| \_\     ^|_____^|_^| ^|_^|\__, ^|\__, ^|_^|___^|_^|___^|\__^|
echo                                        ^|___/ ^|___/                    
echo              STEREOSCOPIC ANAGLYPH 3D MOD INSTALLER
echo ====================================================================
echo.

:: Hardcoded game directory path
set "GAME=C:\beach buggy remix"

echo [INFO] Target directory set to: "%GAME%"
echo.

:: ==========================================
:: STRICT FOLDER EXISTENCE CHECK
:: ==========================================
if not exist "%GAME%\" (
    color 04
    echo ====================================================================
    echo   [ERROR] CRITICAL ERROR: GAME FOLDER NOT FOUND!
    echo ====================================================================
    echo   The installation was ABORTED because the script could not find 
    echo   the game directory at the specified hardcoded path:
    echo   "%GAME%"
    echo.
    echo   FIX: Please make sure your game folder is named correctly and 
    echo        located directly on your C:\ drive.
    echo ====================================================================
    pause
    exit /b 1
)

:: Folder verified, proceed with file injection
echo [PROCESS] Folder verified! Starting file injection...

:: Backup original file if it exists
if exist "%GAME%\SDL2.dll" (
    if not exist "%GAME%\SDL2_orig.dll" (
        echo [PROCESS] Creating backup of original SDL2.dll...
        ren "%GAME%\SDL2.dll" SDL2_orig.dll
    )
)

echo [PROCESS] Copying custom 3D SDL2.dll...
copy /y "%~dp0SDL2.dll" "%GAME%\SDL2.dll" >nul

echo [PROCESS] Injecting d3dcompiler_47.dll...
copy /y "%SystemRoot%\System32\d3dcompiler_47.dll" "%GAME%\d3dcompiler_47.dll" >nul

echo.
echo ====================================================================
echo   [SUCCESS] SUCCESS! 3D MOD INSTALLED SUCCESSFULLY!
echo ====================================================================
echo   Start the game from your desktop shortcut.
echo   Press [F6] in-game to toggle 3D Anaglyph mode.
echo.
echo   Log + Config folder:
echo   %%LOCALAPPDATA%%\Packages\VectorUnit.BeachBuggyRacing_*\LocalState\VectorUnit\BBRAnaglyph\
echo ====================================================================
pause
