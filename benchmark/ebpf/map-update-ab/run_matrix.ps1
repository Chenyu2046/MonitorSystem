[CmdletBinding()]
param(
  [string]$EvidenceDir = "",
  [string]$RemoteRoot = "/home/chenyu/MonitorSystem",
  [string]$RemoteUser = "chenyu",
  [string]$RemoteHost = "127.0.0.1",
  [int]$SshPort = 2222,
  [string]$IperfPath = "C:\Users\26561\AppData\Local\Microsoft\WinGet\Packages\ar51an.iPerf3_Microsoft.Winget.Source_8wekyb3d8bbwe\iperf3.exe",
  [int]$WarmupSeconds = 5,
  [int]$DurationSeconds = 5,
  [int]$Rounds = 5,
  [int]$BandwidthMbpsPerStream = 10,
  [switch]$SmokeOnly
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
if ([string]::IsNullOrWhiteSpace($EvidenceDir)) {
  $EvidenceDir = Join-Path $repo ("benchmark/evidence/linux-ebpf-map-update-ab-" + (Get-Date -Format yyyyMMdd))
}
$rawDir = Join-Path $EvidenceDir "raw"
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null

$sshArgs = @("-p", "$SshPort", "-o", "BatchMode=yes", "$RemoteUser@$RemoteHost")
$pw = -join ([int[]](89,117,116,97,111,50,48,48,52,51,49,48) | ForEach-Object {[char]$_})
$remoteBuild = "$RemoteRoot/benchmark/ebpf/map-update-ab/.output"
$remoteRunner = "$remoteBuild/map_update_runner"
$remoteObject = @{
  global = "$remoteBuild/global_update.bpf.o"
  percpu = "$remoteBuild/percpu_update.bpf.o"
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
  [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function Invoke-SshText([string]$Script) {
  $psi = [Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = "ssh.exe"
  $psi.Arguments = (($sshArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ } }) -join ' ') + " bash -s"
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $p.StandardInput.Write($Script)
  $p.StandardInput.Close()
  $stdout = $p.StandardOutput.ReadToEnd()
  $stderr = $p.StandardError.ReadToEnd()
  $p.WaitForExit()
  if ($p.ExitCode -ne 0) { throw "SSH failed ($($p.ExitCode)): $stderr`n$stdout" }
  return $stdout
}

function Invoke-SudoText([string]$Script) {
  $psi = [Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = "ssh.exe"
  $psi.Arguments = (($sshArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ } }) -join ' ') + " sudo -S -p '' bash -s"
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi)
  $p.StandardInput.WriteLine($pw)
  $p.StandardInput.Write($Script)
  $p.StandardInput.Close()
  $stdout = $p.StandardOutput.ReadToEnd()
  $stderr = $p.StandardError.ReadToEnd()
  $p.WaitForExit()
  if ($p.ExitCode -ne 0) { throw "SSH sudo failed ($($p.ExitCode)): $stderr`n$stdout" }
  return $stdout
}

function Wait-RemoteFile([string]$Path, [int]$Seconds = 20) {
  for ($i = 0; $i -lt ($Seconds * 4); $i++) {
    $out = Invoke-SshText "test -s '$Path' && cat '$Path' || true"
    if ($out -match "READY") { return $out }
    Start-Sleep -Milliseconds 250
  }
  return (Invoke-SshText "cat '$Path' 2>/dev/null || true")
}

function Start-GuestServer([string]$Affinity, [string]$Path) {
  $script = @"
set -eu
pkill -x iperf3 2>/dev/null || true
rm -f '$Path'
nohup taskset -c '$Affinity' iperf3 -s -1 -p 5201 -J > '$Path' 2>&1 < /dev/null &
echo `$!
"@
  return (Invoke-SshText $script).Trim()
}

function Wait-GuestProcess([string]$GuestPid, [string]$Path) {
  $script = @"
set +e
for i in `$(seq 1 120); do
  kill -0 '$GuestPid' 2>/dev/null || break
  sleep 0.1
done
cat '$Path' 2>/dev/null || true
"@
  return (Invoke-SshText $script)
}

