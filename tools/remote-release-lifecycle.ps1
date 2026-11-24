param([string]$Output='C:/Users/color/Downloads/share_chatgpt/WindowMark-release-065-validation')
# 0.6.5 发布门禁：在真实交互会话重启主程序与任务栏 Explorer，保留其他应用。
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$root='C:/Users/color/Downloads/yakoye-release/WindowMark'
$exe="$root/WindowMark.exe"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TaskbarOwner {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
}
'@
function Get-TaskbarPid {
    $window=[TaskbarOwner]::FindWindow('Shell_TrayWnd',$null)
    if($window -eq [IntPtr]::Zero){return 0}
    [uint32]$owner=0;[TaskbarOwner]::GetWindowThreadProcessId($window,[ref]$owner)|Out-Null
    return $owner
}
function Get-Main {
    return @(Get-Process WindowMark -ErrorAction SilentlyContinue | Where-Object {!$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId} | Sort-Object StartTime -Descending)[0]
}
function Read-State($phase,$index) {
    $main=Get-Main
    if(!$main){return $null}
    $controllers=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'WindowMarkXamlContainerTrial.exe' -and $_.ParentProcessId -eq $main.Id})
    foreach($controller in $controllers) {
        if($controller.CommandLine -match '--client-map "([^"]+)"') {
            & python "$output/read-client-state.py" --map $Matches[1] --out "$output/$phase-$index.json" | Out-Null
            if($LASTEXITCODE -eq 0){return (Get-Content "$output/$phase-$index.json" -Raw | ConvertFrom-Json)}
        }
    }
    return $null
}
function Wait-Active($phase,$target) {
    for($i=1;$i -le 40;$i++) {
        Start-Sleep -Seconds 3
        $state=Read-State $phase $i
        if($state -and $state.state -eq 3 -and $state.width -gt 0 -and !$state.restore_failures -and (!$target -or $state.target_pid -eq $target)) {return $state}
    }
    throw "Protection did not recover: $phase"
}
try {
    $before=Wait-Active 'before' 0
    $main=Get-Main
    $shell=@((New-Object -ComObject Shell.Application).Windows())|Where-Object {$_.FullName -like '*explorer.exe'}|Select-Object -First 1
    if(!$shell){throw 'Persistent shell not available'}
    $quit=Start-Process $exe -ArgumentList '--quit' -PassThru -WindowStyle Hidden;$quit.WaitForExit(2000)|Out-Null
    if(!$main.WaitForExit(9000)){throw 'Normal exit timed out'}
    if(Test-Path "$root/ImeTrayGuard/active-session.lock"){throw 'Restoration marker was not removed'}
    $shell.Document.Application.ShellExecute($exe,'',$root,'open',1)
    $restart=Wait-Active 'program-restart' 0
    [pscustomobject]@{passed=$true;before=$before;after=$restart}|ConvertTo-Json -Depth 4|Set-Content "$output/program-result.json" -Encoding UTF8
    $oldPid=Get-TaskbarPid
    if(!$oldPid -or (Get-Process -Id $oldPid).ProcessName -ne 'explorer'){throw 'Taskbar process identity not confirmed'}
    $old=Get-Process -Id $oldPid
    Stop-Process -Id $oldPid -Force
    $old.WaitForExit(8000)|Out-Null
    $newPid=0
    for($i=0;$i -lt 15;$i++){Start-Sleep -Seconds 1;$newPid=Get-TaskbarPid;if($newPid -and $newPid -ne $oldPid){break}}
    if(!$newPid -or $newPid -eq $oldPid) {
        Start-Process 'C:/Windows/explorer.exe' -WindowStyle Hidden|Out-Null
        for($i=0;$i -lt 15;$i++){Start-Sleep -Seconds 1;$newPid=Get-TaskbarPid;if($newPid -and $newPid -ne $oldPid){break}}
    }
    if(!$newPid -or $newPid -eq $oldPid){throw 'Taskbar did not return'}
    $explorer=Wait-Active 'explorer-restart' $newPid
    [pscustomobject]@{passed=$true;oldExplorer=$oldPid;newExplorer=$newPid;state=$explorer}|ConvertTo-Json -Depth 4|Set-Content "$output/explorer-result.json" -Encoding UTF8
    $probe=Start-Process "$output/WindowMarkImeTrayProbe.exe" -ArgumentList "--out $output/remote-switch.jsonl --samples 180 --interval-ms 500 --include-app-geometry" -WindowStyle Hidden -PassThru
    $probe.Id|Set-Content "$output/probe-pid.txt"
    # 自有测试窗口关闭后自然返回先前的远程应用；不向远程窗口发送输入。
    'READY_FOR_REMOTE_SWITCH'|Set-Content "$output/switch-ready.txt"
    foreach($round in 1..2){
        $fixture=Start-Process "$output/WindowMarkInputContextFixture.exe" -PassThru
        $fixture.WaitForExit(20000)|Out-Null
        Start-Sleep -Seconds 7
    }
    if(!$probe.WaitForExit(150000)){throw 'Read-only probe timed out'}
    if($probe.ExitCode -ne 0){throw 'Read-only probe failed'}
    $stable=Wait-Active 'after-switch' 0
    $stable|ConvertTo-Json|Set-Content "$output/after-switch-state.json" -Encoding UTF8
    'PASS_LIFECYCLE_AND_CAPTURE_COMPLETE'|Set-Content "$output/completed.txt"
} catch {$_|Out-String|Set-Content "$output/failed.txt" -Encoding UTF8;exit 1}
