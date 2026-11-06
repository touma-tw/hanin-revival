@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cd /d %~dp0
cl /nologo /W3 /O2 /Zi /MT /utf-8 hanin_host.c /Fe:hanin_host.exe /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:EMBED /MANIFESTINPUT:../common/legacy_cp.manifest user32.lib kernel32.lib gdi32.lib advapi32.lib
