param(
    [ValidateRange(1, 1000)] [int]$Workers = 10,
    [ValidateRange(1, 3600)] [int]$DurationSeconds = 30,
    [ValidateRange(10, 60000)] [int]$IntervalMs = 1000,
    [ValidateRange(1, 600)] [int]$DrainTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$ResultDir = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null
$RunId = Get-Date -Format 'yyyyMMdd-HHmmss'
$CsvName = "push-$RunId.csv"
$CsvPath = Join-Path $ResultDir $CsvName

Push-Location $Root
try {
    docker compose -f docker-compose.benchmark.yml up -d --build mysql manager
    $deadline = (Get-Date).AddMinutes(2)
    do {
        $managerId = docker compose -f docker-compose.benchmark.yml ps -q manager
        $managerRunning = $managerId -and ((docker inspect -f '{{.State.Running}}' $managerId) -eq 'true')
        if ($managerRunning) { break }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    if (-not $managerRunning) {
        docker compose -f docker-compose.benchmark.yml logs manager
        throw 'Manager did not start within two minutes.'
    }

    docker compose -f docker-compose.benchmark.yml run --rm loadgen `
        --workers $Workers --duration-seconds $DurationSeconds --interval-ms $IntervalMs `
        --run-id "benchmark-$RunId" `
        --output "/results/$CsvName"

    & (Join-Path $PSScriptRoot 'summarize-results.ps1') -CsvPath $CsvPath
    $ExpectedRows = @(
        Import-Csv -LiteralPath $CsvPath | Where-Object { $_.success -eq '1' }
    ).Count
    $DrainDeadline = (Get-Date).AddSeconds($DrainTimeoutSeconds)
    do {
        $PersistedRows = [int](docker compose -f docker-compose.benchmark.yml exec -T mysql `
            mysql -N -s -ubenchmark -pbenchmark_only_password monitor_db `
            -e "SELECT COUNT(*) FROM server_performance WHERE server_name LIKE 'benchmark-$RunId-%';")
        if ($PersistedRows -ge $ExpectedRows) { break }
        Start-Sleep -Seconds 1
    } while ((Get-Date) -lt $DrainDeadline)

    "server_performance_rows=$PersistedRows expected_successful_reports=$ExpectedRows"
    if ($PersistedRows -lt $ExpectedRows) {
        throw "MySQL persistence drain timeout: expected at least $ExpectedRows rows, got $PersistedRows."
    }

    docker compose -f docker-compose.benchmark.yml stop manager | Out-Null
    $ManagerLogs = docker compose -f docker-compose.benchmark.yml logs manager --no-color | Out-String
    $StatsMatch = [regex]::Match(
        $ManagerLogs,
        'processing_stats accepted=(\d+) processed=(\d+) persistence_tasks=(\d+)')
    if (-not $StatsMatch.Success) {
        throw 'Manager processing_stats line was not found after graceful shutdown.'
    }
    "manager_accepted=$($StatsMatch.Groups[1].Value) manager_processed=$($StatsMatch.Groups[2].Value) manager_persistence_tasks=$($StatsMatch.Groups[3].Value)"
    if ([int64]$StatsMatch.Groups[1].Value -ne $ExpectedRows -or
        [int64]$StatsMatch.Groups[2].Value -lt [int64]$StatsMatch.Groups[1].Value) {
        throw 'Manager accepted/processed counters do not match the benchmark input.'
    }
} finally {
    Pop-Location
}
