# 在目标用户的交互会话运行。只用公开 CLI 启动/退出，读取窗口响应；不模拟鼠标或截图。
$ErrorActionPreference = 'Stop'
$root = 'C:/Users/color/Downloads/yakoye-release/WindowMark'
$output = 'C:/Users/color/Downloads/share_chatgpt/WindowMark-debug-065'
$exe = Join-Path $root 'WindowMark.exe'
$results = @()
try {
    # 若已有正常运行的同目录实例，先通过公开 CLI 正常退出；不强杀、不重启 Explorer。
    $existing = @(Get-Process WindowMark -ErrorAction SilentlyContinue | Where-Object { !$_.HasExited -and $_.SessionId -eq (Get-Process -Id $PID).SessionId })
    if ($existing.Count) {
        $quitExisting = Start-Process -FilePath $exe -ArgumentList '--quit' -PassThru -WindowStyle Hidden
        $quitExisting.WaitForExit(2000) | Out-Null
        foreach ($running in $existing) { if (!$running.WaitForExit(6000)) { throw 'Existing WindowMark did not exit normally' } }
    }
    for ($cycle = 1; $cycle -le 2; $cycle++) {
        $process = Start-Process -FilePath $exe -ArgumentList '--panel' -PassThru
        Start-Sleep -Seconds 3
        $report = & python "$output/diagnose-windowmark-hang.py" $process.Id
        $report | Set-Content "$output/cycle-$cycle-windows.json" -Encoding UTF8
        $windows = ($report | ConvertFrom-Json).windows
        $panel = @($windows | Where-Object { $_.cls -eq 'WindowMark.HomePanel' })
        $healthy = $panel.Count -gt 0 -and @($windows | Where-Object { !$_.responsive }).Count -eq 0
        $quit = Start-Process -FilePath $exe -ArgumentList '--quit' -PassThru -WindowStyle Hidden
        $quit.WaitForExit(2000) | Out-Null
        $exited = $process.WaitForExit(6000)
        $results += [pscustomobject]@{cycle=$cycle;pid=$process.Id;session=(Get-Process -Id $PID).SessionId;panelResponsive=$healthy;normalExit=$exited}
        $results | ConvertTo-Json | Set-Content "$output/smoke-results.json" -Encoding UTF8
        if (!$healthy -or !$exited) { throw "WindowMark smoke failed in cycle $cycle" }
    }
    $shell = @((New-Object -ComObject Shell.Application).Windows()) |
        Where-Object { $_.FullName -like '*explorer.exe' } | Select-Object -First 1
    if (!$shell) { throw 'No persistent Explorer shell available for relaunch' }
    $shell.Document.Application.ShellExecute($exe, '', $root, 'open', 1)
    Start-Sleep -Seconds 3
    Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'WindowMark.exe' -and !(Get-Process -Id $_.ProcessId).HasExited} |
        Select-Object ProcessId,ParentProcessId,ExecutablePath,CommandLine |
        ConvertTo-Json | Set-Content "$output/final-process.json" -Encoding UTF8
    'PASS' | Set-Content "$output/completed.txt"
} catch {
    $_ | Out-String | Set-Content "$output/failed.txt" -Encoding UTF8
    exit 1
}
