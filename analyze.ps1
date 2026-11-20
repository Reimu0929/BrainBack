<#
.SYNOPSIS
  SHARP Brain (PW-AC900) の AppMain.exe を REA で解析するヘルパースクリプト。

.DESCRIPTION
  Phase A: エンジン不要の PE 静的解析 (imports / resources / exports / hardening)
  Phase B: Ghidra による analyze / search / decompile-all
  結果は out\<ファイル名>\ に JSON として保存する。

.EXAMPLE
  .\analyze.ps1 -Binary .\AppMain.exe
  .\analyze.ps1 -Binary C:\dumps\AppMain.exe -SkipGhidra
#>
param(
  [Parameter(Mandatory = $true)]
  [string]$Binary,

  [switch]$SkipGhidra,

  [string]$Provider = 'ghidra'
)

$ErrorActionPreference = 'Stop'

$jdk = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.5.10002-hotspot'
$ghidra = 'C:\tools\ghidra_12.2_PUBLIC'
$env:JAVA_HOME = $jdk
$env:GHIDRA_INSTALL_DIR = $ghidra
$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [System.Environment]::GetEnvironmentVariable('Path', 'User') + ';' + "$jdk\bin"

$full = (Resolve-Path -LiteralPath $Binary).Path
$name = [System.IO.Path]::GetFileNameWithoutExtension($full)
$outdir = Join-Path (Get-Location) "out\$name"
New-Item -ItemType Directory -Force $outdir | Out-Null

Write-Host "=== REA analysis: $full ===" -ForegroundColor Cyan
Write-Host "Output: $outdir"

function Invoke-Rea {
  param([string]$Tag, [string[]]$Args)
  $file = Join-Path $outdir "$Tag.json"
  Write-Host "--> $Tag"
  & rea @Args --json 2>&1 | Set-Content -LiteralPath $file -Encoding UTF8
  if ($LASTEXITCODE -ne 0) {
    Write-Warning "$Tag failed (exit $LASTEXITCODE). See $file"
  }
}

# ---- Phase A: engine-free static PE inspection ----
Invoke-Rea '01_pe_imports'     @('inspect-pe-imports', $full)
Invoke-Rea '02_pe_resources'   @('inspect-pe-resources', $full)
Invoke-Rea '03_pe_certificates'@('inspect-pe-certificates', $full)
Invoke-Rea '04_native_exports' @('inspect-native-exports', $full)
Invoke-Rea '05_hardening'      @('inspect-binary-hardening', $full)
Invoke-Rea '06_artifact'       @('inspect-artifact', $full)

# ---- Phase B: Ghidra native analysis ----
if (-not $SkipGhidra) {
  Invoke-Rea '10_analyze'       @('analyze', $full, '--provider', $Provider)
  Invoke-Rea '11_search_TextMemo' @('search', $full, 'TextMemo', '--provider', $Provider)
  Invoke-Rea '12_search_CreateFile' @('search', $full, 'CreateFile', '--provider', $Provider)
  Invoke-Rea '13_search_FSKAREN' @('search', $full, 'FSKAREN', '--provider', $Provider)
  Invoke-Rea '14_decompile_all'  @('decompile-all', $full, '--provider', $Provider)
}

Write-Host "=== Done. Results in $outdir ===" -ForegroundColor Green
