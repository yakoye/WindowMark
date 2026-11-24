# 在当前用户交互会话安装待验证构建；不修改用户偏好，不启动额外远程连接。
param([string]$Output='C:/Users/color/Downloads/share_chatgpt/WindowMark-release-065-cache')
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$root='C:/Users/color/Downloads/yakoye-release/WindowMark'
Start-Transcript -Path "$Output/deploy-transcript.log" -Force|Out-Null
try {
    $old=@(Get-Process WindowMark -ErrorAction SilentlyContinue | Where-Object {!$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId})
    $quit=Start-Process "$root/WindowMark.exe" -ArgumentList '--quit' -WindowStyle Hidden -PassThru
    $quit.WaitForExit(2000)|Out-Null
    foreach($p in $old){if(!$p.WaitForExit(9000)){throw 'Existing application did not exit'}}
    $hash=(Get-FileHash "$root/WindowMark.exe" -Algorithm SHA256).Hash
    $backup="$root/history/$($hash.Substring(0,12).ToLower())"
    New-Item -ItemType Directory -Path $backup -Force|Out-Null
    Copy-Item "$root/WindowMark.exe" "$backup/WindowMark.exe" -Force
    Copy-Item "$Output/WindowMark.exe" "$root/WindowMark.exe" -Force
    if((Get-FileHash "$root/WindowMark.exe").Hash -ne (Get-FileHash "$Output/WindowMark.exe").Hash){throw 'Installed executable hash mismatch'}
    $shell=@((New-Object -ComObject Shell.Application).Windows())|Where-Object {$_.FullName -like '*explorer.exe'}|Select-Object -First 1
    if(!$shell){throw 'Persistent Explorer not found'}
    $shell.Document.Application.ShellExecute("$root/WindowMark.exe",'--ime-guard-enable',$root,'open',1)
    Start-Sleep -Seconds 3
    # 自有空白编辑框短暂前台，用于学习真实原生输入法宽度，无键鼠模拟和文字输入。
    $fixture=Start-Process "$Output/WindowMarkInputContextFixture.exe" -PassThru
    $fixture.WaitForExit(20000)|Out-Null
    for($i=1;$i -le 12;$i++) {
        Start-Sleep -Seconds 3
        $main=@(Get-Process WindowMark -ErrorAction SilentlyContinue|Where-Object {!$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId}|Sort-Object StartTime -Descending)[0]
        $controllers=@(Get-CimInstance Win32_Process|Where-Object {$_.Name -eq 'WindowMarkXamlContainerTrial.exe' -and $_.ParentProcessId -eq $main.Id})
        foreach($controller in $controllers){
            if($controller.CommandLine -match '--client-map "([^"]+)"'){
                & python "$Output/read-client-state.py" --map $Matches[1] --out "$Output/installed-state-$i.json"|Out-Null
                if($LASTEXITCODE -eq 0){
                    $state=Get-Content "$Output/installed-state-$i.json" -Raw|ConvertFrom-Json
                    if($state.state -eq 3 -and $state.width -gt 0 -and !$state.restore_failures){'PASS'|Set-Content "$Output/installed.txt";exit 0}
                }
            }
        }
    }
    throw 'Installed protection did not become active'
} catch {$_|Out-String|Set-Content "$Output/deploy-failed.txt" -Encoding UTF8;exit 1}
