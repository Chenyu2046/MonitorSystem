param(
    [ValidateSet('Legacy', 'Sharded')]
    [string]$Architecture,
    [Parameter(Mandatory = $true)] [string]$RepositoryPath,
    [Parameter(Mandatory = $true)] [string]$EvidencePath,
    [ValidateRange(1, 1000)] [int]$Workers,
    [ValidateRange(1, 3600)] [int]$DurationSeconds = 60,
    [ValidateRange(10, 60000)] [int]$IntervalMs = 1000,
    [ValidateRange(30, 3600)] [int]$DrainTimeoutSeconds = 180,
    [switch]$StaggerStart,
    [Parameter(Mandatory = $true)] [string]$RunId
)

$ErrorActionPreference = 'Stop'
$RepositoryPath = (Resolve-Path -LiteralPath $RepositoryPath).Path
$EvidencePath = [IO.Path]::GetFullPath($EvidencePath)
$ComposePath = Join-Path $RepositoryPath 'docker-compose.benchmark.yml'
$ResultDir = Join-Path $RepositoryPath 'benchmark/results'
$RawCsvPath = Join-Path $ResultDir "$RunId.csv"
$EvidenceCsvPath = Join-Path $EvidencePath 'result.csv'
$ResourcePath = Join-Path $EvidencePath 'resource.csv'
$StatsPath = Join-Path $EvidencePath 'manager-stats.txt'
$SummaryPath = Join-Path $EvidencePath 'summary.txt'
$LoadgenLogPath = Join-Path $EvidencePath 'loadgen.log'
$Project = "manager-ab-$RunId".ToLowerInvariant()

New-Item -ItemType Directory -Force -Path $ResultDir, $EvidencePath | Out-Null

function Invoke-Compose([string[]]$Arguments) {
    & docker compose -p $Project -f $ComposePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed: $($Arguments -join ' ')"
    }
}

function Get-ContainerId([string]$Service) {
    $id = (& docker compose -p $Project -f $ComposePath ps -q $Service | Select-Object -First 1).Trim()
    if (-not $id) { throw "Container ID not found for service $Service" }
    return $id
}

function Get-DbRows([string]$Prefix) {
    $output = & docker compose -p $Project -f $ComposePath exec -T mysql `
        mysql -N -s -ubenchmark -pbenchmark_only_password monitor_db `
        -e "SELECT COUNT(*) FROM server_performance WHERE server_name LIKE '$Prefix%';"
    if ($LASTEXITCODE -ne 0) { throw 'MySQL row-count query failed.' }
    $number = $output | Where-Object { $_ -match '^\s*\d+\s*$' } | Select-Object -Last 1
    if (-not $number) { throw "MySQL row-count output was not numeric: $output" }
    return [int]$number.Trim()
}

