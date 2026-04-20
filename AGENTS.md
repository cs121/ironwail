# AGENTS.md

## Mission

Maintain and improve Ironwail with a high-performance engine mindset.

Combine:
- Original local build/runtime workflows
- Improved v2 engineering rules
- Carmack-style coding philosophy
- AI agent guardrails
- Practical smoke testing

---

## Runtime / Smoke Tests

Always run from:
C:\Quake\rerelease

Preferred executable:
C:\Quake\rerelease\ironwail.exe

Quick smoke:
```powershell
Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease"
```

Timed smoke:
```powershell
$p = Start-Process -FilePath "C:\Quake\rerelease\ironwail.exe" -WorkingDirectory "C:\Quake\rerelease" -PassThru
Start-Sleep -Seconds 5
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
```

Use:
- -condebug
- -nosteamapi

---

## Build

```bash
"/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" C:/projects/ironwail/Windows/VisualStudio/ironwail.sln /t:Build /p:Configuration=Release;Platform=x64 /m
```

Copy built binary to:

C:\Quake\rerelease\ironwail.exe

---

## Carmack Style Rules

1. Make it work
2. Make it fast
3. Make it clean

Prefer:
- simple code
- direct logic
- measurable performance
- clear ownership
- small functions

Avoid:
- unnecessary abstraction
- speculative rewrites
- clever but fragile code

---

## AI Agent Rules

Always:
1. inspect symbols
2. inspect call sites
3. understand ownership
4. apply smallest safe fix

Never:
- fake compile success
- claim tested if not tested
- rewrite subsystems blindly

---

## Renderer Rules

Respect:
- Reverse-Z
- GL state correctness
- FBO validity
- existing fallback paths

Retest:
- shadows
- postfx
- dynamic lights
- UI/HUD
- resolution changes

---

## Performance Rules

Avoid:
- per-frame allocations
- repeated lookups
- duplicate passes
- sync stalls

Prefer:
- cache reuse
- batching
- linear memory access

---

## Audio Rules

No allocations / I/O in callback.

SDL backend only.

---

## Dangerous Files

- gl_rmain.c
- gl_shadow.c
- r_postfx.c
- snd_dma.c
- host_cmd.c
- sv_phys.c

---

## Final Rule

The best code is fast, stable, understandable code.
