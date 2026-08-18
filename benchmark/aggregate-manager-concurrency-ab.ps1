param(
    [string]$EvidenceRoot = (Join-Path $PSScriptRoot 'evidence/manager-concurrency-ab-20260818')
)

$ErrorActionPreference = 'Stop'

function Read-Summary([string]$Path) {
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([^=]+)=(.*)$') { $values[$matches[1]] = $matches[2] }
    }
    $hostMatch = [regex]::Match($Path, '[\\/]([0-9]+)host[\\/]')
    [pscustomobject]@{
        path = (Resolve-Path -LiteralPath $Path).Path
        architecture = $values.architecture
        workload = if ($Path -match '[\\/]stagger[\\/]') { 'stagger' } else { 'burst' }
        hosts = [int]$hostMatch.Groups[1].Value
        status = $values.status
        total_samples = $values.total_samples
        successful_rpc = $values.successful_rpc
        persisted_rows = $values.persisted_rows
        total_completion_sec = $values.total_completion_sec
        drain_duration_sec = $values.drain_duration_sec
        persisted_reports_per_sec = $values.persisted_reports_per_sec
        rpc_p50_us = $values.rpc_p50_us
        rpc_p95_us = $values.rpc_p95_us
        rpc_p99_us = $values.rpc_p99_us
        queue_delay_mean_us = $values.queue_delay_mean_us
        max_queue_delay_us = $values.max_queue_delay_us
        max_shard_queue_depth = $values.max_shard_queue_depth
        max_persistence_queue_depth = $values.max_persistence_queue_depth
        queue_full = $values.queue_full
        persistence_rejected = $values.persistence_rejected
        manager_cpu_avg_percent = $values.manager_cpu_avg_percent
        manager_cpu_peak_percent = $values.manager_cpu_peak_percent
        manager_rss_avg_mib = $values.manager_rss_avg_mib
        manager_rss_peak_mib = $values.manager_rss_peak_mib
        mysql_cpu_avg_percent = $values.mysql_cpu_avg_percent
        mysql_cpu_peak_percent = $values.mysql_cpu_peak_percent
        mysql_rss_peak_mib = $values.mysql_rss_peak_mib
        failure = $values.failure
    }
}

function Median([object[]]$Values) {
    $numbers = @($Values | Where-Object { $_ -and $_ -ne 'N/A' } | ForEach-Object { [double]$_ } | Sort-Object)
    if ($numbers.Count -eq 0) { return 'N/A' }
    $middle = [Math]::Floor($numbers.Count / 2)
    if ($numbers.Count % 2 -eq 1) { return $numbers[$middle] }
    return [Math]::Round(($numbers[$middle - 1] + $numbers[$middle]) / 2, 3)
}

