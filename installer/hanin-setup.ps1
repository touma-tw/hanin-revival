<#
  hanin-setup.ps1 - 安裝程式的輔助步驟(由 NSIS 以系統管理員身分呼叫)。

  -Action Install    把漢音加入目前使用者的「中文(台灣)」鍵盤清單,並清除舊版開發安裝的殘留
  -Action Uninstall  從所有語言的鍵盤清單移除漢音
#>
param([ValidateSet('Install', 'Uninstall')][string]$Action = 'Install')
$ErrorActionPreference = 'Continue'

$tipId = '0404:{615F8932-792A-4588-8635-332CF2B7EC61}{0A18306E-52DC-4413-BF02-07B97906B415}'

function Remove-LegacyCertificate {
    # early development builds trusted a machine-local self-signed code-signing certificate;
    # it is not used any more (Easy Anti-Cheat does not accept it), so remove that trust.
    $subject = 'CN=Hanin Revival Local Code Signing'
    foreach ($storeName in 'Root', 'TrustedPublisher', 'My') {
        $st = [Security.Cryptography.X509Certificates.X509Store]::new($storeName, 'LocalMachine')
        $st.Open('ReadWrite')
        foreach ($c in @($st.Certificates | Where-Object { $_.Subject -eq $subject })) {
            $st.Remove($c)
            Write-Output "removed legacy certificate $($c.Thumbprint) from LocalMachine\$storeName"
        }
        $st.Close()
    }
}

if ($Action -eq 'Install') {
    $list = Get-WinUserLanguageList
    $tw = $list | Where-Object { $_.LanguageTag -like 'zh-Hant*' -or $_.LanguageTag -eq 'zh-TW' } | Select-Object -First 1
    if (-not $tw) {
        $list.Add('zh-Hant-TW')
        $tw = $list | Where-Object { $_.LanguageTag -like 'zh-Hant*' } | Select-Object -First 1
    }
    if (-not ($tw.InputMethodTips -contains $tipId)) { $tw.InputMethodTips.Add($tipId) }
    Set-WinUserLanguageList $list -Force -WarningAction SilentlyContinue
    Write-Output "keyboard list ($($tw.LanguageTag)): $($tw.InputMethodTips -join ', ')"

    Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'HaninRevivalHost' -ErrorAction SilentlyContinue
    Remove-LegacyCertificate
}
else {
    $list = Get-WinUserLanguageList
    foreach ($l in $list) { if ($l.InputMethodTips -contains $tipId) { [void]$l.InputMethodTips.Remove($tipId) } }
    Set-WinUserLanguageList $list -Force -WarningAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'HaninRevivalHost' -ErrorAction SilentlyContinue
    Remove-LegacyCertificate
    Write-Output 'removed from keyboard lists'
}
exit 0
