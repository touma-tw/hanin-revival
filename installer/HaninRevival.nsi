; 漢音輸入法復原版 (Hanin Revival) 安裝程式
;
; 本安裝檔只包含本專案的橋接程式。漢音輸入法本身(Panasonic 漢音 5.02 試用版)
; 由使用者自備:把 Hanin502.exe 放在本安裝檔的同一個資料夾。安裝時會以 SHA-256
; 驗證該檔,再從中解出引擎、字典與工具箱。
;
; 建置:makensis /DVERSION=x.y.z HaninRevival.nsi   (先執行 build-release.ps1 準備 bin\)

Unicode true
!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!define APPNAME   "漢音輸入法復原版"
!define APPKEY    "HaninRevival"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\HaninRevival"
!define RUNKEY    "Software\Microsoft\Windows\CurrentVersion\Run"
!define HANIN_SHA256 "C8EE45C4F0C24DE502912DD1747A66481AD7CFB6A953540B814205458C9B5814"

Name "${APPNAME}"
Caption "${APPNAME} ${VERSION} 安裝程式"
!ifdef TESTNOADMIN
  OutFile "..\release\test\HaninRevival-Setup-TEST.exe"
!else
  OutFile "..\release\HaninRevival-Setup-${VERSION}.exe"
!endif
InstallDir "$PROGRAMFILES64\HaninRevival"
!ifdef TESTNOADMIN
  RequestExecutionLevel user   ; test build: only exercises the .onInit checks
!else
  RequestExecutionLevel admin
!endif
ManifestDPIAware true
SetCompressor /SOLID lzma
BrandingText "Hanin Revival ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1028 "ProductName" "${APPNAME}"
VIAddVersionKey /LANG=1028 "FileDescription" "${APPNAME} 安裝程式"
VIAddVersionKey /LANG=1028 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1028 "LegalCopyright" "MIT License (bridge code only)"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!define MUI_ICON "..\tip\hanin_fallback.ico"
!define MUI_UNICON "..\tip\hanin_fallback.ico"
!define MUI_WELCOMEPAGE_TITLE "${APPNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "這個程式讓 Panasonic 漢音輸入法 5.02 能在 64 位元的 Windows 10 / 11 上使用。$\r$\n$\r$\n本安裝檔只包含橋接程式,不包含漢音輸入法本身。請把漢音 5.02 的安裝檔 Hanin502.exe 放在本安裝檔的同一個資料夾,安裝時會先驗證它的 SHA-256,確認是未被修改過的原版檔案。$\r$\n$\r$\n漢音輸入法的著作權屬於原作者;本專案為非官方的相容性橋接程式。"
!define MUI_FINISHPAGE_TITLE "安裝完成"
!define MUI_FINISHPAGE_TEXT "請先登出再重新登入 Windows,讓所有程式載入漢音。$\r$\n$\r$\n之後按 Win + 空白鍵 選擇「漢音輸入法」。鍵盤排列(標準、倚天…)等設定,請由語言列的「設定/工具選單」或開始功能表的「漢音工具箱」調整。"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE.txt"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "TradChinese"

Var PS        ; 64-bit Windows PowerShell

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APPNAME} 只支援 64 位元的 Windows。"
    Abort
  ${EndIf}
  SetRegView 64
  StrCpy $PS "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  ${IfNot} ${FileExists} $PS
    StrCpy $PS "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
  ${EndIf}

  ${IfNot} ${FileExists} "$EXEDIR\Hanin502.exe"
    MessageBox MB_ICONEXCLAMATION "找不到漢音輸入法的安裝檔:$\r$\n$EXEDIR\Hanin502.exe$\r$\n$\r$\n本安裝程式不包含漢音輸入法本身。請自行取得漢音 5.02 試用版的安裝檔 Hanin502.exe,放到本安裝檔的同一個資料夾後再執行一次。"
    Abort
  ${EndIf}

  ; SHA-256 of the user's file (guards against a modified / infected copy)
  nsExec::ExecToStack '"$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath $\'$EXEDIR\Hanin502.exe$\').Hash"'
  Pop $0
  Pop $1
  StrCpy $1 $1 64
  ${If} $1 != "${HANIN_SHA256}"
    MessageBox MB_ICONSTOP "Hanin502.exe 的 SHA-256 與原版不符,為了安全已停止安裝。$\r$\n$\r$\n預期:${HANIN_SHA256}$\r$\n實際:$1$\r$\n$\r$\n這個檔案可能被修改過(例如夾帶病毒),或是不同的版本。請改用原版的漢音 5.02 試用版安裝檔。"
    Abort
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  StrCpy $PS "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  ${IfNot} ${FileExists} $PS
    StrCpy $PS "$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
  ${EndIf}
FunctionEnd

