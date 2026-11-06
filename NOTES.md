# 漢音輸入法復原 — 工作筆記

## 來源
- 使用者自備的 `Hanin502.exe`:漢音 5.02 試用版(Panasonic Taiwan Laboratories, 2001),Wise Installer,16-bit NE 外殼(Win11 x64 無法執行)。
- 開發初期以 raw-deflate 暴力掃描解出全部檔案;正式版由 `installer/extract-hanin.ps1` 依固定位置解出並逐檔驗證 SHA-256。
  - Wise 腳本位置未找出,檔名依內容/順序推斷:`HANIN.DIC`(019, 448KB)、`AIMEI.DIC`(016, 格式不同)、
    `ADR.DIC`/`NAME.DIC`(015/018,**可能互換,未驗證**)、`HQUERY.DIC`(017, "MITT Query V1.0")、
    `EXPERT/*.DIC`(029–042,依字母順序對應,**未驗證**)。
  - `whanin.ini.orig` 是原始設定;`whanin.ini` 會被引擎寫回(StatusXPos 等)。

## HANIN95.IME 事實
- PE32 x86 DLL,1997-07-02 編譯,ANSI/Big5,IMM32 IME(UI class `Hanin95`)。
- 只匯入 8 個 IMM32 函式:ImmLockIMC/UnlockIMC/CreateIMCC/DestroyIMCC/LockIMCC/UnlockIMCC/ReSizeIMCC/GenerateMessage。
- fdwProperty=0x0004000a(KBD_CHAR_FIRST | NEED_ALTKEY | CANDLIST_START_FROM_1)。
- **ImeToAsciiEx 從 uVirtKey 的 bits 8–15 讀字元碼**(Win95 ANSI 慣例,不是 HIWORD)。
- 會呼叫危險 API:`SetCursorPos`(開選字窗時把滑鼠移過去)、`ClipCursor`、`keybd_event`、`WinExec`、
  `SetForegroundWindow`、`EnumWindows`/`FindWindowA` — 推測是舊版在 Win10 上卡死/怪異的來源之一。
- 寫死 `C:\HANIN\...` 路徑、在 Windows 目錄讀寫 `whanin.ini`。
- 32 位元 IME 無法載入 64 位元行程 → 這才是在現代 Windows「不能用」的根本原因。

## Phase 0 宿主(host/hanin_host.c)— 已驗證可用
- `LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)` 映射 IME → 自己填 IAT → 手動呼叫 DllMain。
- IMM32 8 函式自行實作(HIMCC = `LocalAlloc(LHND)`,與真 IMM32 相同)。
- 路徑重導 `C:\HANIN` → `payload/`;危險 API 一律攔截記 log。
- 測試結果(2026-11-06):
  - `su3cl3⏎` → 你好
  - `ji3g4vm,6g/ ⏎` → 我是學生
  - `rup wu0 wu0 fu4cp3cl3⏎` → 今天天氣很好(長句自動選字正確)
  - `u4␣` → 開同音字窗,CANDIDATEINFO 有 155 個候選;翻頁/選字可運作
## Phase 1 — TSF 前端(2026-11-06 完成並安裝)

### 架構
```
應用程式(x64/x86) ── HaninTip64/32.dll(TSF TIP)──具名管道 \\.\pipe\HaninRevival.<session>──▶ hanin_host.exe(x86, 原版引擎)
```
- `common/protocol.h`:固定大小 REQ/RESP(TEST/KEY/RESET/COMPLETE/TOGGLE/PING)。
- `host/hanin_host.c`:伺服器模式。管道執行緒 → SendMessage 到主執行緒序列化呼叫引擎;
  看門狗:單次請求 >4s 即自殺;引擎例外即自殺(前端自動重啟)。原版視窗改為完全透明、點擊穿透、不搶焦點;
  選字窗內容靠「強制重繪 + 攔截 TextOutA」擷取成 R/F/T 行格式。
  `--test <dataDir> "<keys>"` 走與伺服器相同的請求路徑;另有 `{MEMSNAP}{MEMDIFF}` 記憶體比對工具。
