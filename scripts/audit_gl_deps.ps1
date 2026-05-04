# audit_gl_deps.ps1 - Ironwail GL dependency audit
# Scans for OpenGL leaks outside ref_gl.dll project files
# Usage: powershell -File scripts/audit_gl_deps.ps1

$ErrorActionPreference = "SilentlyContinue"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Files compiled into ref_gl.dll (from ref_gl.vcxproj)
$ref_gl_sources = @(
    "ref_gl_plugin.c", "ref_gl_bridge_stubs.c", "gl_backend.c",
    "gl_backend_runtime.c", "gl_backend_resources.c", "gl_draw.c",
    "gl_rmain.c", "gl_rmisc.c", "gl_screen.c", "gl_vidsdl.c",
    "gl_model.c", "gl_mesh.c", "gl_sky.c", "gl_warp.c",
    "gl_texmgr.c", "gl_shaders.c", "gl_shadow.c", "gl_shadow_runtime.c",
    "gl_oit.c", "gl_lightgrid.c", "gl_ktx2.c", "gl_refrag.c",
    "gl_rlight.c", "gl_fog.c", "gl_dlight.c",
    "r_world.c", "r_alias.c", "r_entitylight.c", "r_part.c",
    "r_part_q3p.c", "r_decals.c", "r_sprite.c", "r_brush.c",
    "r_resources_gl.c", "r_quality.c", "r_sanitize.c", "r_ssao.c",
    "r_tonemap.c", "r_postfx.c"
)

function Is-RefGlFile {
    param([string]$Path)
    $name = Split-Path -Leaf $Path
    return $ref_gl_sources -contains $name
}

function Search-GL {
    param([string]$Pattern, [string]$Label, [string[]]$Include)
    Write-Host "`n=== $Label ===" -ForegroundColor Cyan
    $isCaseSensitive = ($Pattern -match '\[A-Z\]')
    $searchPaths = @("$Root\Quake")
    if ($Include -eq "*.vcxproj") { $searchPaths = @("$Root\Windows") }
    $results = Get-ChildItem -Path $searchPaths -Recurse -File -Include $Include |
        Select-String -Pattern $Pattern -CaseSensitive:$isCaseSensitive |
        Where-Object { $_.Path -notmatch 'thirdparty' -and $_.Path -notmatch 'build-logs' }
    $leaks = $results | Where-Object { -not (Is-RefGlFile $_.Path) }
    $total = ($results | Measure-Object).Count
    $leakCount = ($leaks | Measure-Object).Count
    Write-Host "  Total matches: $total | Outside ref_gl: $leakCount"
    if ($leakCount -gt 0) {
        $leaks | Group-Object Path | ForEach-Object {
            $short = $_.Name.Replace($Root + "\", "")
            $count = $_.Count
            $sample = ($_.Group | Select-Object -First 2).Line.Trim().Substring(0, [Math]::Min(100, $_.Group[0].Line.Trim().Length))
            Write-Host "  [$count] $short" -ForegroundColor Yellow
            Write-Host "      $sample" -ForegroundColor DarkGray
        }
    } else {
        Write-Host "  Clean." -ForegroundColor Green
    }
}

Write-Host "Ironwail GL Dependency Audit" -ForegroundColor White
Write-Host "============================" -ForegroundColor White
Write-Host "Root: $Root" -ForegroundColor DarkGray

Search-GL -Pattern 'GLuint' -Label "GLuint (GL object handles)" -Include "*.h"
Search-GL -Pattern 'GLenum' -Label "GLenum (GL enum types)" -Include "*.h"
Search-GL -Pattern 'gl[A-Z]\w*\(' -Label "Direct GL API calls" -Include "*.c"
Search-GL -Pattern 'SDL_GL_' -Label "SDL_GL calls" -Include "*.c"
Search-GL -Pattern '#include.*glquake' -Label "#include glquake.h" -Include "*.c","*.h"
Search-GL -Pattern 'framebufs\.' -Label "framebufs direct access" -Include "*.c"
Search-GL -Pattern 'glprogs\.' -Label "glprogs direct access" -Include "*.c"
Search-GL -Pattern 'opengl32\.lib' -Label "opengl32.lib linkage" -Include "*.vcxproj"

Write-Host "`n=== Header GL-Type Summary ===" -ForegroundColor Cyan
$glHeaders = @("glquake.h", "gl_model.h", "gl_texmgr.h", "gl_backend.h", "gl_ktx2.h")
foreach ($h in $glHeaders) {
    $path = "$Root\Quake\src\render\$h"
    if (Test-Path $path) {
        $GLuint = (Select-String -Pattern 'GLuint' -Path $path).Count
        $GLenum = (Select-String -Pattern 'GLenum' -Path $path).Count
        Write-Host "  $h : GLuint=$GLuint GLenum=$GLenum"
    }
}

Write-Host "`n=== Engine Link Dependencies ===" -ForegroundColor Cyan
$vcxproj = "$Root\Windows\VisualStudio\ironwail.vcxproj"
if (Test-Path $vcxproj) {
    $linksOpengl = (Select-String -Pattern 'opengl32\.lib' -Path $vcxproj).Count
    Write-Host "  ironwail.exe links opengl32.lib: $linksOpengl references"
}
$refvcx = "$Root\Windows\VisualStudio\ref_gl.vcxproj"
if (Test-Path $refvcx) {
    $linksOpengl = (Select-String -Pattern 'opengl32\.lib' -Path $refvcx).Count
    Write-Host "  ref_gl.dll links opengl32.lib: $linksOpengl references"
}

Write-Host "`nDone." -ForegroundColor White
