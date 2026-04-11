param(
    [string]$ExePath = "C:\Quake\rerelease\ironwail.exe",
    [string]$WorkDir = "C:\Quake\rerelease",
    [int]$Seconds = 8
)

$ErrorActionPreference = "Stop"
$backends = @("OpenGL", "Vulkan", "DX12")

foreach ($backend in $backends) {
    Write-Host "== Smoke backend: $backend =="
    $args = @("-nosteamapi", "-condebug", "+r_backend", $backend)
    $p = Start-Process -FilePath $ExePath -WorkingDirectory $WorkDir -ArgumentList $args -PassThru
    Start-Sleep -Seconds $Seconds
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
    }
    Write-Host ("exit_code={0}" -f $p.ExitCode)
}

Write-Host "log: $WorkDir\qconsole.log"
