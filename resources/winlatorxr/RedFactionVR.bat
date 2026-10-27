@echo off
rem Red Faction VR (Alpine Faction VR) for WinlatorXR.
rem Converts the GOG Red Faction 1.21 RF.exe to the 1.20 NA RF_120na.exe that
rem Alpine Faction needs (first start only), writes the launcher settings
rem (3200x1448 side by side, Direct3D 11, VR on) and starts the game.
rem The WinlatorXR screen size must be 3200x1448.
cd /d "%~dp0"
if not exist "RF_120na.exe" (
    if exist "RF-1.21.exe.mbsdiff" if exist "RF.exe" (
        echo Creating RF_120na.exe from RF.exe ...
        "%~dp0minibsdiff.exe" app "RF.exe" "RF-1.21.exe.mbsdiff" "RF_120na.exe"
    )
)
if not exist "RF_120na.exe" (
    echo RF_120na.exe is missing. See README-WinlatorXR.txt.
    pause
    exit /b 1
)
regedit /s "%~dp0afvr_winlatorxr.reg"
reg add "HKCU\Software\Volition\Red Faction\Alpine Faction" /v "Executable Path" /t REG_SZ /d "%~dp0RF_120na.exe" /f >nul
"%~dp0AlpineFactionVR.exe" -game