function Aggregate([object[]]$Records, [int]$Hosts) {
    $legacy = @($Records | Where-Object { $_.architecture -eq 'Legacy' -and $_.hosts -eq $Hosts })
    $sharded = @($Records | Where-Object { $_.architecture -eq 'Sharded' -and $_.hosts -eq $Hosts })
    $legacyPass = @($legacy | Where-Object status -eq 'PASS')
    $shardedPass = @($sharded | Where-Object status -eq 'PASS')
    $legacyTime = Median ($legacyPass | ForEach-Object total_completion_sec)
    $shardedTime = Median ($shardedPass | ForEach-Object total_completion_sec)
    $improvement = if ($legacyTime -ne 'N/A' -and $shardedTime -ne 'N/A') {
        [Math]::Round((1 - ([double]$shardedTime / [double]$legacyTime)) * 100, 2)
    } else { 'N/A' }
    $throughput = if ($legacyPass.Count -gt 0 -and $shardedPass.Count -gt 0) {
        [Math]::Round(([double](Median ($shardedPass | ForEach-Object persisted_reports_per_sec)) /
            [double](Median ($legacyPass | ForEach-Object persisted_reports_per_sec))), 3)
    } else { 'N/A' }
    [pscustomobject]@{
        hosts = $Hosts
        legacy_pass_runs = $legacyPass.Count
        legacy_fail_runs = $legacy.Count - $legacyPass.Count
        sharded_pass_runs = $shardedPass.Count
        sharded_fail_runs = $sharded.Count - $shardedPass.Count
        legacy_total_completion_sec_median = $legacyTime
        sharded_total_completion_sec_median = $shardedTime
        legacy_persisted_reports_per_sec_median = Median ($legacyPass | ForEach-Object persisted_reports_per_sec)
        sharded_persisted_reports_per_sec_median = Median ($shardedPass | ForEach-Object persisted_reports_per_sec)
        total_time_reduction_percent = $improvement
        sharded_over_legacy_persisted_throughput = $throughput
        legacy_p95_us_median = Median ($legacyPass | ForEach-Object rpc_p95_us)
        legacy_p99_us_median = Median ($legacyPass | ForEach-Object rpc_p99_us)
        sharded_accepted_p95_us_median = Median ($shardedPass | ForEach-Object rpc_p95_us)
        sharded_accepted_p99_us_median = Median ($shardedPass | ForEach-Object rpc_p99_us)
        sharded_queue_mean_us_median = Median ($shardedPass | ForEach-Object queue_delay_mean_us)
        sharded_queue_max_us_median = Median ($shardedPass | ForEach-Object max_queue_delay_us)
        sharded_peak_shard_depth_median = Median ($shardedPass | ForEach-Object max_shard_queue_depth)
        sharded_peak_persistence_depth_median = Median ($shardedPass | ForEach-Object max_persistence_queue_depth)
        legacy_manager_cpu_avg_percent_median = Median ($legacyPass | ForEach-Object manager_cpu_avg_percent)
        sharded_manager_cpu_avg_percent_median = Median ($shardedPass | ForEach-Object manager_cpu_avg_percent)
        legacy_manager_rss_peak_mib_median = Median ($legacyPass | ForEach-Object manager_rss_peak_mib)
        sharded_manager_rss_peak_mib_median = Median ($shardedPass | ForEach-Object manager_rss_peak_mib)
    }
}

$records = @(Get-ChildItem -LiteralPath $EvidenceRoot -Recurse -Filter summary.txt | ForEach-Object { Read-Summary $_.FullName })
foreach ($workload in @('stagger', 'burst')) {
    $workloadRecords = @($records | Where-Object {
        $_.workload -eq $workload -and $_.path -notmatch '[\\/]long600s[\\/]'
    })
    $rows = @(1, 10, 25, 50, 75, 100, 150, 200 | ForEach-Object { Aggregate $workloadRecords $_ })
    $rows | Export-Csv -LiteralPath (Join-Path $EvidenceRoot "$workload-ab-summary.csv") -NoTypeInformation -Encoding utf8
}

$longRows = @($records | Where-Object { $_.path -match '[\\/]long600s[\\/]' }) |
    Select-Object workload, architecture, hosts, status, total_samples, successful_rpc, persisted_rows,
        total_completion_sec, drain_duration_sec, persisted_reports_per_sec, rpc_p50_us, rpc_p95_us, rpc_p99_us,
        queue_delay_mean_us, max_queue_delay_us, max_shard_queue_depth, max_persistence_queue_depth,
        manager_cpu_avg_percent, manager_cpu_peak_percent, manager_rss_avg_mib, manager_rss_peak_mib,
        mysql_cpu_avg_percent, mysql_cpu_peak_percent, mysql_rss_peak_mib, path
$longRows | Export-Csv -LiteralPath (Join-Path $EvidenceRoot 'long-stability-summary.csv') -NoTypeInformation -Encoding utf8

$records | Where-Object status -eq 'FAIL' |
    Select-Object workload, architecture, hosts, path, total_samples, successful_rpc, persisted_rows, queue_full, persistence_rejected, failure |
    Export-Csv -LiteralPath (Join-Path $EvidenceRoot 'failures.csv') -NoTypeInformation -Encoding utf8

$hashLines = @()
Get-ChildItem -LiteralPath $EvidenceRoot -Recurse -File |
    Where-Object { $_.Name -in @('result.csv', 'resource.csv', 'manager-stats.txt', 'summary.txt') } |
    Sort-Object FullName |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        $relative = $_.FullName.Substring((Resolve-Path $EvidenceRoot).Path.Length).TrimStart('\', '/')
        $hashLines += "$hash  $relative"
    }
$hashLines | Out-File -LiteralPath (Join-Path $EvidenceRoot 'sha256-manifest.txt') -Encoding utf8

$records | Group-Object workload, architecture, status | Select-Object Name, Count | Format-Table -AutoSize
