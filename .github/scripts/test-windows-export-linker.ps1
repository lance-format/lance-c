# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'ARM64')]
    [string]$Architecture,
    [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )

    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Tool failed with exit code $LASTEXITCODE"
    }
}

function Get-DllExports {
    param([string]$Path)

    $text = & dumpbin.exe /nologo /exports $Path 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $Path`n$text"
    }
    return @(
        [regex]::Matches(
            $text, '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)(?:\s+=.*)?\s*$') |
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

$msvcComponent = if ($Architecture -eq 'ARM64') {
    'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
} else {
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe not found at $vswhere"
}
$visualStudio = & $vswhere -latest -products * `
    -requires $msvcComponent -property installationPath |
    Select-Object -First 1
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($visualStudio)) {
    throw "no Visual Studio installation with MSVC component $msvcComponent was found"
}
$devShell = Join-Path $visualStudio 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
$devArch = $Architecture.ToLowerInvariant()
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation `
    -DevCmdArguments "-arch=$devArch -host_arch=$devArch" | Out-Null

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
$workPath = (Resolve-Path -LiteralPath $WorkDir).Path
$fixture = Join-Path $PSScriptRoot '..\..\tests\windows\export_linker'
Copy-Item -LiteralPath (Join-Path $fixture 'public.c') -Destination $workPath
Copy-Item -LiteralPath (Join-Path $fixture 'dependency.c') -Destination $workPath
Push-Location $workPath
try {
    Invoke-Tool cl.exe /nologo /c public.c
    Invoke-Tool cl.exe /nologo /c dependency.c
    Invoke-Tool lib.exe /nologo /out:dependency.lib dependency.obj

    $directives = (& dumpbin.exe /nologo /directives dependency.obj 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $directives -notmatch '/EXPORT:aws_lc_dependency') {
        throw "fixture does not carry the native export directive:`n$directives"
    }

    $machine = if ($Architecture -eq 'ARM64') { 'ARM64' } else { 'X64' }
    Set-Content -LiteralPath base.def -Value @(
        'EXPORTS',
        '  lance_api',
        '  crc_fast_dependency'
    ) -Encoding ascii
    Set-Content -LiteralPath public.def -Value @(
        'LIBRARY lance_c',
        '; lance-c exact public exports',
        'EXPORTS',
        '  lance_api'
    ) -Encoding ascii

    # This is the counterexample: public.obj reaches the exporting member by
    # an ordinary reference, and the last narrow DEF does not suppress it.
    Invoke-Tool link.exe /nologo /DLL /NOENTRY /NODEFAULTLIB `
        "/MACHINE:$machine" /OUT:current.dll /IMPLIB:current.lib `
        /DEF:base.def /DEF:public.def public.obj dependency.lib
    Assert-Exports 'current DLL' (Get-DllExports current.dll) @(
        'aws_lc_dependency', 'lance_api'
    )
    Assert-Exports 'current import library' (Get-ImportExports current.lib) @(
        'aws_lc_dependency', 'lance_api'
    )

    Invoke-Tool lib.exe /nologo "/MACHINE:$machine" /DEF:public.def `
        /OUT:lance_c.dll.lib
    if (-not (Test-Path -LiteralPath lance_c.dll.exp -PathType Leaf)) {
        throw 'LIB did not create the matching export file'
    }

    $response = Join-Path $workPath 'linker-arguments'
    $fixedDll = Join-Path $workPath 'fixed output.dll'
    $fixedImport = Join-Path $workPath 'fixed output.lib'
    $responseLines = @(
        '/NOLOGO',
        '/DLL',
        '/NOENTRY',
        '/NODEFAULTLIB',
        "/MACHINE:$machine",
        "/OUT:$fixedDll",
        "/IMPLIB:$fixedImport",
        "/DEF:$(Join-Path $workPath 'public.def')",
        (Join-Path $workPath 'public.obj'),
        (Join-Path $workPath 'dependency.lib')
    ) | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
    [System.IO.File]::WriteAllText(
        $response,
        (($responseLines -join "`r`n") + "`r`n"),
        [System.Text.Encoding]::Unicode)

    & (Join-Path $PSScriptRoot 'limit-windows-exports.cmd') "@$response"
    if ($LASTEXITCODE -ne 0) {
        throw "export linker failed with exit code $LASTEXITCODE"
    }
    Assert-Exports 'fixed DLL' (Get-DllExports $fixedDll) @('lance_api')
    Assert-Exports 'fixed import library' (Get-ImportExports $fixedImport) @(
        'lance_api'
    )
} finally {
    Pop-Location
}

Write-Host "Windows export linker ($Architecture): PASS"
