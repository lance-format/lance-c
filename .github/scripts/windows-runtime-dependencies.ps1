# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Classify one PE import for the staged Windows package. API-set names such as
# `api-ms-win-core-synch-l1-2-0.dll` and `ext-ms-win-*` are loader contracts,
# not files that must exist under System32. Their availability is established
# by running the installed native consumer on the target host.

function Get-PeDependencies {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$DependentsText
    )

    $direct = [System.Collections.Generic.List[string]]::new()
    $delay = [System.Collections.Generic.List[string]]::new()
    $section = ''
    foreach ($line in ($DependentsText -split "`r?`n")) {
        if ($line -match 'Image has the following dependencies:') {
            $section = 'direct'
            continue
        }
        if ($line -match 'Image has the following delay load dependencies:') {
            $section = 'delay'
            continue
        }
        if ($section -and $line -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
            $dependency = $Matches[1]
            if ($section -eq 'direct') {
                $direct.Add($dependency)
            } else {
                $delay.Add($dependency)
            }
        }
    }

    return [pscustomobject]@{
        Direct = @($direct | Sort-Object -Unique)
        Delay = @($delay | Sort-Object -Unique)
    }
}

function Assert-NoRedistributableRuntimeDependencies {
    [CmdletBinding()]
    param(
        [string[]]$Dependencies,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $pattern =
        '^(?i)(VCRUNTIME\d*|MSVCP\d*|MSVCR\d*|CONCRT\d*|VCOMP\d*)\.dll$'
    $unexpected = @($Dependencies | Where-Object { $_ -match $pattern })
    if ($unexpected.Count -ne 0) {
        throw ("$Description has redistributable VC Runtime dependencies: " +
            "$($unexpected -join ', ')")
    }
}

function Resolve-RuntimeDependency {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Dependency,
        [Parameter(Mandatory = $true)][string]$PrefixPath,
        [Parameter(Mandatory = $true)][string]$System32
    )

    if ($Dependency -match '(?i)^(api|ext)-') {
        return 'api-set'
    }

    $systemCandidate = Join-Path $System32 $Dependency
    if (Test-Path -LiteralPath $systemCandidate -PathType Leaf) {
        return 'system32'
    }

    $appLocalCandidate = Join-Path $PrefixPath "lib\$Dependency"
    if (Test-Path -LiteralPath $appLocalCandidate -PathType Leaf) {
        return 'staged'
    }

    throw "runtime dependency $Dependency is neither in System32 nor staged beside the DLL"
}
