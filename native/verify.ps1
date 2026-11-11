param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Baseline
)
$ErrorActionPreference='Stop'
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$Baseline=(Resolve-Path -LiteralPath $Baseline).Path
$testProfile='verify-' + [Guid]::NewGuid().ToString('N').Substring(0,16)
$profilePath=Join-Path (Split-Path -Parent $Executable) "profiles\$testProfile\progress.sav"

function Run-Game([string]$path,[string[]]$arguments) {
    $out= & $path @arguments
    if($LASTEXITCODE -ne 0) { throw "Game exited $LASTEXITCODE for $($arguments -join ' ')" }
    $out
}
function Read-Progress([string[]]$lines,[string]$label) {
    $found=$lines | Where-Object { $_ -like "$label frame=*" } | Select-Object -Last 1
    if(!$found) { throw "Missing $label progress" }
    $found.Substring($label.Length+1)
}

$scenarios=@(
    @{ Name='Village gameplay'; Args=@('--probe','2400','--fire','150','175','--move','450','620','8') },
    @{ Name='Room transitions'; Args=@('--probe','5500') }
)
foreach($scenario in $scenarios) {
    foreach($path in $Baseline,$Executable) {
        for($run=1;$run -le 3;$run++) {
            $stats=(Run-Game $path $scenario.Args)[0]
            "$($scenario.Name) $(Split-Path -Leaf $path) run $run. $stats"
        }
    }
    $baselineStats=(Run-Game $Baseline ($scenario.Args+@('--audio-hash')))[0]
    $nativeStats=(Run-Game $Executable ($scenario.Args+@('--audio-hash')))[0]
    $ignore='elapsed_ms=\d+[.]\d+ '
    if(($baselineStats -replace $ignore) -cne ($nativeStats -replace $ignore)) {
        throw "State or PCM differed after optimization for $($scenario.Name)"
    }
    "Visual memory, game state counters and PCM match. $nativeStats"
}

$early=@('--probe','400','--fire','90','140','--move','230','400','9')
$result=Run-Game $Executable $early
if((Read-Progress $result Ended) -notmatch '^frame=400 room=05 ') { throw 'Expected gameplay transition to room 05' }
$result

$created=Run-Game $Executable @('--probe','220','--fire','90','140','--profile',$testProfile,'--save-at','220')
$saved=Read-Progress $created Saved
$firstHash=(Get-FileHash -LiteralPath $profilePath).Hash
$resume=@('--probe','180','--load','--profile',$testProfile,'--move','230','400','9')
$loaded=Run-Game $Executable $resume
if((Read-Progress $loaded Loaded) -cne $saved){throw 'Restart did not restore original progress'}
if((Read-Progress $loaded Ended) -notmatch '^frame=400 room=05 '){throw 'Could not advance to room 05 after restart'}
$loaded
$cancel=Run-Game $Executable ($resume+@('--save-at','180','--cancel-overwrite'))
if('Canceled save overwrite' -notin $cancel){throw 'Cancellation not reported'}
if((Get-FileHash -LiteralPath $profilePath).Hash -ne $firstHash){throw 'Cancellation modified the save'}
$cancel
$replace=Run-Game $Executable ($resume+@('--save-at','180','--overwrite'))
$updated=Read-Progress $replace Saved
if((Get-FileHash -LiteralPath $profilePath).Hash -eq $firstHash){throw 'Overwrite did not replace the save'}
$reloaded=Run-Game $Executable @('--probe','30','--load','--profile',$testProfile)
if((Read-Progress $reloaded Loaded) -cne $updated){throw 'Restart did not restore the overwrite'}
$reloaded
"Passed save/create, restart/load, cancel unchanged, overwrite, and restart/load for $testProfile."
