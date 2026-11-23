# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

# Compare a staged DLL export list with a published release baseline. Removing
# an export that the published release provided fails the check; exports added
# beyond the baseline are allowed and reported.
function Assert-WindowsExportBaseline {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$BaselineExports,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$ActualExports,
        [Parameter(Mandatory = $true)][string]$BaselineDescription
    )

    $published = @(
        $BaselineExports |
            ForEach-Object { $_.Trim() } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    if ($published.Count -eq 0) {
        throw "published export baseline is empty: $BaselineDescription"
    }
    $invalid = @($published | Where-Object { $_ -cnotmatch '^lance_[A-Za-z0-9_]+$' })
    if ($invalid.Count -ne 0) {
        throw ("published export baseline has invalid entries: " +
            "$BaselineDescription`n$($invalid -join "`n")")
    }

    $removed = @(
        $published | Where-Object { $ActualExports -cnotcontains $_ } |
            Sort-Object -Unique
    )
    if ($removed.Count -ne 0) {
        throw ("staged DLL no longer exports symbols published in " +
            "$BaselineDescription`n$($removed -join "`n")")
    }
    Write-Host ("==> Verified all $($published.Count) published exports from " +
        "$BaselineDescription are present")

    $added = @(
        $ActualExports | Where-Object { $published -cnotcontains $_ } |
            Sort-Object -Unique
    )
    if ($added.Count -ne 0) {
        Write-Host ("==> Exports added beyond $BaselineDescription are allowed " +
            "($($added.Count)):")
        $added | ForEach-Object { Write-Host "    $_" }
    }
}
