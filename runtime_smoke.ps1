param(
	[int]$Runs = 1,
	[string]$ExePath = "C:\Quake\rerelease\ironwail.exe",
	[string]$WorkDir = "C:\Quake\rerelease",
	[string]$LogDir = "C:\Quake\source\ironwail\Windows\VisualStudio\Build-ironwail\runtime-smoke-logs",
	[string]$ExtraArgs = ""
)

$ErrorActionPreference = "Stop"

if ($Runs -lt 1) {
	throw "Runs must be >= 1"
}
if (-not (Test-Path $ExePath)) {
	throw "Executable not found: $ExePath"
}
if (-not (Test-Path $WorkDir)) {
	throw "WorkDir not found: $WorkDir"
}

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$fail = 0
$summary = @()
$qconsole = Join-Path $WorkDir "qconsole.log"
$errorPattern = "Host_Error|Sys_Error|GL error|FrameGraph .*incomplete FBO|FATAL|CRASH"

for ($i = 1; $i -le $Runs; $i++) {
	$ts = Get-Date -Format "yyyyMMdd_HHmmss_fff"
	$stdoutLog = Join-Path $LogDir ("stdout_{0}_run{1}.log" -f $ts, $i)
	$qconsoleLog = Join-Path $LogDir ("qconsole_{0}_run{1}.log" -f $ts, $i)
	$args = '-condebug -window +developer 1 +r_gl_state_validate 1 ' + $ExtraArgs + ' +timedemo demo1 +quit'
	$cmd = ('cd /d "{0}" && "{1}" {2}' -f $WorkDir, $ExePath, $args)

	if (Test-Path $qconsole) {
		$removed = $false
		for ($retry = 0; $retry -lt 20; $retry++) {
			try {
				Remove-Item $qconsole -Force -ErrorAction Stop
				$removed = $true
				break
			} catch {
				Start-Sleep -Milliseconds 200
			}
		}
		if (-not $removed -and (Test-Path $qconsole)) {
			throw "Could not remove locked qconsole.log before run $i"
		}
	}

	cmd /c $cmd 2>&1 | Tee-Object -FilePath $stdoutLog | Out-Null

	if (Test-Path $qconsole) {
		Copy-Item $qconsole $qconsoleLog -Force
		$errors = Select-String -Path $qconsoleLog -Pattern $errorPattern -SimpleMatch:$false
	} else {
		$errors = @("qconsole.log missing")
	}

	$ok = ($errors.Count -eq 0)
	if (-not $ok) {
		$fail++
	}

	$summary += [PSCustomObject]@{
		run = $i
		ok = $ok
		stdout = $stdoutLog
		qconsole = if (Test-Path $qconsoleLog) { $qconsoleLog } else { "<missing>" }
		error_count = $errors.Count
	}
}

$summary | Format-Table -AutoSize | Out-String | Write-Host

if ($fail -gt 0) {
	throw "Runtime smoke failed in $fail/$Runs runs"
}

Write-Host ("Runtime smoke passed ({0}/{0} runs)." -f $Runs)
