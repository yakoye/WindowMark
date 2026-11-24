# 用户已要求启用保护；以公开 CLI 重新启用，保留旧诊断标记备份，不重启 Explorer。
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$output='C:/Users/color/Downloads/share_chatgpt/WindowMark-ime-check-20261124'
$root='C:/Users/color/Downloads/yakoye-release/WindowMark'
try {
    & python "$output/check-ime-broker.py" | Set-Content "$output/broker-before.json" -Encoding UTF8
    $existing=@(Get-Process WindowMark -ErrorAction SilentlyContinue | Where-Object {!$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId})
    $quit=Start-Process "$root/WindowMark.exe" -ArgumentList '--quit' -PassThru -WindowStyle Hidden
    $quit.WaitForExit(2000) | Out-Null
    foreach($process in $existing){if(!$process.WaitForExit(8000)){throw 'Existing WindowMark did not exit normally'}}
    $shell=@((New-Object -ComObject Shell.Application).Windows()) | Where-Object {$_.FullName -like '*explorer.exe'} | Select-Object -First 1
    if(!$shell){throw 'No persistent Explorer shell available'}
    $shell.Document.Application.ShellExecute("$root/WindowMark.exe",'--ime-guard-enable',$root,'open',1)
    for($i=1;$i -le 4;$i++) {
        Start-Sleep -Seconds 4
        $controllers=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'WindowMarkXamlContainerTrial.exe' -and $_.SessionId -eq (Get-Process -Id $PID).SessionId})
        $controllers | Select-Object ProcessId,ParentProcessId,CommandLine | ConvertTo-Json | Set-Content "$output/controllers-$i.json" -Encoding UTF8
        foreach($controller in $controllers) {
            if($controller.CommandLine -match '--client-map "([^"]+)"') {
                & python "$output/read-client-state.py" --map $Matches[1] --out "$output/client-$i.json" |
                    Set-Content "$output/client-$i.stdout.txt" -Encoding UTF8
            }
        }
    }
    & python "$output/check-ime-broker.py" | Set-Content "$output/broker-after.json" -Encoding UTF8
    'DONE' | Set-Content "$output/completed.txt"
} catch {$_ | Out-String | Set-Content "$output/failed.txt" -Encoding UTF8;exit 1}
