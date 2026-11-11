param(
    [string]$Disk = (Join-Path $PSScriptRoot 'game.dsk'),
    [string]$Python,
    [string]$Node,
    [string]$Zig,
    [ValidateSet('0','1','2','3')][string]$Optimize = '2',
    [switch]$CheckDisk,
    [switch]$Launch
)

$ErrorActionPreference = 'Stop'
$expectedHash = 'F5DEE21140345EA84BEE66CCF86DB892C079CE13BC1D13C240A2103BA0CE2A26'
if (!(Test-Path -LiteralPath $Disk -PathType Leaf)) {
    throw "Disk image not found at '$Disk'. Put your compatible CPC disk image beside Build.ps1 as game.dsk, or pass -Disk with its path."
}
$Disk = (Resolve-Path -LiteralPath $Disk).ProviderPath
$diskHash = (Get-FileHash -LiteralPath $Disk -Algorithm SHA256).Hash
if ($diskHash -ne $expectedHash) {
    throw "Unsupported disk image. Expected SHA-256 $expectedHash, got $diskHash. See README.md for the tested dump."
}
if ($CheckDisk) {
    Write-Output "Compatible disk image. MD5 $((Get-FileHash -LiteralPath $Disk -Algorithm MD5).Hash)"
    return
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
$Zig = Find-BuildTool $Zig 'zig.exe' '-Zig'

$native = Join-Path $PSScriptRoot 'native'
$map = Join-Path $PSScriptRoot 'map'
$build = [IO.Directory]::CreateDirectory((Join-Path $native 'build')).FullName
$work = Join-Path $build ('work-' + [Guid]::NewGuid().ToString('N'))
$extracted = Join-Path $work 'extracted'
$recovered = Join-Path $extracted 'recovered'
$renderer = Join-Path $work 'capture-rooms.exe'
$game = Join-Path $build 'SorceryPlus.exe'
$stagedGame = Join-Path $work 'SorceryPlus.exe'
[void][IO.Directory]::CreateDirectory($work)
try {
    Invoke-BuildStep 'Extracting disk' $Node @((Join-Path $PSScriptRoot 'tools\sorcery-extract.mjs'), $Disk, $extracted)
    Invoke-BuildStep 'Preparing assets' $Python @((Join-Path $native 'prepare.py'), $Disk, $extracted, $work)
    Invoke-BuildStep 'Translating Z80 code' $Python @((Join-Path $native 'translate.py'), $recovered, (Join-Path $work 'translated.inc'))

    $compile = @{ Python = $Python; Zig = $Zig; Work = $work; Optimize = $Optimize }
    Write-Host 'Building the room capture tool'
    & (Join-Path $native 'compile.ps1') @compile -Output $renderer -Capture
    Invoke-BuildStep 'Capturing and mapping rooms' $Python @((Join-Path $map 'extract_atlas.py'), $recovered, '--renderer', $renderer, '--out', $work)
    Invoke-BuildStep 'Generating atlas pages' $Python @((Join-Path $map 'build_pages.py'), '--data-dir', $work, '--sprites-from', $recovered)
    Invoke-BuildStep 'Verifying the generated atlas' $Python @((Join-Path $map 'verify_atlas.py'), '--data-dir', $work)

    Write-Host 'Compiling SorceryPlus.exe'
    & (Join-Path $native 'compile.ps1') @compile -Output $stagedGame
    Move-Item -LiteralPath $stagedGame -Destination $game -Force
} finally {
    $workItem = Get-Item -LiteralPath $work -ErrorAction SilentlyContinue
    if ($workItem -and $workItem.Parent.FullName -eq $build -and
            $workItem.Name -match '^work-[0-9a-f]{32}$' -and
            !($workItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $workItem.FullName -Recurse -Force
    }
}
Write-Output "Built $game"
if ($Launch) { Start-Process -FilePath $game -WorkingDirectory $build }