- `tip/HaninTip.cpp`:TIP。管道 overlapped I/O、寫 0.8s / 讀 1.5s 硬逾時、失敗後退避;找不到宿主時自動啟動
  (AppContainer/低完整性行程不啟動)。行內組字 + 虛線底線(display attribute)、自繪 DPI-aware 選字窗、
  中/英同步到 `GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION`(工作列中/英指示器,點擊可反向切換)。
- 安裝:`install/make-dist.ps1` → `dist/`;`dist/install.ps1`(UAC 一次)裝到 `C:\Program Files\HaninRevival`,
  regsvr32 x64/x86、加入使用者 zh-TW 鍵盤清單、HKCU Run 開機啟動宿主。使用者資料在 `%LOCALAPPDATA%\HaninRevival\data`。

### 重要發現
- 漢音中/英狀態不在 fdwConversion;在 UI 視窗 `GetWindowLong(hUI, 4)`(IMMGWL_PRIVATE)指向的 HGLOBAL 結構 `+0x5F2`(0=中、1=英)。
  Shift **放開**時在 ImeProcessKey(TEST 階段)就切換。
- `NotifyIME(CPS_COMPLETE)` 原版是用 **全系統 keybd_event(VK_RETURN)** 實作 → 已攔截;COMPLETE 改由宿主自己送 ESC/ENTER。
- 開選字窗時原版會 `SetCursorPos` 把滑鼠移過去 → 已攔截。
- 測試陷阱:CUAS 編輯框下,App 直接呼叫 `ITfKeystrokeMgr::TestKeyDown` 不會派送到 TIP;只有前景執行緒的真實輸入才會
  → `test/tiptest.cpp` 會短暫取得前景並用 SendInput,且每鍵前確認前景未變。

### 除錯
- 宿主詳細 log:`hanin_host.exe --server --verbose`(或環境變數 `HANIN_VERBOSE=1`)→ `%LOCALAPPDATA%\HaninRevival\data\hanin_host.log`。
  預設**不記錄按鍵內容**,只記錯誤與被攔截的危險 API。
- TIP log:`%LOCALAPPDATA%\HaninRevival\tip.log`(錯誤);設 `HANIN_TIP_DEBUG=1` 才記按鍵流程。
- 開發迴圈免 UAC:`tip/devreg.cmd` 把 HKCU COM 覆寫指向 `tip/dev/` 的新 DLL(提權程式會忽略 HKCU);
  **用完要刪** `HKCU\Software\Classes\CLSID\{615F8932-792A-4588-8635-332CF2B7EC61}`(2026-11-06 已刪)。

### 已驗證
- x64 / x86 TSF 端對端:長句、選字窗(格狀、作用中列、頁碼)、Shift 中英切換+指示器、ESC、行內組字顯示與選字窗定位。
- 強制終止宿主:App 不凍結,只遺失當下未送出的組字,宿主自動重啟。

### 鍵盤排列(whanin.ini `InputMethod`)
- 原版工具箱選「倚天」寫入的是 **`3`**(使用者目前設定);`1` = 標準(大千)。
  `0` 不是合法值(工具箱顯示無選取),但引擎會退回成倚天行為 —— 早先手動設 0 是誤判,已由工具箱改成 3。
- 工具箱改設定會即時 `SendMessage(0x962, 0x323/0x324, 值)` 給引擎的 `Hanin95` UI 視窗,不必重啟。

### ⚠ 檔案系統虛擬化陷阱(2026-11-06 發現並修正)
- 從 Claude 桌面版(MSIX)內啟動的行程,寫入 `%LOCALAPPDATA%` 會被重導到
  `%LOCALAPPDATA%\Packages\Claude_<id>\LocalCache\Local\...`,而且**這些行程沒有封裝身分**
  (`GetCurrentPackageFullName` 回 15700),只能用行為偵測。Explorer 啟動的程式看不到這份資料。
- 症狀:使用者從開始功能表開工具箱 → 找不到資料夾而失敗;重開機後引擎會用預設值(標準鍵盤)重建資料。
- 修正(`common/outside_pkg.h`):宿主與工具箱啟動時寫探測檔偵測重導,若被隔離就以 Explorer 為父行程
  (`PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`)重啟自己;宿主第一次在真實位置建立資料時,先從虛擬副本搬回設定與使用者詞庫。
- **開發/測試時要看真實資料**:用 `explorer.exe probe.cmd` 讓腳本在外部執行,輸出寫到 F:\(不受虛擬化影響)。

