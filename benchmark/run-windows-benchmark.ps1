param(
    [ValidateRange(1, 1000)] [int]$Workers = 10,
    [ValidateRange(1, 3600)] [int]$DurationSeconds = 30,
    [ValidateRange(10, 60000)] [int]$IntervalMs = 1000
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
    $PersistedRows = [int](docker compose -f docker-compose.benchmark.yml exec -T mysql `
        mysql -N -s -ubenchmark -pbenchmark_only_password monitor_db `
        -e "SELECT COUNT(*) FROM server_performance WHERE server_name LIKE 'benchmark-$RunId-%';")
    "server_performance_rows=$PersistedRows expected_successful_reports=$ExpectedRows"
    if ($PersistedRows -lt $ExpectedRows) {
        throw "MySQL persistence mismatch: expected at least $ExpectedRows rows, got $PersistedRows."
    }
} finally {
    Pop-Location
}
