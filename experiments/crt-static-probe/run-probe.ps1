# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Consumer,
    [Parameter(Mandatory = $true)][string]$Architecture,
    [Parameter(Mandatory = $true)][string]$Generator,
    [string]$Toolset = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-$Architecture-$Consumer"
$configure = @(
    '-S', $PSScriptRoot,
    '-B', $build,
    '-G', $Generator,
    '-A', $Architecture,
    "-DPROBE_CONSUMER=$Consumer",
    '-DCMAKE_BUILD_TYPE=Release'
)
if (-not [string]::IsNullOrWhiteSpace($Toolset)) {
    $configure += @('-T', $Toolset)
}
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw "configure failed for $Consumer" }
& cmake --build $build --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw "build failed for $Consumer" }

$exe = Join-Path $build 'Release\probe.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "missing $exe" }
$dll = Join-Path $build 'Release\crt_probe.dll'
if ($Consumer -eq 'shared') {
    if (-not (Test-Path -LiteralPath $dll)) { throw "missing $dll" }
    Copy-Item -LiteralPath $dll -Destination (Split-Path -Parent $exe) -Force
}

$dumpbin = Get-ChildItem `
    'C:\Program Files\Microsoft Visual Studio\*\VC\Tools\MSVC\*\bin\Host*\*\dumpbin.exe' `
    -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $dumpbin) { throw 'dumpbin not found' }
Write-Host "==> $Consumer executables"
& $dumpbin.FullName /dependents $exe
if ($Consumer -eq 'shared') {
    Write-Host '==> DLL imports'
    & $dumpbin.FullName /dependents $dll
}

$oldPath = $env:Path
try {
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$Consumer runtime probe failed" }
} finally {
    $env:Path = $oldPath
}

