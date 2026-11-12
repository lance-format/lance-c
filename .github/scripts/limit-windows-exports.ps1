# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$LinkerArguments
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ExportMarker = '; lance-c exact public exports'

function Write-ExportLog {
    param([string]$Message)

    if ($env:LANCE_C_WINDOWS_EXPORT_LOG) {
        Add-Content -LiteralPath $env:LANCE_C_WINDOWS_EXPORT_LOG -Value $Message
    }
}

function Expand-ResponseFileArguments {
    param([string[]]$Arguments)

    $expanded = [System.Collections.Generic.List[string]]::new()
    foreach ($argument in $Arguments) {
        if (-not $argument.StartsWith('@')) {
            $expanded.Add($argument)
            continue
        }

        $responsePath = $argument.Substring(1)
        if (-not (Test-Path -LiteralPath $responsePath -PathType Leaf)) {
            throw "linker response file does not exist: $responsePath"
        }
        $responseText = [System.IO.File]::ReadAllText($responsePath)
        foreach ($line in ($responseText -split '\r?\n')) {
            if ([string]::IsNullOrWhiteSpace($line)) {
                continue
            }
            if ($line.Length -lt 2 -or -not $line.StartsWith('"') -or
                -not $line.EndsWith('"')) {
                throw "unsupported linker response-file row in ${responsePath}: $line"
            }
            $value = $line.Substring(1, $line.Length - 2).Replace('\"', '"')
            $expanded.Add($value)
        }
    }
    return $expanded.ToArray()
}

function Get-Machine {
    param([string[]]$Arguments)

    foreach ($argument in $Arguments) {
        if ($argument -match '^[/-]MACHINE:(.+)$') {
            return $Matches[1].ToUpperInvariant()
        }
    }
    if ($env:LANCE_C_WINDOWS_MACHINE) {
        return $env:LANCE_C_WINDOWS_MACHINE.ToUpperInvariant()
    }
    switch ($env:PROCESSOR_ARCHITECTURE) {
        'AMD64' { return 'X64' }
        'ARM64' { return 'ARM64' }
        default {
            throw "cannot determine MSVC machine from PROCESSOR_ARCHITECTURE=$env:PROCESSOR_ARCHITECTURE"
        }
    }
}

function Find-MsvcLinker {
    param([string]$Machine)

    if ($env:LANCE_C_WINDOWS_LINK) {
        if (Test-Path -LiteralPath $env:LANCE_C_WINDOWS_LINK -PathType Leaf) {
            return (Resolve-Path -LiteralPath $env:LANCE_C_WINDOWS_LINK).Path
        }
        throw "LANCE_C_WINDOWS_LINK does not exist: $env:LANCE_C_WINDOWS_LINK"
    }

    $component = if ($Machine -eq 'ARM64') {
        'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
    } else {
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $visualStudio = & $vswhere -latest -products * `
            -requires $component -property installationPath
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($visualStudio)) {
            $relativePaths = if ($Machine -eq 'ARM64') {
                @('bin\Hostarm64\arm64\link.exe', 'bin\Hostx64\arm64\link.exe')
            } else {
                @('bin\Hostx64\x64\link.exe', 'bin\Hostx86\x64\link.exe')
            }
            $candidates = Get-ChildItem -LiteralPath `
                (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory |
                Sort-Object Name -Descending |
                ForEach-Object {
                    foreach ($relativePath in $relativePaths) {
                        Join-Path $_.FullName $relativePath
                    }
                }
            $found = $candidates |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
                Select-Object -First 1
            if ($found) {
                return $found
            }
        }
    }

    $command = Get-Command link.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) {
        return $command.Source
    }
    throw "MSVC link.exe not found for $Machine"
}

function Get-MarkedDefinition {
    param([string[]]$Arguments)

    for ($index = $Arguments.Count - 1; $index -ge 0; $index--) {
        if ($Arguments[$index] -notmatch '^[/-]DEF:(.+)$') {
            continue
        }
        $definition = $Matches[1]
        if (-not (Test-Path -LiteralPath $definition -PathType Leaf)) {
            continue
        }
        if ((Get-Content -LiteralPath $definition -Raw).Contains($ExportMarker)) {
            return (Resolve-Path -LiteralPath $definition).Path
        }
    }
    return $null
}

function Get-ImportLibrary {
    param([string[]]$Arguments)

    foreach ($argument in $Arguments) {
        if ($argument -match '^[/-]IMPLIB:(.+)$') {
            return $Matches[1]
        }
    }
    throw 'marked lance-c DEF link has no /IMPLIB argument'
}

$originalArguments = @($LinkerArguments)
Write-ExportLog "original arguments: $($originalArguments -join ' | ')"
$expandedArguments = Expand-ResponseFileArguments $originalArguments
Write-ExportLog "expanded arguments: $($expandedArguments -join ' | ')"
$machine = Get-Machine $expandedArguments
$realLinker = Find-MsvcLinker $machine
$definition = Get-MarkedDefinition $expandedArguments
Write-ExportLog "machine=$machine linker=$realLinker definition=$definition"

if (-not $definition) {
    Write-ExportLog 'forwarding without a marked lance-c DEF'
    & $realLinker @originalArguments
    exit $LASTEXITCODE
}

if (-not ($expandedArguments | Where-Object { $_ -ieq '/DLL' })) {
    throw "marked lance-c export DEF was used for a non-DLL link: $definition"
}

$definitionDirectory = Split-Path -Parent $definition
$generatedImportLibrary = Join-Path $definitionDirectory 'lance_c.dll.lib'
$generatedExportFile = Join-Path $definitionDirectory 'lance_c.dll.exp'
foreach ($generated in @($generatedImportLibrary, $generatedExportFile)) {
    if (-not (Test-Path -LiteralPath $generated -PathType Leaf)) {
        throw "build script did not generate MSVC export artifact: $generated"
    }
}

$importLibrary = Get-ImportLibrary $expandedArguments
Write-ExportLog "import library=$importLibrary export file=$generatedExportFile"
$importDirectory = Split-Path -Parent $importLibrary
if ($importDirectory) {
    New-Item -ItemType Directory -Path $importDirectory -Force | Out-Null
}
$resolvedImportLibrary = [System.IO.Path]::GetFullPath($importLibrary)
$resolvedGeneratedLibrary = [System.IO.Path]::GetFullPath($generatedImportLibrary)
if ($resolvedImportLibrary -ne $resolvedGeneratedLibrary) {
    Copy-Item -LiteralPath $generatedImportLibrary `
        -Destination $resolvedImportLibrary -Force
}

$linkArguments = $originalArguments + $generatedExportFile
Write-ExportLog "final arguments: $($linkArguments -join ' | ')"
& $realLinker @linkArguments
exit $LASTEXITCODE
