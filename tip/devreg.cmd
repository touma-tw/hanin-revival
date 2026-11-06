@echo off
rem Dev loop: build, copy x64 DLL under a fresh name, point HKCU COM override at it (no UAC).
cd /d %~dp0
call "%~dp0build.cmd" || exit /b 1
if not exist dev mkdir dev
set N=%RANDOM%
copy /y obj\x64\HaninTip.dll dev\HaninTip64_%N%.dll >nul
reg add "HKCU\Software\Classes\CLSID\{615F8932-792A-4588-8635-332CF2B7EC61}\InprocServer32" /ve /d "%~dp0dev\HaninTip64_%N%.dll" /f >nul
reg add "HKCU\Software\Classes\CLSID\{615F8932-792A-4588-8635-332CF2B7EC61}\InprocServer32" /v ThreadingModel /d Apartment /f >nul
echo DEV DLL: dev\HaninTip64_%N%.dll
