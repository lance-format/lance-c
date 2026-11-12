# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-DllExports {
    param([string]$Path)

    $text = & dumpbin.exe /nologo /exports $Path 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $Path`n$text"
    }
    return @(
        [regex]::Matches(
            $text, '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$') |
            ForEach-Object { $_.Groups[1].Value } |
            Sort-Object -Unique
    )
}

function Get-ImportExports {
    param([string]$Path)

    $text = & dumpbin.exe /nologo /exports $Path 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $Path`n$text"
    }
    $inTable = $false
    $foundTable = $false
    $exports = foreach ($line in ($text -split '\r?\n')) {
        if ($line -match '^\s*ordinal\s+name\s*$') {
            $inTable = $true
            $foundTable = $true
            continue
        }
        if ($line -match '^  Summary\s*$') {
            $inTable = $false
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
        throw "$Description mismatch: actual=[$($Actual -join ', ')] missing=[$($missing -join ', ')] extra=[$($extra -join ', ')]"
    }
}

$manifest = (Resolve-Path -LiteralPath $ManifestPath).Path
$targetRoot = Join-Path (Split-Path -Parent $manifest) 'target\release'
$dll = Join-Path $targetRoot 'lance_c.dll'
$importLibrary = Join-Path $targetRoot 'lance_c.dll.lib'
foreach ($artifact in @($dll, $importLibrary)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Cargo did not produce $artifact"
    }
}

Assert-Exports 'Cargo DLL' (Get-DllExports $dll) @('lance_api')
Assert-Exports 'Cargo import library' `
    (Get-ImportExports $importLibrary) @('lance_api')

$outDir = Get-ChildItem -LiteralPath `
    (Join-Path (Split-Path -Parent $manifest) 'target\release\build') `
    -Directory -Filter 'lance-c-probe-*' |
    ForEach-Object { Join-Path $_.FullName 'out' } |
    Where-Object {
        Test-Path -LiteralPath (Join-Path $_ 'lance_c.def') -PathType Leaf
    } |
    Select-Object -First 1
if (-not $outDir) {
    throw 'Cargo probe build-script output directory was not found'
}
$definition = Join-Path $outDir 'lance_c.def'
$dependency = Join-Path $outDir 'dependency.lib'
$object = Join-Path $WorkDir 'public.obj'
$publicSource = Join-Path $PSScriptRoot '..\probe\pr216\public.c'
& cl.exe /nologo /c "/Fo:$object" $publicSource
if ($LASTEXITCODE -ne 0) {
    throw 'failed to compile the response-file probe object'
}

$response = Join-Path $WorkDir 'linker-arguments'
$responseDll = Join-Path $WorkDir 'response.dll'
$responseImport = Join-Path $WorkDir 'response.lib'
$responseLines = @(
    '/nologo',
    '/DLL',
    '/NOENTRY',
    '/NODEFAULTLIB',
    '/MACHINE:X64',
    "/OUT:$responseDll",
    "/IMPLIB:$responseImport",
    "/DEF:$definition",
    $object,
    $dependency
) | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
[System.IO.File]::WriteAllText(
    $response, (($responseLines -join "`r`n") + "`r`n"), [System.Text.Encoding]::Unicode)

& (Join-Path $PSScriptRoot 'limit-windows-exports.cmd') "@$response"
if ($LASTEXITCODE -ne 0) {
    throw "response-file link failed with exit code $LASTEXITCODE"
}
Assert-Exports 'response-file DLL' (Get-DllExports $responseDll) @('lance_api')
Assert-Exports 'response-file import library' `
    (Get-ImportExports $responseImport) @('lance_api')

Write-Host 'Cargo linker probe: PASS'