function Wait-MySqlReady {
    $deadline = (Get-Date).AddMinutes(3)
    do {
        $result = & docker compose -p $Project -f $ComposePath exec -T mysql `
            mysql -N -s -ubenchmark -pbenchmark_only_password monitor_db -e 'SELECT 1' 2>$null
        if ($LASTEXITCODE -eq 0 -and ($result | Where-Object { $_ -match '^\s*1\s*$' })) {
            return
        }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    throw 'MySQL did not accept a readiness query within three minutes.'
}

function Get-Percentile([object[]]$Rows, [double]$Percentile) {
    $latencies = @($Rows | Where-Object { $_.success -eq '1' } |
        ForEach-Object { [int64]$_.latency_us } | Sort-Object)
    if ($latencies.Count -eq 0) { return 0 }
    $index = [Math]::Ceiling($latencies.Count * $Percentile) - 1
    return $latencies[[Math]::Min($index, $latencies.Count - 1)]
}

function Get-ResourceSummary([object[]]$Rows, [string]$Pattern) {
    $matched = @($Rows | Where-Object { $_.container -match $Pattern })
    if ($matched.Count -eq 0) {
        return @{ cpu_avg = 'N/A'; cpu_peak = 'N/A'; rss_avg_mib = 'N/A'; rss_peak_mib = 'N/A' }
    }
    $cpu = @($matched | ForEach-Object { [double]$_.cpu_percent })
    $rss = @($matched | ForEach-Object {
        $match = [regex]::Match($_.memory_usage, '([0-9.]+)\s*(KiB|MiB|GiB)')
        if (-not $match.Success) { return 0 }
        $value = [double]$match.Groups[1].Value
        switch ($match.Groups[2].Value) {
            'KiB' { $value / 1024 }
            'GiB' { $value * 1024 }
            default { $value }
        }
    })
    return @{
        cpu_avg = [Math]::Round(($cpu | Measure-Object -Average).Average, 2)
        cpu_peak = [Math]::Round(($cpu | Measure-Object -Maximum).Maximum, 2)
        rss_avg_mib = [Math]::Round(($rss | Measure-Object -Average).Average, 2)
        rss_peak_mib = [Math]::Round(($rss | Measure-Object -Maximum).Maximum, 2)
    }
}

$resourceJob = $null
$managerId = $null
$mysqlId = $null
$benchmarkStart = $null
$loadStart = $null
$loadEnd = $null
$allPersistedTime = $null
$persistedRows = 0
$expectedRows = 0
$managerLogs = ''
$failure = $null

try {
    Invoke-Compose @('up', '-d', '--build', 'mysql')
    $mysqlId = Get-ContainerId 'mysql'
    Wait-MySqlReady
    Invoke-Compose @('up', '-d', '--build', 'manager')
    $managerId = Get-ContainerId 'manager'
    $deadline = (Get-Date).AddMinutes(3)
    do {
        $running = (& docker inspect -f '{{.State.Running}}' $managerId).Trim()
        if ($running -eq 'true') { break }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    if ($running -ne 'true') { throw 'Manager did not become running.' }

    $benchmarkStart = Get-Date
    $resourceJob = Start-Job -ArgumentList @($managerId, $mysqlId) -ScriptBlock {
        param($ManagerId, $MysqlId)
        while ($true) {
            $timestamp = (Get-Date).ToUniversalTime().ToString('o')
            $rows = docker stats --no-stream --format '{{.Name}}|{{.CPUPerc}}|{{.MemUsage}}' $ManagerId $MysqlId 2>$null
            foreach ($row in $rows) {
                $parts = $row -split '\|', 3
                if ($parts.Count -eq 3) {
                    "$timestamp,$($parts[0]),$($parts[1].TrimEnd('%')),$($parts[2])"
                }
            }
            Start-Sleep -Seconds 1
        }
    }

    $loadStart = Get-Date
    $loadArgs = @(
        'run', '--rm', 'loadgen',
        '--workers', $Workers,
        '--duration-seconds', $DurationSeconds,
        '--interval-ms', $IntervalMs,
        '--run-id', $RunId,
        '--output', "/results/$RunId.csv"
    )
    if ($StaggerStart) { $loadArgs += '--stagger-start' }
    $loadOutput = @(& docker compose -p $Project -f $ComposePath @loadArgs 2>&1)
    $loadExitCode = $LASTEXITCODE
    $loadEnd = Get-Date
    $loadOutput | Out-File -LiteralPath $LoadgenLogPath -Encoding utf8
    if ($loadExitCode -ne 0) { $failure = "loadgen exit code $loadExitCode" }
    if (-not (Test-Path -LiteralPath $RawCsvPath)) { throw "Missing result CSV: $RawCsvPath" }

    $rows = @(Import-Csv -LiteralPath $RawCsvPath)
    $totalRows = $rows.Count
    $successRows = @($rows | Where-Object { $_.success -eq '1' }).Count
    $failedRows = $totalRows - $successRows
    $expectedRows = $successRows
    $drainDeadline = (Get-Date).AddSeconds($DrainTimeoutSeconds)
    do {
        $persistedRows = Get-DbRows "$RunId-worker-"
        if ($persistedRows -eq $expectedRows) {
            $allPersistedTime = Get-Date
            break
        }
        Start-Sleep -Seconds 1
    } while ((Get-Date) -lt $drainDeadline)
    if ($persistedRows -ne $expectedRows -and -not $failure) {
        $failure = "drain timeout: expected $expectedRows, got $persistedRows"
    }

    if ($resourceJob) {
        Stop-Job -Job $resourceJob -ErrorAction SilentlyContinue
        $resourceRows = @(Receive-Job -Job $resourceJob -ErrorAction SilentlyContinue)
        Remove-Job -Job $resourceJob -Force -ErrorAction SilentlyContinue
        @('timestamp_utc,container,cpu_percent,memory_usage') + $resourceRows |
            Out-File -LiteralPath $ResourcePath -Encoding utf8
        $resourceJob = $null
    }

    Invoke-Compose @('stop', 'manager')
    $managerLogs = (& docker compose -p $Project -f $ComposePath logs manager --no-color | Out-String)
    $managerLogs | Out-File -LiteralPath $StatsPath -Encoding utf8
    if ($Architecture -eq 'Sharded') {
        $statsPattern = 'processing_stats accepted=(\d+) queue_full=(\d+) processed=(\d+) persistence_tasks=(\d+) persistence_rejected=(\d+) queue_delay_samples=(\d+) queue_delay_total_us=(\d+) max_queue_delay_us=(\d+) max_shard_queue_depth=(\d+) max_shard_queue_bytes=(\d+) max_persistence_queue_depth=(\d+) max_persistence_queue_bytes=(\d+)'
        $statsMatch = [regex]::Match($managerLogs, $statsPattern)
        if (-not $statsMatch.Success -and -not $failure) { $failure = 'processing_stats not found' }
    }
} catch {
    $failure = $_.Exception.Message
} finally {
    if ($resourceJob) {
        Stop-Job -Job $resourceJob -ErrorAction SilentlyContinue
        $resourceRows = @(Receive-Job -Job $resourceJob -ErrorAction SilentlyContinue)
        Remove-Job -Job $resourceJob -Force -ErrorAction SilentlyContinue
        @('timestamp_utc,container,cpu_percent,memory_usage') + $resourceRows |
            Out-File -LiteralPath $ResourcePath -Encoding utf8
    }
    & docker compose -p $Project -f $ComposePath down --remove-orphans | Out-Null
}

$resourceData = @()
if (Test-Path -LiteralPath $ResourcePath) {
    $resourceData = @(Import-Csv -LiteralPath $ResourcePath)
}
$managerResource = Get-ResourceSummary $resourceData '-manager-'
$mysqlResource = Get-ResourceSummary $resourceData '-mysql-'

$queueStats = @{
    accepted = 'N/A'; queue_full = 'N/A'; processed = 'N/A'; persistence_tasks = 'N/A'
    persistence_rejected = 'N/A'; queue_delay_samples = 'N/A'; queue_delay_total_us = 'N/A'
    queue_delay_mean_us = 'N/A'; max_queue_delay_us = 'N/A'; max_shard_queue_depth = 'N/A'
    max_shard_queue_bytes = 'N/A'; max_persistence_queue_depth = 'N/A'; max_persistence_queue_bytes = 'N/A'
}
if ($Architecture -eq 'Sharded' -and $statsMatch.Success) {
    $queueStats.accepted = $statsMatch.Groups[1].Value
    $queueStats.queue_full = $statsMatch.Groups[2].Value
    $queueStats.processed = $statsMatch.Groups[3].Value
    $queueStats.persistence_tasks = $statsMatch.Groups[4].Value
    $queueStats.persistence_rejected = $statsMatch.Groups[5].Value
    $queueStats.queue_delay_samples = $statsMatch.Groups[6].Value
    $queueStats.queue_delay_total_us = $statsMatch.Groups[7].Value
    $queueStats.queue_delay_mean_us = if ([int64]$statsMatch.Groups[6].Value -gt 0) {
        [Math]::Round([int64]$statsMatch.Groups[7].Value / [int64]$statsMatch.Groups[6].Value, 2)
    } else { 0 }
    $queueStats.max_queue_delay_us = $statsMatch.Groups[8].Value
    $queueStats.max_shard_queue_depth = $statsMatch.Groups[9].Value
    $queueStats.max_shard_queue_bytes = $statsMatch.Groups[10].Value
    $queueStats.max_persistence_queue_depth = $statsMatch.Groups[11].Value
    $queueStats.max_persistence_queue_bytes = $statsMatch.Groups[12].Value
}

$totalCompletion = if ($allPersistedTime -and $benchmarkStart) {
    [Math]::Round(($allPersistedTime - $benchmarkStart).TotalSeconds, 3)
} else { 'N/A' }
$loadDuration = if ($loadEnd -and $loadStart) {
    [Math]::Round(($loadEnd - $loadStart).TotalSeconds, 3)
} else { 'N/A' }
$drainDuration = if ($allPersistedTime -and $loadEnd) {
    [Math]::Round(($allPersistedTime - $loadEnd).TotalSeconds, 3)
} else { 'N/A' }
$csvHash = (Get-FileHash -LiteralPath $RawCsvPath -Algorithm SHA256).Hash
Copy-Item -LiteralPath $RawCsvPath -Destination $EvidenceCsvPath -Force
$status = if ($failure) { 'FAIL' } else { 'PASS' }
$summary = @(
    "status=$status"
    "architecture=$Architecture"
    "repository=$RepositoryPath"
    "run_id=$RunId"
    "workers=$Workers"
    "duration_seconds=$DurationSeconds"
    "interval_ms=$IntervalMs"
    "arrival_model=$(if ($StaggerStart) { 'stagger_start' } else { 'burst' })"
    "total_samples=$totalRows"
    "successful_rpc=$successRows"
    "failed_rpc=$failedRows"
    "persisted_rows=$persistedRows"
    "load_duration_sec=$loadDuration"
    "drain_duration_sec=$drainDuration"
    "total_completion_sec=$totalCompletion"
    "accepted_reports_per_sec=$(if ($totalCompletion -ne 'N/A') { [Math]::Round($successRows / $totalCompletion, 3) } else { 'N/A' })"
    "processed_reports_per_sec=$(if ($totalCompletion -ne 'N/A' -and $queueStats.processed -ne 'N/A') { [Math]::Round([int64]$queueStats.processed / $totalCompletion, 3) } else { 'N/A' })"
    "persisted_reports_per_sec=$(if ($totalCompletion -ne 'N/A') { [Math]::Round($persistedRows / $totalCompletion, 3) } else { 'N/A' })"
    "rpc_p50_us=$(Get-Percentile $rows 0.50)"
    "rpc_p95_us=$(Get-Percentile $rows 0.95)"
    "rpc_p99_us=$(Get-Percentile $rows 0.99)"
    "accepted=$($queueStats.accepted)"
    "queue_full=$($queueStats.queue_full)"
    "processed=$($queueStats.processed)"
    "persistence_tasks=$($queueStats.persistence_tasks)"
    "persistence_rejected=$($queueStats.persistence_rejected)"
    "queue_delay_samples=$($queueStats.queue_delay_samples)"
    "queue_delay_total_us=$($queueStats.queue_delay_total_us)"
    "queue_delay_mean_us=$($queueStats.queue_delay_mean_us)"
    "max_queue_delay_us=$($queueStats.max_queue_delay_us)"
    "max_shard_queue_depth=$($queueStats.max_shard_queue_depth)"
    "max_shard_queue_bytes=$($queueStats.max_shard_queue_bytes)"
    "max_persistence_queue_depth=$($queueStats.max_persistence_queue_depth)"
    "max_persistence_queue_bytes=$($queueStats.max_persistence_queue_bytes)"
    "manager_cpu_avg_percent=$($managerResource.cpu_avg)"
    "manager_cpu_peak_percent=$($managerResource.cpu_peak)"
    "manager_rss_avg_mib=$($managerResource.rss_avg_mib)"
    "manager_rss_peak_mib=$($managerResource.rss_peak_mib)"
    "mysql_cpu_avg_percent=$($mysqlResource.cpu_avg)"
    "mysql_cpu_peak_percent=$($mysqlResource.cpu_peak)"
    "mysql_rss_avg_mib=$($mysqlResource.rss_avg_mib)"
    "mysql_rss_peak_mib=$($mysqlResource.rss_peak_mib)"
    "result_csv_sha256=$csvHash"
    "failure=$failure"
)
$summary | Out-File -LiteralPath $SummaryPath -Encoding utf8
Get-Content -LiteralPath $SummaryPath
if ($failure) { exit 2 }
