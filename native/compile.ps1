param(
    [Parameter(Mandatory)][string]$Zig,
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$Work,
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet('0','1','2','3')][string]$Optimize='2',
    [switch]$Capture
)

$ErrorActionPreference = 'Stop'
$embeddedSource = Join-Path $Work 'embedded-data.c'
$embedOptions = @(if ($Capture) { '--capture-only' } else { '--atlas-dir'; Join-Path $Work 'deliverable' })
& $Python (Join-Path $PSScriptRoot 'embed_assets.py') (Join-Path $Work 'assets') `
    (Join-Path $PSScriptRoot 'LAUNCH.txt') $embeddedSource --edition-info (Join-Path $Work 'edition.json') @embedOptions
if ($LASTEXITCODE -ne 0) { throw 'Asset embedding failed.' }

$sources = @('presentation.c','audio.c','audio_dialog.c','music.c','sound_manager.c','storage.c',
             'high_scores.c','save_dialog.c','keyboard_dialog.c','help_dialog.c','app_icon.c',
             'system_dialog_icon.c','room_atlas.c') | ForEach-Object { Join-Path $PSScriptRoot $_ }
if ($Capture) {
    $entry = Join-Path (Split-Path -Parent $PSScriptRoot) 'map\capture_rooms.c'
    $resources = @()
    $subsystem = 'console'
} else {
    $entry = Join-Path $PSScriptRoot 'machine.c'
    $resource = Join-Path $Work 'SorceryPlus.res'
    & $Zig rc /fo $resource /I $PSScriptRoot (Join-Path $PSScriptRoot 'SorceryPlus.rc')
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    $resources = @($resource)
    $subsystem = 'windows'
}
& $Zig cc -target x86_64-windows-gnu "-Wl,--subsystem,$subsystem" "-O$Optimize" `
    -I $Work -I $PSScriptRoot $entry @sources $embeddedSource @resources `
    -luser32 -lgdi32 -lwinmm -lole32 -lshell32 -luuid -lcomctl32 -lws2_32 -lbcrypt -o $Output
if ($LASTEXITCODE -ne 0) { throw "Zig failed to compile $Output." }