Section "Install"
  SetShellVarContext all
  SetRegView 64

  DetailPrint "停止執行中的漢音引擎…"
  nsExec::ExecToLog '"$WINDIR\System32\taskkill.exe" /F /IM hanin_host.exe'
  nsExec::ExecToLog '"$WINDIR\System32\taskkill.exe" /F /IM hanin_tools.exe'
  Sleep 500

  SetOutPath "$INSTDIR"
  ; DLLs that running programs still hold cannot be overwritten, but can be renamed
  Delete "$INSTDIR\*.old-*"
  System::Call 'kernel32::GetTickCount() i .r9'
  ${If} ${FileExists} "$INSTDIR\HaninTip64.dll"
    Rename "$INSTDIR\HaninTip64.dll" "$INSTDIR\HaninTip64.dll.old-$9"
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\HaninTip32.dll"
    Rename "$INSTDIR\HaninTip32.dll" "$INSTDIR\HaninTip32.dll.old-$9"
  ${EndIf}
  ; leftovers of the early PowerShell-based installs
  Delete "$INSTDIR\install.ps1"
  Delete "$INSTDIR\uninstall.ps1"

  File "bin\HaninTip64.dll"
  File "bin\HaninTip32.dll"
  File "bin\hanin_host.exe"
  File "bin\hanin_tools.exe"
  File "extract-hanin.ps1"
  File "hanin-setup.ps1"
  File "..\LICENSE.txt"
  File "..\README.md"

  DetailPrint "驗證並解出 Hanin502.exe…"
  nsExec::ExecToLog '"$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\extract-hanin.ps1" -Source "$EXEDIR\Hanin502.exe" -Dest "$INSTDIR"'
  Pop $0
  ${If} $0 != "0"
    MessageBox MB_ICONSTOP "從 Hanin502.exe 解出檔案失敗(代碼 $0)。安裝已中止。"
    Abort
  ${EndIf}

  DetailPrint "註冊輸入法…"
  ExecWait '"$WINDIR\Sysnative\regsvr32.exe" /s "$INSTDIR\HaninTip64.dll"' $0
  ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s "$INSTDIR\HaninTip32.dll"' $1
  ${If} $0 != "0"
  ${OrIf} $1 != "0"
    MessageBox MB_ICONSTOP "輸入法註冊失敗(x64=$0, x86=$1)。"
    Abort
  ${EndIf}

  DetailPrint "加入鍵盤清單…"
  nsExec::ExecToLog '"$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\hanin-setup.ps1" -Action Install'

  WriteRegStr HKLM "${RUNKEY}" "HaninRevivalHost" '"$INSTDIR\hanin_host.exe" --server'

  CreateShortcut "$SMPROGRAMS\漢音工具箱.lnk" "$INSTDIR\hanin_tools.exe" "" "$INSTDIR\Tools95.exe" 0 SW_SHOWNORMAL "" "漢音輸入法工具箱(屬性設定、自用辭庫、鍵盤)"
  CreateShortcut "$SMPROGRAMS\漢音文字查詢.lnk" "$INSTDIR\hanin_tools.exe" "--app query" "$INSTDIR\QUERY.exe" 0 SW_SHOWNORMAL "" "漢音文字查詢(部首、筆畫查字)"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr   HKLM "${UNINSTKEY}" "Publisher" "Hanin Revival(非官方相容層)"
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\Tools95.exe,0"
  WriteRegStr   HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" 6000
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64

  nsExec::ExecToLog '"$WINDIR\System32\taskkill.exe" /F /IM hanin_host.exe'
  nsExec::ExecToLog '"$WINDIR\System32\taskkill.exe" /F /IM hanin_tools.exe'
  nsExec::ExecToLog '"$PS" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\hanin-setup.ps1" -Action Uninstall'
  ExecWait '"$WINDIR\Sysnative\regsvr32.exe" /u /s "$INSTDIR\HaninTip64.dll"'
  ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /u /s "$INSTDIR\HaninTip32.dll"'

  DeleteRegValue HKLM "${RUNKEY}" "HaninRevivalHost"
  DeleteRegKey HKLM "${UNINSTKEY}"
  Delete "$SMPROGRAMS\漢音工具箱.lnk"
  Delete "$SMPROGRAMS\漢音文字查詢.lnk"

  ; DLLs still loaded by running programs are removed at the next restart
  Delete /REBOOTOK "$INSTDIR\HaninTip64.dll"
  Delete /REBOOTOK "$INSTDIR\HaninTip32.dll"
  Delete /REBOOTOK "$INSTDIR\*.old-*"
  Delete "$INSTDIR\hanin_host.exe"
  Delete "$INSTDIR\hanin_tools.exe"
  Delete "$INSTDIR\Tools95.exe"
  Delete "$INSTDIR\QUERY.exe"
  Delete "$INSTDIR\extract-hanin.ps1"
  Delete "$INSTDIR\hanin-setup.ps1"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\README.md"
  RMDir /r "$INSTDIR\payload"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"

  MessageBox MB_ICONINFORMATION "已移除漢音輸入法復原版。$\r$\n$\r$\n你的個人設定與自用辭庫保留在:$\r$\n%LOCALAPPDATA%\HaninRevival$\r$\n不再需要的話可以自行刪除。"
SectionEnd
