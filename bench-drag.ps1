# 拖动流畅度实测。回答的是「被拖的那个窗口自己跟不跟手」，不是边框跟不跟手 —— 后者
# 之前测过，是两回事。
#
# 做法：用 SendInput 真按下标题栏、匀速拖 N 步、再松开。这样走的是窗口自己的模态拖动
# 循环，跟手动拖是同一条路径。每injected一步之后采一次样，记录光标和窗口的错位。
#
#   bench-drag.ps1                    自动挑一个窗口（优先 Excel/Word）
#   bench-drag.ps1 -Class XLMAIN      指定窗口类
#   bench-drag.ps1 -Steps 120         每轮拖 120 步
param(
    [string]$Class = '',
    # 真实拖动是连续的二维运动，不是匀速直线。按 8 字形走：x 一个来回的同时 y 走两个，
    # 两个轴一直在变。幅度和频率照实际手感来。
    # 想要的幅度（不是窗口宽度）。实际会被压到「窗口边缘离屏幕还有 EdgeMargin」为止：
    # 窗口撞到屏幕边会被钳住不动，而「位置不变」正是卡顿判据，撞边会被记成假卡顿。
    [int]$AmpX = 1400,
    [int]$AmpY = 1000,
    [int]$EdgeMargin = 100,
    [double]$Hz = 2,          # 每秒几个来回
    [double]$Seconds = 3,
    # 噪声很大：同一配置连测三轮，卡住次数可以在 0 和 5 之间跳。所以轮次要够，
    # 而且必须把每轮的散布打出来 —— 只看平均值会把噪声当成效果。
    [int]$Repeat = 7
)

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public class BD {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint f);
  [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr m, ref MONITORINFO mi);
  [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x,int y,int w,int ht,uint f);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int sz);

  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx,dy; public uint mouseData,dwFlags,time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk,wScan; public uint dwFlags,time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] public struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public UNION u; }

  public static void Aware(){ SetProcessDpiAwarenessContext(new IntPtr(-4)); }
  public static string Cls(IntPtr h){ var s=new StringBuilder(256); GetClassNameW(h,s,256); return s.ToString(); }
  public static string Txt(IntPtr h){ var s=new StringBuilder(512); GetWindowTextW(h,s,512); return s.ToString(); }
  public static List<IntPtr> Order(){ var l=new List<IntPtr>(); EnumWindows((h,p)=>{l.Add(h);return true;}, IntPtr.Zero); return l; }

  public static void MoveTo(int x, int y) {
    int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77);
    int vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
    var i = new INPUT[1];
    i[0].type = 0;
    i[0].u.mi.dx = (int)(((double)(x - vx)) * 65535.0 / (vw - 1));
    i[0].u.mi.dy = (int)(((double)(y - vy)) * 65535.0 / (vh - 1));
    i[0].u.mi.dwFlags = 0x0001 | 0x8000 | 0x4000;   // MOVE | ABSOLUTE | VIRTUALDESK
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Button(bool down) {
    var i = new INPUT[1];
    i[0].type = 0;
    i[0].u.mi.dwFlags = down ? 0x0002u : 0x0004u;   // LEFTDOWN | LEFTUP
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static bool Foreground(IntPtr h) {
    uint pid; uint tid = GetWindowThreadProcessId(GetForegroundWindow(), out pid);
    uint me = GetCurrentThreadId();
    AttachThreadInput(me, tid, true);
    bool ok = SetForegroundWindow(h);
    AttachThreadInput(me, tid, false);
    return ok;
  }
}
'@ -ErrorAction Stop
[BD]::Aware() | Out-Null

$conf = Join-Path $env:LOCALAPPDATA 'WindowMark\settings.conf'
$exe = Join-Path $env:LOCALAPPDATA 'Programs\WindowMark\WindowMark.exe'
$backup = Join-Path $env:TEMP 'wm-bench-settings.conf'
Copy-Item $conf $backup -Force

function Get-Rect($h) { $r = New-Object BD+RECT; [BD]::GetWindowRect($h, [ref]$r) | Out-Null; $r }