### 系統 UTF-8(ACP 65001)相容(2026-11-06)
- 使用者開了「Beta:使用 Unicode UTF-8 提供全球語言支援」→ 工具箱執行期 ANSI 文字亂碼(資源檔文字正常)。
- `common/legacy_cp.manifest`(`activeCodePage=Legacy` + gdiScaling)嵌入 hanin_host.exe 與 hanin_tools.exe
  → 兩者固定用系統地區的 950,不受全系統 UTF-8 影響(引擎內的 CharNextA 等也因此正確)。

### 快速鍵(原版工具箱「快速鍵」頁,whanin.ini `HotKey=`)
- TIP 現在把 Ctrl / Ctrl+Alt 組合交給引擎(單獨 Alt+鍵 仍直接給 App)。實測引擎**不吃** Ctrl+C/V/X/Z/A/S/Y/F/W。
- 可用:Shift 中英切換、Shift+Enter/Shift+Delete、Ctrl+` 模式循環、Ctrl+3 特殊符號表、Ctrl+/ 曖昧音字集
  (後兩者是選字窗型視窗,走自繪選字窗)、Ctrl+6 工具箱、Ctrl+2 文字查詢。
- Ctrl+6/Ctrl+2:引擎 `WinExec("C:\HANIN\tools95|query95 <arg>")` → 宿主改為啟動 `hanin_tools.exe [--app query] <arg>`。
- 不可用:Ctrl+1 快速功能鈕、Ctrl+5 螢幕鍵盤、Ctrl+8(原版自有視窗目前全透明,且點選結果需要宿主→TIP 的非同步通道);
  Ctrl+7 說明(Hanin95.hlp,Win10+ 無 WinHelp);Ctrl+4 重複句庫(引擎吃鍵但無可見反應,未研究)。
- 注意:Ctrl+1~8 會蓋掉瀏覽器切分頁等 App 快捷鍵,可在工具箱「快速鍵」頁改成「(無)」。
- 文字查詢(QUERY.exe,有重定位但同樣固定 0x400000)用同一啟動器 `--app query`;查到的字回傳給引擎後
  能否送進 App 尚未驗證(同樣需要非同步通道)。

### 程式碼簽章(Easy Anti-Cheat / VRChat)
- VRChat 的 EAC 擋下未簽章的 HaninTip64.dll(「不受信任的系統檔」)。
- 使用者決定先試本機自簽:install.ps1 在 LocalMachine\My 建立 `CN=Hanin Revival Local Code Signing`
  (僅程式碼簽署 EKU、私鑰不可匯出、20 年),公開部分加入 LocalMachine\Root 與 TrustedPublisher,
  簽 HaninTip64/32.dll、hanin_host.exe、hanin_tools.exe。uninstall.ps1 會把憑證從三個存放區移除。
- 容器外驗證:四檔 `Valid`。**EAC 是否接受本機自簽根憑證尚待使用者在 VRChat 實測**;不行就要改用正式憑證。
- 開機自動啟動改寫 HKLM Run(HKCU 寫入會被 Claude 容器虛擬化,外面看不到)。
- VRChat 的「每個應用程式視窗使用不同輸入法」問題屬 VRChat/Unity 行為(微軟輸入法也一樣),輸入法端無解;維持不勾選。

### EAC 實測結果與語言列(2026-11-06)
- **EAC 不接受本機自簽**(使用者實測,同樣「不受信任的系統檔」)。新酷音的 chewing_tip.dll 由
  SignPath Foundation(GlobalSign 簽發,免費給開源專案)簽章 → 正式簽章候選路線;需公開開源 repo,
  且簽章產物不能含 Panasonic 原版檔(安裝時改由使用者的 Hanin502.exe 解出)。待使用者決定。
- 語言列(最終):三個按鈕,仿新酷音配置 —— 浮動語言列上「中 中文/英文切換」(`GUID_HaninLbiMode`)、
  「設定/工具選單」(`GUID_HaninLbiMenu`,BTN_MENU);工作列指示器 `GUID_LBI_INPUTMODE`。已截圖確認浮動語言列顯示。
  使用者第一次回報「沒按鈕、快速鍵沒作用」時根本還沒登出,所有程式載著最舊版 DLL。
- (舊紀錄)語言列:兩個按鈕。`GUID_LBI_INPUTMODE`「中/英」(工作列輸入指示器用;使用者開了浮動桌面語言列時不顯示),
  自訂 `GUID_HaninLbiMenu`「漢音」選單按鈕(浮動語言列上可見,已截圖確認)。選單:工具箱、文字查詢、中英切換;
  啟動程式走 `HCMD_LAUNCH` 由宿主執行(宿主在容器外、AppContainer App 也可用)。
- 補 `GUID_TFCAT_TIPCAP_COMLESS`;ActivateEx 改用 `TF_CreateCategoryMgr`(不依賴 COM 初始化)。
  Windows「覆寫預設輸入法」清單看不到漢音的原因未確認(新酷音多宣告 COMLESS/UIELEMENTENABLED)。
- 快速鍵「沒作用」實因:使用者所有程式都在 Ctrl 放行版本安裝前啟動,仍載著舊 DLL(已載入 DLL 無法熱替換)。
  **每次重裝後要登出再登入**。宿主讀 HotKey,把快速功能鈕/螢幕鍵盤/說明/橫直排 對應的鍵還給 App。
- Ctrl+4 重複句庫可用(需先打過句子);Shift+Enter 線上新詞登錄:見下方「線上新詞登錄與自用辭典存檔」。

### Phase 2 — 原版工具箱(2026-11-06 完成並安裝)
- `tools/hanin_tools.c` → `hanin_tools.exe`(x86,`/BASE:0x10000000 /DYNAMICBASE:NO`)。開始功能表捷徑「漢音工具箱」。
- Tools95.exe 去除了重定位、固定 0x400000;但行程啟動時 0x400000 已被系統映射(MEM_MAPPED,非 manifest 造成)
  → 啟動器以 `--child` **暫停狀態**重新啟動自己,先 `VirtualAllocEx` 保留 0x400000,子行程再手動映射各區段、自填 IAT、跳進 EP。
- 攔截:GetWindowsDirectoryA、OpenFile/_lopen/_lcreat/DeleteFileA/FindFirstFileA(C:\HANIN 重導)、*PrivateProfile*、
  GetModuleHandleA(NULL)/GetModuleFileNameA(NULL)(回傳 Tools95 本身,資源才載得到)、ExitProcess(有寫入就重啟引擎宿主)。
- log:`%LOCALAPPDATA%\HaninRevival\data\hanin_tools.log`。
- 已驗證:主畫面、漢音屬性設定、切換倚天即時生效、關閉後引擎自動重啟。
  未驗證:自用辭庫、略語、自定注音鍵盤編輯器等其他工具;引擎未啟動時開工具箱,設定只寫檔、關閉後才生效。

### 未完成 / 待驗證
- 市集(UWP)App、開始功能表搜尋框(AppContainer 管道 ACL 已設,未實測)。
- 選字窗滑鼠點選;漢音工具箱(Tools95)、使用者造詞;ADR/NAME 與專業詞庫檔名對應未驗證。
- 宿主卡死(非崩潰)情境只靠設計推論:前端 1.5s 逾時 + 宿主 4s 看門狗,未實際模擬。
- 焦點切換時有組字中的文字:以 COMPLETE 送出,未用多視窗實測。

### Phase 3 — 散佈用安裝程式與開源(2026-11-06)
- `installer/HaninRevival.nsi`(NSIS 3,Unicode,需 UTF-8 BOM):檢查同目錄 `Hanin502.exe` → SHA-256
  (`C8EE45C4…5814`)→ `extract-hanin.ps1` 依固定位移以 .NET DeflateStream 解出 23 檔並逐檔驗證 SHA-256
  → regsvr32(Sysnative/SysWOW64)→ `hanin-setup.ps1`(使用者鍵盤清單、清除舊版自簽憑證與 HKCU Run)
  → HKLM Run、全體使用者開始功能表捷徑、解除安裝項目。`/DTESTNOADMIN` 產生只跑檢查流程的測試版。
- 安裝檔只含本專案程式;原版圖示由安裝程式執行 `hanin_host --export-icon` 從使用者自己的 `HANIN95.IME`
  (圖示群組索引 1,僅一張 16×16)匯出成 `payload\hanin.ico`;我們自己的備用圖示為 `tip/hanin_fallback.ico`。
- 散佈版**不簽章**(本機自簽對 EAC 無效);DllUnregisterServer 以 `ITfInputProcessorProfiles::Unregister` 清除整個 TIP 機碼。
- 建置:`installer\build-release.ps1 -Version x.y.z` → `release\HaninRevival-Setup-x.y.z.exe`。
- 測試:缺檔、SHA 不符(竄改 1 bit)、正常;容器外靜默安裝 → 解除安裝(x64/x86 註冊全清)→ 重裝,皆通過。
- git:本機 repo 已初始化並 commit,`.gitignore` 排除所有 Panasonic 檔案、反組譯、原版圖示、建置產物;尚未推送。

### Defender 誤判 HANIN95.IME(2026-11-06,修正於 0.1.1)
- v0.1.0 把 TSF 設定檔圖示與語言列圖示直接指向 `payload\HANIN95.IME`,所有顯示輸入法圖示的行程都會開啟它。
  16:23 rundll32(文字服務對話框)讀取時,Defender 即時保護判為 `Trojan:Win32/PShellDown.SB`(嚴重)並隔離。
- 同一檔案(SHA `074946D0…0569`,與原版一致)以 `MpCmdRun -Scan -ScanType 3` 靜態掃描:no threats;
  hanin_host 當天以同版病毒碼(1.463.147.0)多次載入也未觸發 → 判斷為情境式誤判。
- 修正:圖示改用安裝時匯出的 `hanin.ico`,只剩 hanin_host / hanin_tools 會讀 IME。
  引擎實際載入的是 `%LOCALAPPDATA%\HaninRevival\data\HANIN95.IME` 副本(未被隔離),所以當時打字不受影響。
- 未能重現:Win11 上 `rundll32 shell32.dll,Control_RunDLL input.dll,,{C07337D3-…}` 不會開出對話框。
- 待辦:向 Microsoft 回報誤判(使用者自行提交)。

### 線上新詞登錄與自用辭典存檔(2026-11-06,0.1.1)
- Shift+Enter 是快速鍵表索引 1(whanin.ini `HotKey` 第 2 個值 525 = Shift+0x0D)。按鍵分類函式 `0x61D560`
  的跳躍表 `0x61DCE4` → `0x61D6B0`:需 組字長度 `[+602]` ≥ 4 bytes、`[+604]==0`、且 `[+5FC]==1` 才回 0x12,否則放行。
- `[+5FC]` = 游標已移進組字區(←:`0x614370` 設 1;→ 回到最右端:`0x614430` 清 0)。
  所以操作方式是:游標移到新詞開頭再按 Shift+Enter,登錄 游標..結尾 的 2~5 字(`0x6077E0` → `0x605320`)。
  **引擎本來就正常**;之前測試直接按 Shift+Enter,游標在最右端所以被放行。
- 真正的缺陷:新詞與學習結果只寫入記憶體表 `0x6252D0`(文字以 XOR 0x2A 存放),並設待存檔旗標
  `[0x62479A]`(另一旗標 `[0x247AC]`);寫回 `$USR000.DIC` 的 `0x604EA0`(無參數)原版只在 UI 事件
  (`0x60B260`、UI 視窗訊息 0x321..0x325)時呼叫,宿主平常不會觸發(使用者資料夾的辭典 15:53 有寫入,推測是工具箱送訊息觸發,未證實)
  → 宿主重啟(登出、崩潰復原、工具箱結束)前沒存到的就遺失。
- 修正:宿主每次處理完非 TEST 請求後,若旗標非 0 就呼叫 `0x604EA0`。呼叫前核對機器碼
  `66 83 3D <&dirty>`,不符則停用並記錄 WARNING。實測:登錄「八八八」後檔案寫回,XOR 編碼的字串出現在檔中。
- 測試陷阱:原版用 `OpenFile`(OFS_MAXPATHNAME=128),測試資料夾路徑過長時自用辭典開檔失敗(err 13,
  「自用辭典檔案錯誤」)。測試用短路徑(例如 `release\hd`)。宿主測試指令 `{S+ENTER}`/`{S+DEL}` 依真實順序送 Shift。
