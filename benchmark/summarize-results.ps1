param(
    [Parameter(Mandatory = $true)] [string]$CsvPath
)

$Rows = Import-Csv -LiteralPath $CsvPath
if (-not $Rows) { throw "No samples in $CsvPath" }
$Success = @($Rows | Where-Object { $_.success -eq '1' })
$Latencies = @($Success | ForEach-Object { [int64]$_.latency_us } | Sort-Object)
function Get-Percentile([double]$Percentile) {
    if ($Latencies.Count -eq 0) { return 0 }
    $Index = [Math]::Ceiling($Latencies.Count * $Percentile) - 1
    return $Latencies[[Math]::Min($Index, $Latencies.Count - 1)]
}

[pscustomobject]@{
    csv = Split-Path -Leaf $CsvPath
    latency_stage = 'rpc_accepted'
    samples = $Rows.Count
    accepted = $Success.Count
    success = $Success.Count
    success_rate_percent = [Math]::Round(100 * $Success.Count / $Rows.Count, 2)
    accepted_p50_us = Get-Percentile 0.50
    accepted_p95_us = Get-Percentile 0.95
    accepted_p99_us = Get-Percentile 0.99
} | Format-List