function Stop-WM {
    Get-Process -Name WindowMark -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 900
}
function Start-WM($drawer, $border) {
    (Get-Content $conf) -replace '^drawer\.enabled=.*', "drawer.enabled=$drawer" `
                        -replace '^border\.enabled=.*', "border.enabled=$border" |
        Set-Content $conf -Encoding UTF8
    if (-not (Select-String -Path $conf -Pattern '^drawer\.enabled=' -Quiet)) {
        Add-Content $conf "drawer.enabled=$drawer"
    }
    Start-Process $exe | Out-Null
    Start-Sleep -Seconds 4
}

# ---- 挑目标窗口 ----
$prefer = if ($Class) { @($Class) } else { @('XLMAIN', 'OpusApp', 'Chrome_WidgetWin_1', 'CabinetWClass') }
$target = [IntPtr]::Zero
foreach ($want in $prefer) {
    foreach ($h in [BD]::Order()) {
        if (-not [BD]::IsWindowVisible($h)) { continue }
        if ([BD]::Cls($h) -ne $want) { continue }
        if ([BD]::IsZoomed($h)) { continue }          # 最大化的窗口拖不动
        $r = Get-Rect $h
        if (($r.R - $r.L) -lt 500 -or $r.L -lt -30000) { continue }
        $target = $h; break
    }
    if ($target -ne [IntPtr]::Zero) { break }
}
if ($target -eq [IntPtr]::Zero) { Write-Host '没找到可拖动的目标窗口（都最大化了？）'; exit 1 }

$saved = Get-Rect $target
Write-Host ("目标: [{0}] {1}" -f [BD]::Cls($target), [BD]::Txt($target))
Write-Host ("  原始 rect ({0},{1}) {2}x{3}" -f $saved.L, $saved.T, ($saved.R - $saved.L), ($saved.B - $saved.T))

# 不改窗口大小 —— 尺寸会影响拖动成本，改小了测出来的就不是用户实际用的那个窗口。
# 只在窗口处于吸附/最大化状态时还原，因为那种状态下拖动不会立刻脱离，测出来的
# 「一动不动」是假数据（第一次测就栽在这里，连基线都显示卡了 80 次）。
if ([BD]::IsZoomed($target)) {
    Write-Host "  窗口是最大化状态，先还原（最大化窗口拖不动）"
    [BD]::ShowWindow($target, 9) | Out-Null    # SW_RESTORE
    Start-Sleep -Milliseconds 600
}
$origin = Get-Rect $target
Write-Host ("  测试用 rect ({0},{1}) {2}x{3}  （保持原样，不改尺寸）" -f `
    $origin.L, $origin.T, ($origin.R - $origin.L), ($origin.B - $origin.T))
Write-Host ""

# 标题栏上一个不会点到按钮的位置：右侧按钮左边留 260px，快速访问工具栏右边
$grabX = $origin.R - 260
$grabY = $origin.T + 15

# 把幅度压到「窗口四边离屏幕工作区还有 EdgeMargin」为止。抓取点在窗口内的相对位置决定了
# 光标能走多远：窗口左边 = 光标 x - offsetX，右边 = 左边 + 窗口宽。
$mon = [BD]::MonitorFromWindow($target, 2)
$mi = New-Object BD+MONITORINFO
$mi.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($mi)
[BD]::GetMonitorInfoW($mon, [ref]$mi) | Out-Null
$work = $mi.rcWork
$winW = $origin.R - $origin.L
$winH = $origin.B - $origin.T
$offX = $grabX - $origin.L
$offY = $grabY - $origin.T
# 光标 x 的允许范围，反推自「窗口左边 >= work.L+margin」和「窗口右边 <= work.R-margin」
$minCx = $work.L + $EdgeMargin + $offX
$maxCx = $work.R - $EdgeMargin - ($winW - $offX)
$minCy = $work.T + $EdgeMargin + $offY
$maxCy = $work.B - $EdgeMargin - ($winH - $offY)
$roomX = [math]::Max(0, [math]::Min($grabX - $minCx, $maxCx - $grabX))
$roomY = [math]::Max(0, [math]::Min($grabY - $minCy, $maxCy - $grabY))
$ampX = [math]::Min($AmpX, $roomX)
$ampY = [math]::Min($AmpY, $roomY)
Write-Host ("  工作区 ({0},{1})-({2},{3})  窗口 {4}x{5}" -f $work.L, $work.T, $work.R, $work.B, $winW, $winH)
Write-Host ("  幅度：想要 ±{0}/±{1}，受边界 {2}px 限制后实际 ±{3}/±{4}" -f $AmpX, $AmpY, $EdgeMargin, $ampX, $ampY)
if ($ampX -lt 100 -or $ampY -lt 60) {
    Write-Host "  窗口太大，留不出行程。把它调小一点再测。" -ForegroundColor Yellow
    exit 1
}
Write-Host ""

