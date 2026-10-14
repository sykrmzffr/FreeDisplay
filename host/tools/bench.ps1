# Latency benchmark: runs the host with each argument set while a test animation
# plays on the virtual monitor, then prints the host's latency lines.
param([string[]]$Configs = @("", "--speed 0", "--speed 0 --scale 75", "--speed 0 --scale 67", "--speed 0 --scale 50"))

$out = Join-Path $PSScriptRoot "..\out"
foreach ($cfg in $Configs) {
    Get-Process FreeDisplayHost -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    $log = Join-Path $env:TEMP "fd_bench.log"
    $argList = @("--no-install") + ($cfg -split ' ' | Where-Object { $_ })
    $p = Start-Process (Join-Path $out "FreeDisplayHost.exe") -ArgumentList $argList -RedirectStandardOutput $log -NoNewWindow -PassThru
    for ($i = 0; $i -lt 40 -and -not (Select-String -Path $log -Pattern "Tablet decoder" -Quiet -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    & (Join-Path $out "animate.exe") 1 17
    Stop-Process -Id $p.Id -Force
    "=== [$cfg]"
    Select-String -Path $log -Pattern "Encoder ready|latency avg" | Select-Object -Last 3 | ForEach-Object { $_.Line }
}
