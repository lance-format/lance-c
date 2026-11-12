# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$prefixPath = (Resolve-Path -LiteralPath $Prefix).Path
$workPath = (Resolve-Path -LiteralPath $WorkDir).Path
$dll = Join-Path $prefixPath 'lib\lance_c.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "missing staged DLL: $dll"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products * -property installationPath
$dumpbin = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') `
    -Directory |
    Sort-Object Name -Descending |
    ForEach-Object {
        @(
            (Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe'),
            (Join-Path $_.FullName 'bin\Hostarm64\arm64\dumpbin.exe')
        )
    } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $dumpbin) {
    throw 'dumpbin not found'
}

$dependents = (& $dumpbin /dependents $dll) -join "`n"
Write-Host $dependents
$forbidden = @('VCRUNTIME', 'MSVCP', 'MSVCR', 'CONCRT', 'VCOMP')
foreach ($name in $forbidden) {
    if ($dependents -match "(?im)^\s*${name}[A-Za-z0-9_.-]*\.dll\s*$") {
        throw "staged DLL still imports $name runtime DLL"
    }
}

Write-Host "==> Staged DLL has no MSVC runtime DLL imports"
