# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-DllExports {
    param([string]$Path)

    $text = Get-Content -LiteralPath $Path -Raw
    return @(
        [regex]::Matches(
            $text, '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$') |
            ForEach-Object { $_.Groups[1].Value } |
            Sort-Object -Unique
    )
}

function Get-ImportExports {
    param([string]$Path)

    $inTable = $false
    $foundTable = $false
    $exports = foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -match '^\s*ordinal\s+name\s*$') {
            $inTable = $true
            $foundTable = $true
            continue
        }
        if ($line -match '^  Summary\s*$') {
            $inTable = $false
            continue
        }
        if ($inTable -and $line -match '^\s*(?:\d+\s+)?(\S+)\s*$') {
            $Matches[1]
        }
    }
    if (-not $foundTable) {
        throw "no import export table in $Path"
    }
    return @($exports | Sort-Object -Unique)
}

function Assert-Exports {
    param(
        [string]$Description,
        [string[]]$Actual,
        [string[]]$Expected
    )

    $missing = @($Expected | Where-Object { $Actual -notcontains $_ })
    $extra = @($Actual | Where-Object { $Expected -notcontains $_ })
    if ($missing.Count -ne 0 -or $extra.Count -ne 0) {
        throw "$Description mismatch: missing=[$($missing -join ', ')] extra=[$($extra -join ', ')]"
    }
}

$workPath = (Resolve-Path -LiteralPath $WorkDir).Path
$linkerVersion = Get-Content -LiteralPath (Join-Path $workPath 'link-version.txt') -Raw
if ($linkerVersion -notmatch 'Microsoft \(R\) Incremental Linker Version 14\.') {
    throw "probe did not use MSVC link.exe`n$linkerVersion"
}

$currentDll = Get-DllExports (Join-Path $workPath 'current.dll.exports.txt')
Assert-Exports 'current DLL' $currentDll @('aws_lc_dependency', 'lance_api')
$currentLib = Get-ImportExports (Join-Path $workPath 'current.lib.exports.txt')
Assert-Exports 'current import library' $currentLib @('aws_lc_dependency', 'lance_api')

$fixedWithDef = Get-DllExports (Join-Path $workPath 'fixed-with-def.dll.exports.txt')
Assert-Exports 'export file plus DEF DLL' $fixedWithDef @('lance_api')
$fixedWithDefLib = Get-ImportExports (Join-Path $workPath 'fixed-with-def.lib.exports.txt')
Assert-Exports 'export file plus DEF import library' $fixedWithDefLib @('lance_api')

$fixedWithoutDef = Get-DllExports (Join-Path $workPath 'fixed-without-def.dll.exports.txt')
Assert-Exports 'export file DLL' $fixedWithoutDef @('lance_api')
$fixedWithoutDefLib = Get-ImportExports (Join-Path $workPath 'fixed-without-def.lib.exports.txt')
Assert-Exports 'export file import library' $fixedWithoutDefLib @('lance_api')

$expOnly = Get-DllExports (Join-Path $workPath 'exp-only.dll.exports.txt')
Assert-Exports 'export-file-only DLL' $expOnly @('lance_api')
if (Test-Path -LiteralPath (Join-Path $workPath 'exp-only.lib')) {
    throw 'LINK created an import library for an export-file link'
}

Write-Host 'MSVC export probe: PASS'
Write-Host '  current DEF: dependency export retained'
Write-Host '  pre-generated EXP: exact export set'
Write-Host '  export file only: matching import library not created'
