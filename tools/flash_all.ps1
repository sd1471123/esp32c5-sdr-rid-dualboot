# 整片烧录（Windows PowerShell）
#   powershell -ExecutionPolicy Bypass -File tools\flash_all.ps1 -p COM6
#   powershell -ExecutionPolicy Bypass -File tools\flash_all.ps1 -p COM6 -Erase
param([string]$p = "COM6", [switch]$Erase)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $env:IDF_PATH) { $env:IDF_PATH = "D:\esp-idf-v6.1" }
$Py = "python"
if ($env:IDF_PYTHON_ENV_PATH) {
    $Candidate = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
    if (Test-Path $Candidate) { $Py = $Candidate }
}

$Common = @("-m", "esptool", "--chip", "esp32c5", "-p", $p, "-b", "460800",
            "--before", "default-reset", "--after", "hard-reset")
if ($Erase) { & $Py @Common erase-flash }

$Images = @(
    @("0x2000",  "bootloader.bin"),
    @("0x8000",  "partition-table.bin"),
    @("0x10000", "ota_data_initial.bin"),
    @("0x20000", "launcher.bin"),
    @("0x120000","sdr.bin"),
    @("0x320000","rid_a.bin"),
    @("0x620000","rid_b.bin")
)
$Args = $Common + @("write-flash", "--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m")
foreach ($img in $Images) { $Args += @($img[0], (Join-Path $Root ("out\" + $img[1]))) }
& $Py @Args
if ($LASTEXITCODE -ne 0) { throw "esptool failed" }
Write-Host "Flashed. USB-Serial-JTAG console at 115200 baud."
