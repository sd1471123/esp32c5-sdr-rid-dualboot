# 一键构建三个 app（Windows PowerShell）
# 用法（在 ESP-IDF 环境外也能跑）：
#   powershell -ExecutionPolicy Bypass -File tools\build_all.ps1
param([switch]$SkipSetTarget)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $env:IDF_PATH) { $env:IDF_PATH = "D:\esp-idf-v6.1" }

$Py = "python"
if ($env:IDF_PYTHON_ENV_PATH) {
    $Candidate = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
    if (Test-Path $Candidate) { $Py = $Candidate }
}

function Invoke-Idf([string]$Project, [string[]]$Args) {
    & $Py (Join-Path $Root "tools\idfwrap.py") -C (Join-Path $Root $Project) @Args
    if ($LASTEXITCODE -ne 0) { throw "idf.py failed in $Project" }
}

function Build-One([string]$Project, [string]$Defaults) {
    Write-Host "==> building $Project"
    Remove-Item (Join-Path $Root "$Project\sdkconfig") -ErrorAction SilentlyContinue
    if (-not $SkipSetTarget) {
        if ($Defaults) { $env:SDKCONFIG_DEFAULTS = $Defaults }
        Invoke-Idf $Project @("set-target", "esp32c5")
    }
    if ($Defaults) { $env:SDKCONFIG_DEFAULTS = $Defaults }
    Invoke-Idf $Project @("build")
}

Build-One "launcher" $null
Build-One "sdr" "sdkconfig.defaults;sdkconfig.defaults.esp32c5;sdkconfig.defaults.esp32c5-lcd"
Build-One "rid" $null
Remove-Item Env:SDKCONFIG_DEFAULTS -ErrorAction SilentlyContinue

$Out = Join-Path $Root "out"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item (Join-Path $Root "launcher\build\bootloader\bootloader.bin") (Join-Path $Out "bootloader.bin") -Force
Copy-Item (Join-Path $Root "launcher\build\partition_table\partition-table.bin") (Join-Path $Out "partition-table.bin") -Force
Copy-Item (Join-Path $Root "launcher\build\ota_data_initial.bin") (Join-Path $Out "ota_data_initial.bin") -Force
Copy-Item (Join-Path $Root "launcher\build\dual_boot_launcher.bin") (Join-Path $Out "launcher.bin") -Force
Copy-Item (Join-Path $Root "sdr\build\esp_sdr.bin") (Join-Path $Out "sdr.bin") -Force
Copy-Item (Join-Path $Root "rid\build\remoteid_scanner.bin") (Join-Path $Out "rid.bin") -Force
Copy-Item (Join-Path $Out "rid.bin") (Join-Path $Out "rid_a.bin") -Force
Copy-Item (Join-Path $Out "rid.bin") (Join-Path $Out "rid_b.bin") -Force
Copy-Item (Join-Path $Root "partitions\merged_16mb.csv") (Join-Path $Out "partition-table.csv") -Force

Write-Host "Build done. Flash with: powershell -ExecutionPolicy Bypass -File tools\flash_all.ps1 -p COMx"
