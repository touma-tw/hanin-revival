@echo off
setlocal
cd /d %~dp0
set VCV="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
for %%A in (x64 x86) do (
  call %VCV% %%A >nul 2>&1
  if not exist obj\%%A mkdir obj\%%A
  rc /nologo /c 65001 /fo obj\%%A\HaninTip.res HaninTip.rc || exit /b 1
  cl /nologo /W3 /O2 /Zi /MT /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE /Foobj\%%A\ /Fdobj\%%A\ HaninTip.cpp obj\%%A\HaninTip.res /LD /Fe:obj\%%A\HaninTip.dll /link /DEF:HaninTip.def ole32.lib oleaut32.lib user32.lib gdi32.lib advapi32.lib uuid.lib shell32.lib || exit /b 1
)
copy /y obj\x64\HaninTip.dll HaninTip64.dll >nul
copy /y obj\x86\HaninTip.dll HaninTip32.dll >nul
echo BUILD OK
