# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Deterministic checks for the Windows dependency guard. This does not replace
# the native consumer run: API-set imports are accepted as loader contracts,
# and the installed shared consumer proves their resolution on this host.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'windows-runtime-dependencies.ps1')

$fixture = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
$prefixPath = Join-Path $fixture 'stage'
$system32 = Join-Path $fixture 'Windows\System32'
try {
    New-Item -ItemType Directory -Force -Path (Join-Path $prefixPath 'lib'), $system32 |
        Out-Null
    Set-Content -LiteralPath (Join-Path $system32 'KERNEL32.dll') -Value 'fixture'
    Set-Content -LiteralPath (Join-Path $prefixPath 'lib\app-local.dll') -Value 'fixture'

    $cases = @(
        @{ Dependency = 'KERNEL32.dll'; Expected = 'system32' },
        @{ Dependency = 'app-local.dll'; Expected = 'staged' },
        @{ Dependency = 'api-ms-win-core-synch-l1-2-0.dll'; Expected = 'api-set' },
        @{ Dependency = 'EXT-MS-WIN-NTUSER-Window-l1-1-0.dll'; Expected = 'api-set' }
    )

    foreach ($case in $cases) {
        $actual = Resolve-RuntimeDependency `
            -Dependency $case.Dependency `
            -PrefixPath $prefixPath `
            -System32 $system32
        if ($actual -ne $case.Expected) {
            throw "dependency $($case.Dependency) classified as $actual, expected $($case.Expected)"
        }
        Write-Host "    $($case.Dependency) -> $actual"
    }

    $dumpbinSample = @'
Image has the following dependencies:

    KERNEL32.dll
    api-ms-win-core-synch-l1-2-0.dll

Image has the following delay load dependencies:

    USER32.dll
    VCRUNTIME140.dll
'@
    $parsed = Get-PeDependencies -DependentsText $dumpbinSample
    if ($parsed.Direct.Count -ne 2 -or
        $parsed.Direct -notcontains 'KERNEL32.dll' -or
        $parsed.Direct -notcontains 'api-ms-win-core-synch-l1-2-0.dll') {
        throw "direct dependency parsing failed: $($parsed.Direct -join ', ')"
    }
    if ($parsed.Delay.Count -ne 2 -or
        $parsed.Delay -notcontains 'USER32.dll' -or
        $parsed.Delay -notcontains 'VCRUNTIME140.dll') {
        throw "delay dependency parsing failed: $($parsed.Delay -join ', ')"
    }

    Assert-NoRedistributableRuntimeDependencies `
        -Dependencies $parsed.Direct `
        -Description 'direct dependency fixture'
    $redistributableNames = @(
        'VCRUNTIME140.dll',
        'VCRUNTIME140_1.dll',
        'VCRUNTIME140D.dll',
        'MSVCP140.dll',
        'MSVCP140_ATOMIC_WAIT.dll',
        'MSVCP140_CODECVT_IDS.dll',
        'MSVCP140D.dll',
        'MSVCR120.dll',
        'CONCRT140.dll',
        'VCOMP140.dll'
    )
    foreach ($name in $redistributableNames) {
        $rejected = $false
        try {
            Assert-NoRedistributableRuntimeDependencies `
                -Dependencies @($name) `
                -Description 'redistributable fixture' | Out-Null
        } catch {
            $rejected = $true
            if ($_.Exception.Message -notmatch [regex]::Escape($name)) {
                throw
            }
        }
        if (-not $rejected) {
            throw "redistributable runtime $name was not rejected"
        }
    }

    $vcxprojFixture = Join-Path $fixture 'acceptance.vcxproj'
    Set-Content -LiteralPath $vcxprojFixture -Value @'
<ItemDefinitionGroup>
  <ClCompile>
    <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
  </ClCompile>
</ItemDefinitionGroup>
'@
    $runtimeLibraries = @(
        Get-MsvcRuntimeLibraries -ProjectPath $vcxprojFixture
    )
    if ($runtimeLibraries.Count -ne 1 -or
        $runtimeLibraries[0] -ne 'MultiThreaded') {
        throw "single MSVC runtime library was not preserved as an array"
    }

    $missing = 'missing-runtime-174.dll'
    $rejected = $false
    try {
        Resolve-RuntimeDependency `
            -Dependency $missing `
            -PrefixPath $prefixPath `
            -System32 $system32 | Out-Null
    } catch {
        $rejected = $true
        if ($_.Exception.Message -notmatch [regex]::Escape($missing)) {
            throw
        }
    }
    if (-not $rejected) {
        throw "ordinary missing dependency $missing was not rejected"
    }

    Write-Host "==> Windows dependency guard checks passed"
} finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
