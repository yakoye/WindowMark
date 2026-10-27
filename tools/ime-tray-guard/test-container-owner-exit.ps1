# Force-terminate ONLY our controller for our own offscreen fixture, never Explorer.
param([Parameter(Mandatory=$true)][string]$BuildDirectory,[Parameter(Mandatory=$true)][string]$EvidenceDirectory,[switch]$NormalExit)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $EvidenceDirectory){throw 'Evidence directory exists'}
$null=New-Item -ItemType Directory -Path $EvidenceDirectory
$fixture=$null;$controller=$null
try {
    $fixture=Start-Process -FilePath (Join-Path $BuildDirectory 'WindowMarkXamlContainerFixture.exe') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $EvidenceDirectory 'fixture.jsonl') -RedirectStandardError (Join-Path $EvidenceDirectory 'fixture.stderr.log')
    $null=$fixture.Handle
    $watch=[Diagnostics.Stopwatch]::StartNew()
    while((Get-Item (Join-Path $EvidenceDirectory 'fixture.jsonl')).Length -eq 0 -and $watch.ElapsedMilliseconds -lt 3000){$null=$fixture.WaitForExit(50)}
    $hold=if($NormalExit){12}else{16}
    $args='--fixture-pid {0} --reserve --seconds {1} --out "{2}"' -f $fixture.Id,$hold,(Join-Path $EvidenceDirectory 'trial.jsonl')
    $controller=Start-Process -FilePath (Join-Path $BuildDirectory 'WindowMarkXamlContainerTrial.exe') -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $EvidenceDirectory 'trial.stdout.log') -RedirectStandardError (Join-Path $EvidenceDirectory 'trial.stderr.log')
    $null=$controller.Handle
    $reserved=$false
    while($watch.Elapsed.TotalSeconds -lt 11) {
        $null=$controller.WaitForExit(100)
        if($controller.HasExited){throw 'Controller exited before forced-stop test'}
        $rows=@(Get-Content (Join-Path $EvidenceDirectory 'fixture.jsonl') | ForEach-Object {if($_.EndsWith('}')){$_|ConvertFrom-Json}})
        if($rows | Where-Object {$_.second -ge 7 -and $_.second -le 10 -and $_.width -eq 76 -and $_.min_width -eq 76 -and -not $_.content_visible}){$reserved=$true;break}
    }
    if(-not $reserved){throw 'No real reservation observed before killing controller'}
    if($NormalExit) {
        if(-not $controller.WaitForExit(10000) -or $controller.ExitCode -ne 0){throw 'Normal controller exit failed'}
        $final=Get-Content (Join-Path $EvidenceDirectory 'trial.jsonl') -Tail 1 | ConvertFrom-Json
        if($final.state -ne 5 -or $final.writes -ne 1 -or $final.restores -ne 1 -or $final.restore_failures -ne 0 -or $final.unsubscribe_hr -ne 0){throw 'Normal cleanup evidence incomplete'}
    } else { $controller.Kill();$null=$controller.WaitForExit(3000) }
    if(-not $fixture.WaitForExit(15000)){throw 'Fixture failed to exit'}
    if($fixture.ExitCode -ne 0){throw 'Fixture crashed'}
    $rows=@(Get-Content (Join-Path $EvidenceDirectory 'fixture.jsonl') | ForEach-Object {$_|ConvertFrom-Json})
    if(-not ($rows | Where-Object {$_.second -ge 9 -and $_.second -lt 15 -and $_.width -eq 0 -and $_.min_width -eq 0 -and $_.local_unset})){throw 'Owner death did not restore original unset width'}
    if(-not ($rows | Where-Object {$_.second -ge 16 -and $_.width -eq 76 -and $_.min_width -eq 0 -and $_.local_unset -and $_.content_visible})){throw 'Original content failed to recover'}
    Write-Output "PASS: reserve 76 DIP while hidden; normal_exit=$NormalExit; fixture restores unset/0; original content returns at 76 DIP."
} finally {
    if($null -ne $controller -and -not $controller.HasExited){$controller.Kill();$null=$controller.WaitForExit(3000)}
    if($null -ne $fixture -and -not $fixture.HasExited){$null=$fixture.WaitForExit(25000)}
}
