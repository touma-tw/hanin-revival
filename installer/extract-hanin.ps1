<#
  extract-hanin.ps1 - 從使用者自備的漢音 5.02 安裝檔(Hanin502.exe)解出引擎與字典。

  Hanin502.exe 是 2001 年的 Wise Installer(16 位元外殼),在 64 位元 Windows 上無法執行。
  檔案內容是一連串 raw deflate 串流;因為先以 SHA-256 確認是同一個檔案,各檔的位置可以寫死,
  解出後每個檔案再各自比對大小與 SHA-256。

  本專案不散佈任何 Panasonic 的檔案;這支腳本只處理使用者自己取得的安裝檔。

  用法:extract-hanin.ps1 -Source <Hanin502.exe> -Dest <安裝目錄>
  結束代碼:0 成功、2 找不到檔案、3 安裝檔 SHA-256 不符、4 解出的檔案驗證失敗
#>
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Dest
)
$ErrorActionPreference = 'Stop'

$InstallerSha256 = 'C8EE45C4F0C24DE502912DD1747A66481AD7CFB6A953540B814205458C9B5814'
$InstallerSize   = 2560780

$Files = @(
    @{ Offset = 0x000A386F; Path = 'payload\HANIN95.IME'; Size = 172032; Sha256 = '074946D052FAC5D0347C78F172D34A79C283B6A6947354910DBA6FAEDCB20569' }
    @{ Offset = 0x0005616F; Path = 'payload\HANIN.DIC'; Size = 448295; Sha256 = '3CADDC291FFDA9F7033F62EF6FF4F8CA7B10CCC0DA08B70657B53FAF03ED1FCE' }
    @{ Offset = 0x0002B034; Path = 'payload\AIMEI.DIC'; Size = 36457; Sha256 = '0DA95D1B6120A485A74F0C0D0B2361903C23896EFFCBF515CE2D24E42494B50A' }
    @{ Offset = 0x00016761; Path = 'payload\ADR.DIC'; Size = 127243; Sha256 = '48402CD96889A7759F8DC113C9AA4E97D30BA2BBB037935328B4FCE47EE934C7' }
    @{ Offset = 0x0003D1BE; Path = 'payload\NAME.DIC'; Size = 158653; Sha256 = 'B887F3A0F723E118E23F85EAA53F8CF695098B08F49F530B09807DF3AC435283' }
    @{ Offset = 0x0002EB53; Path = 'payload\HQUERY.DIC'; Size = 90024; Sha256 = '8DFFA6D28D69939A4016E0983C27E7E27514E708D71F02DBA1BF1B5F54A9D4AF' }
    @{ Offset = 0x000CC7DF; Path = 'payload\whanin.ini.orig'; Size = 528; Sha256 = '2DFAB89CE05B872567014C6F29BD32F1097B79697843E2CDDC07B8FBA9029514' }
    @{ Offset = 0x000CC92F; Path = 'Tools95.exe'; Size = 245760; Sha256 = 'C27986A6958803BC1CEE452373F3EE3C089DDF1F424C866B69FA212BCD115620' }
    @{ Offset = 0x000DBE6D; Path = 'QUERY.exe'; Size = 195072; Sha256 = '33DEF0ABBD2B4A79E847A6E1FF73E3E844102F818396AF686245FFE8FC826D30' }
    @{ Offset = 0x000EC74A; Path = 'payload\EXPERT\BIOLOGY.DIC'; Size = 169886; Sha256 = 'B21CA09C092545F2DA83D6905CD2A2348D44CD9084DFD88E44C0C4CEE9AEC31B' }
    @{ Offset = 0x00109DAA; Path = 'payload\EXPERT\BUDDHISM.DIC'; Size = 119901; Sha256 = '313F11DAA4D0D0CD2072DC1933E79AD7316873FAF91652482A4147709B5AE974' }
    @{ Offset = 0x0011E799; Path = 'payload\EXPERT\BUILT.DIC'; Size = 156658; Sha256 = 'DDC13A0CDB07610CB2DDF051A2E17E5F77A5C54ECC38557F00ACF1E45045DBBA' }
    @{ Offset = 0x0013A223; Path = 'payload\EXPERT\CMEDIC.DIC'; Size = 150542; Sha256 = '9FB6CDA18C235096C9BDC2559714094A6B6877578303BAE3483C080A8D3F0810' }
    @{ Offset = 0x00154B28; Path = 'payload\EXPERT\COMMERCI.DIC'; Size = 203645; Sha256 = '8B0E7D806FBE9A16370F33BA2E565944395DF3B7477ECB3713A84868360F3CDD' }
    @{ Offset = 0x00176427; Path = 'payload\EXPERT\COMPUTER.DIC'; Size = 150266; Sha256 = '14893B77C735A53A753E0ED7075710FE1DB2637649D7E4B2CDE408A57F35D950' }
    @{ Offset = 0x0018FC67; Path = 'payload\EXPERT\EDUCATE.DIC'; Size = 142677; Sha256 = '7BDDB2BBC1016E36322B4B1EA8E8FC04D093B79563A0459963D2FD84831D9B94' }
    @{ Offset = 0x001A8B8D; Path = 'payload\EXPERT\INDUSTRY.DIC'; Size = 164199; Sha256 = '85674450121EDDBB58726C5400ACC3382AE55EBB67764C1D0144EF7A4FC9D6FE' }
    @{ Offset = 0x001C55E9; Path = 'payload\EXPERT\LAW_POLI.DIC'; Size = 167092; Sha256 = '87E1150333405C2E20545539F8E1CFFF49D860DAC42D7D387C83EDDA0259FAAF' }
    @{ Offset = 0x001E25CE; Path = 'payload\EXPERT\MEDICAL.DIC'; Size = 177664; Sha256 = '78621C3BF25397AB28D34CE5FC98685AB0A5F38D18F6CB71EC896BA7E6BE754C' }
    @{ Offset = 0x00201AA7; Path = 'payload\EXPERT\MILITARY.DIC'; Size = 151084; Sha256 = 'F0C742CE2E2A7C8EF07C04AF4A9F3C9776F37810D621063120061B0E7995F570' }
    @{ Offset = 0x0021C42F; Path = 'payload\EXPERT\PHY_CHEM.DIC'; Size = 196550; Sha256 = '28C9A9B32AB5D1E0F0E1C722435936734034BB98D341F6D2E486F4C5A5572882' }
    @{ Offset = 0x0023E40A; Path = 'payload\EXPERT\SPORT.DIC'; Size = 154588; Sha256 = '53093DB32D7385FC8E8CD901E1CDF1A6BD7DBC6D6400DF9103CFD4F1897E00CF' }
    @{ Offset = 0x00259671; Path = 'payload\EXPERT\TRAFFIC.DIC'; Size = 114705; Sha256 = 'D0E6AC8248EBA498DEC2CCF519C69F7BB7DD6B4758B4635636982552A107A6FF' }
)

