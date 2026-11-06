@echo off
cd /d %~dp0
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cl /nologo /W3 /O2 /MT /EHsc /utf-8 /DUNICODE /D_UNICODE tiptest.cpp /Fe:tiptest64.exe /link ole32.lib user32.lib gdi32.lib uuid.lib psapi.lib oleaut32.lib || exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
cl /nologo /W3 /O2 /MT /EHsc /utf-8 /DUNICODE /D_UNICODE tiptest.cpp /Fe:tiptest32.exe /link ole32.lib user32.lib gdi32.lib uuid.lib psapi.lib oleaut32.lib || exit /b 1
echo BUILD OK
