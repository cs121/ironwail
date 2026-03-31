# Agent Notes

## Runtime / Smoke Tests

- Always run local smoke tests and manual launches from `C:\Quake\rerelease`.
- Always use `C:\Quake\rerelease` as the working directory when launching `ironwail.exe`.
- Preferred executable path: `C:\Quake\rerelease\ironwail.exe`.
- Preferred quick smoke test:
  ```powershell
  Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease"
  ```
- Preferred timed smoke test:
  ```powershell
  $p = Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease" -PassThru
  Start-Sleep -Seconds 5
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  ```
- If Steam integration blocks a local engine-only smoke, it is acceptable to add `-nosteamapi` for debugging-only runs.
- Use `-condebug` when you need a startup/runtime log.
- When launched from `C:\Quake\rerelease`, the log file is `C:\Quake\rerelease\qconsole.log`.

## Build / Deploy

- Preferred Windows build command:
  ```bash
  "/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" C:/projects/ironwail/Windows/VisualStudio/ironwail.sln /t:Build /p:Configuration=Release\;Platform=x64 /m
  ```
- Preferred output binary:
  `/mnt/c/projects/ironwail/Windows/VisualStudio/Build-ironwail/bin/x64/Release/ironwail.exe`
- After a successful Windows build, copy the fresh binary to:
  `C:\Quake\rerelease\ironwail.exe`
- Runtime shader overrides used for local debugging live under:
  `C:\Quake\rerelease\id1\shaders`

## Repo Conventions

- Match the existing engine style and naming instead of introducing new subsystem-specific conventions.
- Use `rg` / `rg --files` for search.
- Use `apply_patch` for manual file edits.
- Do not revert unrelated user changes in a dirty tree.
- Keep the tree compiling after each milestone or focused fix.
- Prefer small, reviewable changes over large rewrites.

## Audio Work Notes

- SDL is backend-only: device init, format negotiation, callback/queue/output.
- Keep SDL types out of gameplay-facing and high-level audio APIs.
- Do not do parsing, file I/O, allocation, or expensive string lookup in the audio callback.
- Keep raw playback functional as fallback while sound-def migration is incomplete.
- Internal sound-def/runtime changes should stay incremental and compile-safe.

## Known Local Debugging Notes

- `ironwail.cfg` in the local test install lives at `C:\Quake\rerelease\id1\ironwail.cfg`.
- If a smoke test depends on runtime config behavior, verify the installed config and not only the repo defaults.
- Local Windows Event Viewer entries for hangs/crashes can be useful:
  `Application` log, providers `Application Error` and `Windows Error Reporting`.

## Known Repro Commands

- Short startup smoke:
  ```powershell
  $p = Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease" -PassThru
  Start-Sleep -Seconds 5
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  ```
- Startup smoke with console log:
  ```powershell
  $p = Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease" -ArgumentList "-condebug" -PassThru
  Start-Sleep -Seconds 5
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  ```
- Engine-only smoke when Steam integration gets in the way:
  ```powershell
  $p = Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease" -ArgumentList "-nosteamapi","-condebug" -PassThru
  Start-Sleep -Seconds 5
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  ```
- Demo-loop style smoke:
  ```powershell
  $p = Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease" -ArgumentList "-condebug","+developer","1" -PassThru
  Start-Sleep -Seconds 90
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  ```
- Direct map load smoke:
  ```powershell
  $p = Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease" -ArgumentList "-condebug","+map","start" -PassThru
  Start-Sleep -Seconds 20
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  ```