function Run-HostIperf([int]$Payload, [int]$Streams, [int]$Seconds, [string]$OutPath) {
  $args = @("-c", "127.0.0.1", "-p", "5201", "-u", "-b", "${BandwidthMbpsPerStream}M", "-l", "$Payload", "-P", "$Streams", "-t", "$Seconds", "-J")
  $out = & $IperfPath @args 2>&1 | Out-String
  Write-Utf8NoBom $OutPath $out
  return $LASTEXITCODE
}

function Get-Name([string]$Variant, [int]$Cpu, [int]$Payload, [int]$Streams, [int]$Round, [string]$Phase) {
  return "$Variant-cpu$Cpu-size$Payload-stream$Streams-r$('{0:D2}' -f $Round)-$Phase"
}

$envText = @"
date=$(Get-Date -Format o)
host=$(hostname)
windows_iperf3=$IperfPath
windows_iperf3_version=3.21
remote=$RemoteHost`:$SshPort
guest_interface=enp0s3
guest_ifindex=2
warmup_seconds=$WarmupSeconds
duration_seconds=$DurationSeconds
bandwidth_mbps_per_stream=$BandwidthMbpsPerStream
matrix=payload(64,512,1500) x streams(1,8) x cpu(1,2,4) x rounds($Rounds) x variants(global,percpu)
traffic_client_affinity=not_pinned_windows_host
guest_server_affinity=taskset_cpu_sets_0;0,1;0-3
cpu_binding_limit=best_effort_server_process_only; irq_and_virtualbox_vcpu_not_isolated
"@
Write-Utf8NoBom (Join-Path $EvidenceDir "environment.txt") $envText
Write-Utf8NoBom (Join-Path $EvidenceDir "commands.txt") @"
ssh -p $SshPort $RemoteHost
sudo sysctl kernel.bpf_stats_enabled=1
bpftool prog show -j (before/after measured window)
Windows iperf3 3.21: -u -b ${BandwidthMbpsPerStream}M -l PAYLOAD -P STREAMS -t DURATION -J
guest: taskset -c AFFINITY iperf3 -s -1 -p 5201 -J
"@

$metadataPath = Join-Path $EvidenceDir "rounds.jsonl"
if (Test-Path $metadataPath) { Remove-Item -LiteralPath $metadataPath }
$variants = @("global", "percpu")
$payloads = @(64, 512, 1500)
$streamsList = @(1, 8)
$cpus = @(1, 2, 4)
if ($SmokeOnly) {
  $variants = @("global", "percpu"); $payloads = @(64); $streamsList = @(1); $cpus = @(1); $Rounds = 1
}

foreach ($variant in $variants) {
  foreach ($payload in $payloads) {
    foreach ($streams in $streamsList) {
      foreach ($cpu in $cpus) {
        $affinity = switch ($cpu) { 1 { "0" } 2 { "0,1" } 4 { "0-3" } }
        for ($round = 1; $round -le $Rounds; $round++) {
          $name = Get-Name $variant $cpu $payload $streams $round "measured"
          $runnerLog = "/tmp/map-update-ab-$name-runner.log"
          $serverWarmup = "/tmp/map-update-ab-$name-warmup.json"
          $serverMeasured = "/tmp/map-update-ab-$name-measured.json"
          $mpstatPath = "/tmp/map-update-ab-$name-mpstat.txt"
          $beforePath = "/tmp/map-update-ab-$name-before.json"
          $afterPath = "/tmp/map-update-ab-$name-after.json"
          $localPrefix = Join-Path $rawDir $name
          Write-Host "[$variant cpu=$cpu payload=$payload streams=$streams round=$round] start"

          $runnerCmd = "set -eu; tc qdisc del dev enp0s3 clsact 2>/dev/null || true; : > '$runnerLog'; chmod 644 '$runnerLog'; nohup '$remoteRunner' '$($remoteObject[$variant])' 2 60 >> '$runnerLog' 2>&1 < /dev/null & runner_pid=`$!; echo `$runner_pid"
          $runnerPid = (((Invoke-SudoText $runnerCmd) -split "`r?`n") | Where-Object { $_ -match '^\d+$' } | Select-Object -Last 1).Trim()
          $ready = Wait-RemoteFile $runnerLog
          Write-Utf8NoBom "$localPrefix-runner.txt" $ready
          if ($ready -notmatch "READY") {
            $record = @{ variant=$variant; payload=$payload; streams=$streams; cpu_limit=$cpu; round=$round; valid=$false; invalid_reason="runner_not_ready"; affinity="taskset -c $affinity" } | ConvertTo-Json -Compress
            Add-Content -LiteralPath $metadataPath -Value $record
            Invoke-SudoText "kill '$runnerPid' 2>/dev/null || true; tc qdisc del dev enp0s3 clsact 2>/dev/null || true" | Out-Null
            continue
          }

          $warmPid = Start-GuestServer $affinity $serverWarmup
          Start-Sleep -Milliseconds 500
          $warmOut = Join-Path $rawDir "$name-warmup-client.json"
          $warmRc = Run-HostIperf $payload $streams $WarmupSeconds $warmOut
          $warmServerOut = Wait-GuestProcess $warmPid $serverWarmup
          Write-Utf8NoBom "$localPrefix-warmup-server.json" $warmServerOut

          $measPid = Start-GuestServer $affinity $serverMeasured
          Start-Sleep -Milliseconds 500
          Invoke-SudoText "bpftool prog show -j > '$beforePath'" | Out-Null
          $mpPid = (Invoke-SshText "rm -f '$mpstatPath'; nohup mpstat -P ALL 1 $($DurationSeconds + 1) > '$mpstatPath' 2>&1 < /dev/null & echo `$!").Trim()
          Start-Sleep -Milliseconds 250
          $clientOut = Join-Path $rawDir "$name-client.json"
          $clientRc = Run-HostIperf $payload $streams $DurationSeconds $clientOut
          $serverOut = Wait-GuestProcess $measPid $serverMeasured
          for ($i = 0; $i -lt 30; $i++) {
            $mpDone = Invoke-SshText "kill -0 '$mpPid' 2>/dev/null; echo `$?"
            if ($mpDone.Trim() -eq "1") { break }
            Start-Sleep -Milliseconds 200
          }
          $mpstatOut = Invoke-SshText "cat '$mpstatPath' 2>/dev/null || true"
          $beforeOut = Invoke-SudoText "cat '$beforePath' 2>/dev/null || true"
          $afterOut = Invoke-SudoText "bpftool prog show -j > '$afterPath'; cat '$afterPath'"
          Write-Utf8NoBom "$localPrefix-server.json" $serverOut
          Write-Utf8NoBom "$localPrefix-mpstat.txt" $mpstatOut
          Write-Utf8NoBom "$localPrefix-bpf-before.json" $beforeOut
          Write-Utf8NoBom "$localPrefix-bpf-after.json" $afterOut

          $valid = ($warmRc -eq 0 -and $clientRc -eq 0 -and $serverOut -match '"end"')
          $record = @{ variant=$variant; payload=$payload; streams=$streams; cpu_limit=$cpu; round=$round; valid=$valid; affinity="taskset -c $affinity"; client_exit=$clientRc; warmup_client_exit=$warmRc; server_file="$name-server.json"; bpf_before="$name-bpf-before.json"; bpf_after="$name-bpf-after.json"; mpstat_file="$name-mpstat.txt" } | ConvertTo-Json -Compress
          Add-Content -LiteralPath $metadataPath -Value $record
          Invoke-SudoText "kill '$runnerPid' 2>/dev/null || true; tc qdisc del dev enp0s3 clsact 2>/dev/null || true" | Out-Null
          Write-Host "[$variant cpu=$cpu payload=$payload streams=$streams round=$round] done valid=$valid"
        }
      }
    }
  }
}

Write-Host "raw evidence: $EvidenceDir"
