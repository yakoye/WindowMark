# 交互会话中复现旧标记和主程序异常退出；只结束当前测试的 WindowMark，不结束 Explorer/控制程序。
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$root='C:/Users/color/Downloads/yakoye-release/WindowMark'
$output='C:/Users/color/Downloads/share_chatgpt/WindowMark-ime-auto-20261124'
$exe="$root/WindowMark.exe"
$shell=$null
function Start-Normal {
    $script:shell.Document.Application.ShellExecute($exe,'',$root,'open',1)
    Start-Sleep -Seconds 1
    return @(Get-Process WindowMark | Where-Object {!$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId} | Sort-Object StartTime -Descending)[0]
}
function Wait-Active($process,$phase) {
    for($i=1;$i -le 15;$i++) {
        Start-Sleep -Seconds 3
        $controller=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'WindowMarkXamlContainerTrial.exe' -and $_.ParentProcessId -eq $process.Id} | Select-Object -First 1)
        if(!$controller.Count){continue}
        $controller=$controller[0]
        $controller | Select-Object ProcessId,ParentProcessId,CommandLine | ConvertTo-Json | Set-Content "$output/$phase-controller-$i.json" -Encoding UTF8
        if($controller.CommandLine -match '--client-map "([^"]+)"') {
            & python "$output/read-client-state.py" --map $Matches[1] --out "$output/$phase-state-$i.json" | Out-Null
            if($LASTEXITCODE -ne 0){continue}
            $state=Get-Content "$output/$phase-state-$i.json" -Raw | ConvertFrom-Json
            if($state.state -eq 3 -and $state.width -gt 0 -and !$state.restore_failures) {
                return [pscustomobject]@{state=$state;reuseOnly=$controller.CommandLine.Contains('--reuse-only');pid=$process.Id}
            }
        }
    }
    throw "No active protection during $phase"
}
try {
    $script:shell=@((New-Object -ComObject Shell.Application).Windows()) | Where-Object {$_.FullName -like '*explorer.exe'} | Select-Object -First 1
    if(!$script:shell){throw 'Persistent Explorer shell not available'}
    $existing=@(Get-Process WindowMark -ErrorAction SilentlyContinue | Where-Object {!$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId})
    $quit=Start-Process $exe -ArgumentList '--quit' -PassThru -WindowStyle Hidden;$quit.WaitForExit(2000)|Out-Null
    foreach($item in $existing){if(!$item.WaitForExit(9000)){throw 'Normal exit failed'}}
    if(Test-Path "$root/ImeTrayGuard/active-session.lock"){throw 'Previous layout restoration not confirmed'}
    # 使用此前真实故障留下的原始标记，而不是在 Explorer 中制造恢复失败。
    Copy-Item "$output/marker-before.txt" "$root/ImeTrayGuard/active-session.lock"
    Get-Content "$root/ImeTrayGuard/active-session.lock" -Raw | Set-Content "$output/marker-staged.txt"
    $first=Start-Normal
    $stale=Wait-Active $first 'stale-marker'
    $stale | ConvertTo-Json -Depth 4 | Set-Content "$output/stale-marker-result.json" -Encoding UTF8
    Copy-Item "$root/ImeTrayGuard/last-recovery.txt" "$output/stale-marker-recovery-trace.txt"
    if($first.Path -ne $exe.Replace('/','\')){throw 'Unexpected process path, refusing crash test'}
    Stop-Process -Id $first.Id -Force
    if(!(Test-Path "$root/ImeTrayGuard/active-session.lock")){throw 'Crash did not leave a marker; test condition absent'}
    Copy-Item "$root/ImeTrayGuard/active-session.lock" "$output/crash-marker.txt"
    Start-Sleep -Seconds 2
    $second=Start-Normal
    $crash=Wait-Active $second 'after-crash'
    if(!$crash.reuseOnly){throw 'Expected reuse-only recovery after abnormal exit'}
    $crash | ConvertTo-Json -Depth 4 | Set-Content "$output/crash-result.json" -Encoding UTF8
    Start-Sleep -Seconds 4
    $stable=Wait-Active $second 'stable'
    if($stable.state.target_heartbeat -le $crash.state.target_heartbeat){throw 'Heartbeat did not advance'}
    $stable | ConvertTo-Json -Depth 4 | Set-Content "$output/stable-result.json" -Encoding UTF8
    Get-CimInstance Win32_Process | Where-Object {$_.ProcessId -eq $second.Id} |
        Select-Object ProcessId,ParentProcessId,CommandLine | ConvertTo-Json | Set-Content "$output/final-process.json" -Encoding UTF8
    'PASS' | Set-Content "$output/completed.txt"
} catch {$_ | Out-String | Set-Content "$output/failed.txt" -Encoding UTF8;exit 1}