function Run-Drag($label) {
    # 机器忙的时候拖动明显更卡（用户观察到的，我自己编译时最明显），所以先等 CPU 静下来，
    # 否则测的是我自己的构建在抢时间片。
    for ($w = 0; $w -lt 20; $w++) {
        $busy = (Get-Counter '\Processor(_Total)\% Processor Time' -ErrorAction SilentlyContinue).CounterSamples[0].CookedValue
        if ($null -eq $busy -or $busy -lt 25) { break }
        Start-Sleep -Milliseconds 500
    }

    # 每轮都从同一个位置开始，避免窗口越拖越偏
    [BD]::SetWindowPos($target, [IntPtr]::Zero, $origin.L, $origin.T, 0, 0, 0x0015) | Out-Null
    Start-Sleep -Milliseconds 400
    [BD]::Foreground($target) | Out-Null
    Start-Sleep -Milliseconds 600

    $wm = Get-Process -Name WindowMark -ErrorAction SilentlyContinue
    $cpu0 = if ($wm) { $wm.TotalProcessorTime } else { [TimeSpan]::Zero }

    [BD]::MoveTo($grabX, $grabY)
    Start-Sleep -Milliseconds 150
    [BD]::Button($true)
    Start-Sleep -Milliseconds 150

    $before = Get-Rect $target
    $offset = $grabX - $before.L    # 抓取点相对窗口左上角的偏移
    $offsetY = $grabY - $before.T

    $lags = @()
    $stalls = 0
    $steps = 0
    $prevL = $before.L
    $prevT = $before.T
    $moved = $false
    $ampX = $AmpX
    $ampY = $AmpY
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        $t = $sw.Elapsed.TotalSeconds
        # 8 字形：x 走一个来回的时间里 y 走两个，两个轴始终在动
        $x = [int]($grabX + $ampX * [math]::Sin(2 * [math]::PI * $Hz * $t))
        $y = [int]($grabY + $ampY * [math]::Sin(4 * [math]::PI * $Hz * $t))
        [BD]::MoveTo($x, $y)
        $steps++
        Start-Sleep -Milliseconds 8
        $r = Get-Rect $target
        # 窗口应该跟到 (x,y) - 抓取偏移；两轴的偏差取较大的那个
        $lags += [math]::Max([math]::Abs(($r.L + $offset) - $x),
                             [math]::Abs(($r.T + $offsetY) - $y))
        if ($r.L -eq $prevL -and $r.T -eq $prevT) { $stalls++ } else { $moved = $true }
        $prevL = $r.L
        $prevT = $r.T
    }
    $sw.Stop()

    [BD]::Button($false)
    Start-Sleep -Milliseconds 300

    # 一步都没动过 = 拖动压根没起来（按到了按钮、窗口被吸附、前台没抢到）。
    # 这种结果不能当数据用，必须标出来，否则会被当成「卡到极致」。
    if (-not $moved) {
        return [pscustomobject]@{
            配置 = $label; 完美跟随 = '-'; 平均滞后 = '-'; 最大滞后 = '-'
            卡住次数 = '-'; 耗时 = '-'; WM占用CPU = '-'; 备注 = '拖动没生效，数据无效'
        }
    }

    $wm = Get-Process -Name WindowMark -ErrorAction SilentlyContinue
    $cpuMs = if ($wm) { ($wm.TotalProcessorTime - $cpu0).TotalMilliseconds } else { 0 }

    $mean = [math]::Round((($lags | Measure-Object -Average).Average), 1)
    $max = ($lags | Measure-Object -Maximum).Maximum
    # 采样次数每轮不完全一样，所以卡顿按比例算才好比
    $stallPct = if ($steps -gt 0) { 100.0 * $stalls / $steps } else { 0 }

    [pscustomobject]@{
        配置 = $label
        采样 = $steps
        卡住次数 = $stalls
        卡住占比 = $stallPct
        平均滞后 = "${mean}px"
        最大滞后 = "${max}px"
        WM占用CPU = "{0:N0}ms" -f $cpuMs
        备注 = ''
    }
}

