param([string]$Output)
# 只读采集现有远程应用切换，不主动点击、输入或重新连接。
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
try {
    $main=@(Get-Process WindowMark -ErrorAction SilentlyContinue|Where-Object {!$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId}|Sort-Object StartTime -Descending)[0]
    if(!$main){throw 'Application is not running'}
    $controller=@(Get-CimInstance Win32_Process|Where-Object {$_.Name -eq 'WindowMarkXamlContainerTrial.exe' -and $_.ParentProcessId -eq $main.Id})[0]
    if($controller.CommandLine -notmatch '--client-map "([^"]+)"'){throw 'Client map is not available'}
    $map=$Matches[1]
    $probe=Start-Process "$Output/WindowMarkImeTrayProbe.exe" -ArgumentList "--out $Output/remote-switch.jsonl --samples 240 --interval-ms 500 --include-app-geometry" -WindowStyle Hidden -PassThru
    for($i=0;$i -lt 100;$i++){
        & python "$Output/read-client-state.py" --map $map --out "$Output/state-$i.json"|Out-Null
        if($LASTEXITCODE -ne 0){throw 'Client state could not be read'}
        $state=Get-Content "$Output/state-$i.json" -Raw|ConvertFrom-Json
        if($state.state -ne 3 -or $state.width -le 0 -or $state.restore_failures){throw 'Protection became inactive during capture'}
        if($probe.WaitForExit(5000)){break}
    }
    if(!$probe.HasExited -or $probe.ExitCode -ne 0){throw 'Read-only collection did not complete'}
    'PASS_CAPTURE_AND_ACTIVE_STATE'|Set-Content "$Output/completed.txt"
} catch {$_|Out-String|Set-Content "$Output/failed.txt" -Encoding UTF8;exit 1}