function Get-Sha256Hex([byte[]]$data) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($data))).Replace('-', '') } finally { $sha.Dispose() }
}

if (-not (Test-Path -LiteralPath $Source)) {
    Write-Output "找不到 $Source"
    exit 2
}
$bytes = [IO.File]::ReadAllBytes($Source)
$hash = Get-Sha256Hex $bytes
if ($bytes.Length -ne $InstallerSize -or $hash -ne $InstallerSha256) {
    Write-Output "SHA-256 不符:$hash($($bytes.Length) bytes)"
    exit 3
}

foreach ($f in $Files) {
    $offset = [int]$f.Offset
    $in = [IO.MemoryStream]::new($bytes, $offset, ($bytes.Length - $offset), $false)
    $deflate = [IO.Compression.DeflateStream]::new($in, [IO.Compression.CompressionMode]::Decompress)
    $out = [IO.MemoryStream]::new()
    try { $deflate.CopyTo($out) } finally { $deflate.Dispose() }
    $data = $out.ToArray()
    $got = Get-Sha256Hex $data
    if ($data.Length -ne $f.Size -or $got -ne $f.Sha256) {
        Write-Output "解出的 $($f.Path) 驗證失敗(大小 $($data.Length)、SHA-256 $got)"
        exit 4
    }
    $target = Join-Path $Dest $f.Path
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    [IO.File]::WriteAllBytes($target, $data)
}
New-Item -ItemType Directory -Force (Join-Path $Dest 'payload\USR_FILE') | Out-Null
Write-Output "OK:已解出並驗證 $($Files.Count) 個檔案"
exit 0
