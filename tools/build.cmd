@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cd /d %~dp0
cl /nologo /W3 /O2 /MT /utf-8 hanin_tools.c /Fe:hanin_tools.exe /link /SUBSYSTEM:WINDOWS /BASE:0x10000000 /DYNAMICBASE:NO /MANIFEST:EMBED /MANIFESTINPUT:../common/legacy_cp.manifest user32.lib kernel32.lib psapi.lib