function Measure-Config($label, $drawer, $border, $index, $total) {
    Write-Host "=== $index/$total  $label ==="
    Stop-WM
    if ($drawer -ne $null) { Start-WM $drawer $border }

    $rows = @()
    for ($r = 1; $r -le $Repeat; $r++) {
        $one = Run-Drag $label
        if ($one.备注 -ne '') {
            # 拖动没起来时重试一次：第一轮常常因为窗口刚被移动/前台刚切换而失手
            Write-Host "    第 $r 轮没拖起来，重试"
            $one = Run-Drag $label
        }
        if ($one.备注 -eq '') { $rows += $one }
        Write-Host ("    第 {0} 轮: 采样 {1}  卡住 {2} ({3:N1}%)  平均滞后 {4}  CPU {5}" -f `
            $r, $one.采样, $one.卡住次数, $one.卡住占比, $one.平均滞后, $one.WM占用CPU)
    }
    if ($rows.Count -eq 0) {
        return [pscustomobject]@{ 配置 = $label; 有效轮次 = 0 }
    }
    $pctList = $rows | ForEach-Object { [double]$_.卡住占比 } | Sort-Object
    $means = ($rows | ForEach-Object { [double]($_.平均滞后 -replace 'px', '') } | Measure-Object -Average).Average
    $maxes = ($rows | ForEach-Object { [double]($_.最大滞后 -replace 'px', '') } | Measure-Object -Maximum).Maximum
    $cpuList = $rows | ForEach-Object { [double]($_.WM占用CPU -replace 'ms', '' -replace ',', '') } | Sort-Object
    return [pscustomobject]@{
        配置 = $label
        有效轮次 = "$($rows.Count)/$Repeat"
        卡住中位 = "{0:N1}%" -f $pctList[[int]($pctList.Count / 2)]
        卡住散布 = "{0:N1}~{1:N1}%" -f $pctList[0], $pctList[-1]
        平均滞后 = "{0:N1}px" -f $means
        最大滞后 = "{0:N0}px" -f $maxes
        CPU中位 = "{0:N0}ms" -f $cpuList[[int]($cpuList.Count / 2)]
    }
}

$results = @()
$results += Measure-Config '书签+边框'   'true'  'true'  1 4
$results += Measure-Config '只边框'      'false' 'true'  2 4
$results += Measure-Config '只书签'      'true'  'false' 3 4
$results += Measure-Config '未运行(基线)' $null   $null   4 4

# 还原
Copy-Item $backup $conf -Force
Start-Process $exe | Out-Null
Start-Sleep -Seconds 2
# 只还位置，不动尺寸 —— 尺寸从头到尾就没改过
[BD]::SetWindowPos($target, [IntPtr]::Zero, $saved.L, $saved.T, 0, 0, 0x0015) | Out-Null

Write-Host ""
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Output
Write-Host "窗口位置和配置都已还原。"
Write-Host ("轨迹：8 字形，X 幅度 ±{0}px、Y 幅度 ±{1}px，每秒 {2} 个来回，持续 {3}s。" -f $AmpX, $AmpY, $Hz, $Seconds)
Write-Host "「卡住」= 送了鼠标移动但窗口两个轴都没动的采样占比，最接近肉眼看到的顿挫。"
