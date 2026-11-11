param(
    [string]$Disk = (Join-Path $PSScriptRoot 'game.dsk'),
    [string]$AtlasLanguageDisk,
    [string]$Output,
    [string]$Python,
    [string]$Node,
    [string]$Zig,
    [ValidateSet('0','1','2','3')][string]$Optimize = '2',
    [switch]$CheckDisk,
    [switch]$TryOtherDisk,
    [switch]$Launch
)

$ErrorActionPreference = 'Stop'
$expectedHash = 'F5DEE21140345EA84BEE66CCF86DB892C079CE13BC1D13C240A2103BA0CE2A26'
if (!(Test-Path -LiteralPath $Disk -PathType Leaf)) {
    throw "Disk image not found at '$Disk'. Put your compatible CPC disk image beside Build.ps1 as game.dsk, or pass -Disk with its path."
}
$Disk = (Resolve-Path -LiteralPath $Disk).ProviderPath
$diskHash = (Get-FileHash -LiteralPath $Disk -Algorithm SHA256).Hash
if ($CheckDisk -and $diskHash -eq $expectedHash) {
    Write-Output "Compatible UK disk image. MD5 $((Get-FileHash -LiteralPath $Disk -Algorithm MD5).Hash)"
    return
}
if ($diskHash -ne $expectedHash -and !$TryOtherDisk) {
    Write-Host "This disk image differs from the tested dump. SHA-256 $diskHash"
    $choice = Read-Host 'Try extracting it and checking the game data? [y/N]'
    if ($choice -notmatch '^(y|yes)$') {
        Write-Output 'Disk check canceled.'
        return
    }
}

function Find-BuildTool([string]$Override, [string]$Command, [string]$Option) {
    if ($Override) { return $Override }
    $tool = Get-Command $Command -All -CommandType Application -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notlike '*\Microsoft\WindowsApps\*' } | Select-Object -First 1
    if (!$tool) { throw "Install $Command and add it to PATH, or pass $Option with its full path." }
    return $tool.Source
}

function Invoke-BuildStep([string]$Name, [string]$Tool, [string[]]$Arguments) {
    Write-Host $Name
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed (exit $LASTEXITCODE)." }
}

$Python = Find-BuildTool $Python 'python.exe' '-Python'
$Node = Find-BuildTool $Node 'node.exe' '-Node'

$native = Join-Path $PSScriptRoot 'native'
$map = Join-Path $PSScriptRoot 'map'
$build = [IO.Directory]::CreateDirectory((Join-Path $native 'build')).FullName
$work = Join-Path $build ('work-' + [Guid]::NewGuid().ToString('N'))
$extracted = Join-Path $work 'extracted'
$recovered = Join-Path $extracted 'recovered'
$renderer = Join-Path $work 'capture-rooms.exe'
$stagedGame = Join-Path $work 'SorceryPlus.exe'
[void][IO.Directory]::CreateDirectory($work)
try {
    Invoke-BuildStep 'Extracting disk' $Node @((Join-Path $PSScriptRoot 'tools\sorcery-extract.mjs'), $Disk, $extracted)
    Invoke-BuildStep 'Checking and preparing game assets' $Python @((Join-Path $native 'prepare.py'), $extracted, $work)
    if ($CheckDisk) {
        Write-Output "Compatible game data. MD5 $((Get-FileHash -LiteralPath $Disk -Algorithm MD5).Hash)"
        return
    }
    $game = if ($Output) {
        $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
    } else {
        Join-Path (Split-Path -Parent $Disk) 'SorceryPlus.exe'
    }
    if ([IO.Path]::GetExtension($game) -ne '.exe') { throw '-Output must name an .exe file.' }
    $Zig = Find-BuildTool $Zig 'zig.exe' '-Zig'
    $atlasOptions = @()
    if ($AtlasLanguageDisk) {
        $secondDisk = (Resolve-Path -LiteralPath $AtlasLanguageDisk).ProviderPath
        $alternate = Join-Path $work 'alternate-language'
        [void][IO.Directory]::CreateDirectory($alternate)
        $alternateExtraction = Join-Path $alternate 'extracted'
        Invoke-BuildStep 'Extracting the atlas language disk' $Node @((Join-Path $PSScriptRoot 'tools\sorcery-extract.mjs'), $secondDisk, $alternateExtraction)
        Invoke-BuildStep 'Checking the atlas language disk' $Python @((Join-Path $native 'prepare.py'), $alternateExtraction, $alternate)
        $primaryLanguage = (Get-Content -Raw -LiteralPath (Join-Path $work 'edition.json') | ConvertFrom-Json).language
        $alternateLanguage = (Get-Content -Raw -LiteralPath (Join-Path $alternate 'edition.json') | ConvertFrom-Json).language
        if ($primaryLanguage -eq $alternateLanguage) { throw 'The atlas language disk must be the other edition (UK or French).' }
        Invoke-BuildStep 'Reading alternate room and item names' $Python @((Join-Path $map 'extract_atlas.py'), (Join-Path $alternateExtraction 'recovered'), '--out', $alternate)
        $atlasOptions = @('--alternate-data', $alternate)
    }
    Invoke-BuildStep 'Translating Z80 code' $Python @((Join-Path $native 'translate.py'), $recovered, (Join-Path $work 'translated.inc'))

    $compile = @{ Python = $Python; Zig = $Zig; Work = $work; Optimize = $Optimize }
    Write-Host 'Building the room capture tool'
    & (Join-Path $native 'compile.ps1') @compile -Output $renderer -Capture
    Invoke-BuildStep 'Capturing and mapping rooms' $Python @((Join-Path $map 'extract_atlas.py'), $recovered, '--renderer', $renderer, '--out', $work)
    $pageArguments = @((Join-Path $map 'build_pages.py'), '--data-dir', $work, '--sprites-from', $recovered) + $atlasOptions
    Invoke-BuildStep 'Generating atlas pages' $Python $pageArguments
    Invoke-BuildStep 'Verifying the generated atlas' $Python @((Join-Path $map 'verify_atlas.py'), '--data-dir', $work)

    Write-Host 'Compiling SorceryPlus.exe'
    & (Join-Path $native 'compile.ps1') @compile -Output $stagedGame
    $destination = [IO.Directory]::CreateDirectory((Split-Path -Parent $game)).FullName
    $publishFile = Join-Path $destination ('.sorceryplus-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::Copy($stagedGame, $publishFile)
        if ([IO.File]::Exists($game)) {
            [IO.File]::Replace($publishFile, $game, [NullString]::Value)
        } else {
            [IO.File]::Move($publishFile, $game)
        }
    } finally {
        if ([IO.File]::Exists($publishFile)) { [IO.File]::Delete($publishFile) }
    }
} finally {
    $workItem = Get-Item -LiteralPath $work -ErrorAction SilentlyContinue
    if ($workItem -and $workItem.Parent.FullName -eq $build -and
            $workItem.Name -match '^work-[0-9a-f]{32}$' -and
            !($workItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $workItem.FullName -Recurse -Force
    }
}
Write-Output "Built $game"
if ($Launch) { Start-Process -FilePath $game -WorkingDirectory (Split-Path -Parent $game) }
