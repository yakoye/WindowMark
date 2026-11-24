# 在交互会话验证实际心跳、正常退出及不带启用参数重开。
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$output='C:/Users/color/Downloads/share_chatgpt/WindowMark-ime-check-20261124'
$root='C:/Users/color/Downloads/yakoye-release/WindowMark'
function Read-State($name) {
    $controllers=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'WindowMarkXamlContainerTrial.exe' -and $_.SessionId -eq (Get-Process -Id $PID).SessionId})
    foreach($controller in $controllers) {
        if($controller.CommandLine -match '--client-map "([^"]+)"') {
            & python "$output/read-client-state.py" --map $Matches[1] --out "$output/$name.json" |
                Set-Content "$output/$name.stdout.txt" -Encoding UTF8
            return (Get-Content "$output/$name.json" -Raw | ConvertFrom-Json)
        }
    }
    return $null
}
try {
    $first=Read-State 'active-before-restart'
    Start-Sleep -Seconds 4
    $second=Read-State 'active-before-restart-second'
    if(!$first -or !$second -or $first.state -ne 3 -or $second.state -ne 3 -or $second.target_heartbeat -le $first.target_heartbeat) {
        throw 'Protection not confirmed active with a fresh heartbeat'
    }
    $running=Get-Process -Id $second.owner_pid
    $quit=Start-Process "$root/WindowMark.exe" -ArgumentList '--quit' -PassThru -WindowStyle Hidden
    $quit.WaitForExit(2000) | Out-Null
    if(!$running.WaitForExit(8000)){throw 'WindowMark did not exit normally'}
    $markerRemoved=!(Test-Path "$root/ImeTrayGuard/active-session.lock")
    [pscustomobject]@{normalExit=$true;markerRemoved=$markerRemoved} | ConvertTo-Json | Set-Content "$output/normal-exit.json" -Encoding UTF8
    if(!$markerRemoved){throw 'Normal exit did not confirm restoration'}
    $shell=@((New-Object -ComObject Shell.Application).Windows()) | Where-Object {$_.FullName -like '*explorer.exe'} | Select-Object -First 1
    if(!$shell){throw 'No persistent Explorer shell available'}
    $shell.Document.Application.ShellExecute("$root/WindowMark.exe",'',$root,'open',1)
    $active=$false
    for($i=1;$i -le 8;$i++) {
        Start-Sleep -Seconds 4
        $state=Read-State "restart-$i"
        if($state -and $state.state -eq 3 -and $state.width -gt 0 -and !$state.restore_failures){$active=$true;break}
    }
    Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'WindowMark.exe' -and !(Get-Process -Id $_.ProcessId).HasExited} |
        Select-Object ProcessId,ParentProcessId,CommandLine | ConvertTo-Json | Set-Content "$output/final-process.json" -Encoding UTF8
    [pscustomobject]@{normalRestartActive=$active;state=$state;markerRemovedOnExit=$markerRemoved} |
        ConvertTo-Json -Depth 4 | Set-Content "$output/verify-result.json" -Encoding UTF8
} catch {$_ | Out-String | Set-Content "$output/verify-failed.txt" -Encoding UTF8;exit 1}
