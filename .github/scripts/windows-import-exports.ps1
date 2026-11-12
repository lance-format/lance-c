# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

# Parse dumpbin's import-library export table without filtering symbol names.
# Dependency exports must reach the exact-set check; headings and section sizes
# must not be mistaken for names.
function Get-WindowsImportLibraryExports {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    $inTable = $false
    $foundTable = $false
    foreach ($line in ($Text -split '\r?\n')) {
        if ($line -match '^\s*ordinal\s+name\s*$') {
            $inTable = $true
            $foundTable = $true
            continue
        }
        if ($line -match '^  Summary\s*$') {
            $inTable = $false
        }
        if (-not $inTable -or [string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -match '^\s*(?:\d+\s+)?(\S+)\s*$') {
            $Matches[1]
        } else {
            throw "Unrecognized import-library export row: $line"
        }
    }
    if (-not $foundTable) {
        throw 'Import-library dumpbin output has no export table'
    }
}
