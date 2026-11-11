# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Classify one PE import for the staged Windows package. API-set names such as
# `api-ms-win-core-synch-l1-2-0.dll` and `ext-ms-win-*` are loader contracts,
# not files that must exist under System32. Their availability is established
# by running the installed native consumer on the target host.

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
