# 抓「实际被 WindowMark 画了边框」的窗口。
#
# 不复刻 C++ 里的过滤规则——那样脚本和代码会走偏。这里反查真实存在的
# WindowMark.WindowBorder 窗口，看它围着的是谁。
#
# 匹配方式很重要：边框矩形一定等于目标窗口的 DWM 扩展边界向外扩 reach 个像素
# （reach = border.width + border.offset）。所以按【尺寸差】匹配，不能只按中心距离——
# 按中心匹配会把一个 2560x1400 的最大化窗口错认成 2560x1440 的输入法窗口，
# 两者中心只差 20px。这个坑踩过一次，报出来的类名是个早已排除的类。
#
#   list-tracked.ps1              盯 30 秒（默认）
#   list-tracked.ps1 -Watch 60    盯 60 秒
#   list-tracked.ps1 -Watch 0     只抓当前这一瞬
param(
    [int]$Watch = 30
)

Add-Type -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public class LT {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);
  // 边框窗口是 WS_EX_TRANSPARENT + WS_DISABLED，命中测试会跳过它，
  // 所以在边框中心取点拿到的就是被它围住的那个窗口。尺寸对不上时用这个兜底：
  // 候选条一边打字一边变宽，边框位置会滞后几十像素，按尺寸匹配必然失败。
  public static IntPtr RootAt(int x, int y){
    POINT p; p.X = x; p.Y = y;
    IntPtr h = WindowFromPoint(p);
    if (h == IntPtr.Zero) return IntPtr.Zero;
    IntPtr root = GetAncestor(h, 2);
    return root == IntPtr.Zero ? h : root;
  }
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int sz);
  public static void Aware(){ SetProcessDpiAwarenessContext(new IntPtr(-4)); }
  public static string Cls(IntPtr h){ var s=new StringBuilder(256); GetClassNameW(h,s,256); return s.ToString(); }
  public static string Txt(IntPtr h){ var s=new StringBuilder(512); GetWindowTextW(h,s,512); return s.ToString(); }
  public static List<IntPtr> Order(){ var l=new List<IntPtr>(); EnumWindows((h,p)=>{l.Add(h);return true;}, IntPtr.Zero); return l; }
  // DWM 扩展边界，失败时退回 GetWindowRect —— 与 WinUtil.cpp 的 ExtendedFrame 一致
  public static RECT Frame(IntPtr h){
    RECT r;
    if (DwmGetWindowAttribute(h, 9, out r, 16) == 0) return r;
    GetWindowRect(h, out r);
    return r;
  }
}
'@ -ErrorAction Stop
[LT]::Aware() | Out-Null

# reach = max(0, max(1,width) + offset)，与 WinBorderBackend::Reach() 一致
$conf = Join-Path $env:LOCALAPPDATA 'WindowMark\settings.conf'
$bw = 4; $bo = -1
if (Test-Path $conf) {
    foreach ($line in (Get-Content $conf)) {
        if ($line -match '^border\.width=(-?\d+)') { $bw = [int]$Matches[1] }
        if ($line -match '^border\.offset=(-?\d+)') { $bo = [int]$Matches[1] }
    }
}
$reach = [Math]::Max(0, [Math]::Max(1, $bw) + $bo)
Write-Host "border.width=$bw  border.offset=$bo  ->  边框比窗口每边外扩 $reach px"

$procCache = @{}
function ProcName($h) {
    $procId = 0; [LT]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
    if (-not $procCache.ContainsKey($procId)) {
        $procCache[$procId] = try { (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { '?' }
    }
    return $procCache[$procId]
}

function Sample {
    $all = [LT]::Order()
    $borders = @()
    $wins = @()
    foreach ($h in $all) {
        $cls = [LT]::Cls($h)
        if ($cls -eq 'WindowMark.WindowBorder') {
            if (-not [LT]::IsWindowVisible($h)) { continue }
            $r = New-Object LT+RECT
            [LT]::GetWindowRect($h, [ref]$r) | Out-Null
            $borders += [pscustomobject]@{ R = $r }
            continue
        }
        if ([LT]::IsWindowVisible($h) -eq $false) { continue }
        $f = [LT]::Frame($h)
        if (($f.R - $f.L) -le 1) { continue }
        $wins += [pscustomobject]@{ H = $h; F = $f; Cls = $cls }
    }

    $hit = @()
    $miss = @()
    foreach ($b in $borders) {
        # 边框应当是 frame 每边外扩 reach，所以反推出的 frame 是边框内缩 reach
        $wantL = $b.R.L + $reach; $wantT = $b.R.T + $reach
        $wantR = $b.R.R - $reach; $wantB = $b.R.B - $reach
        $best = $null; $bestErr = 5   # 四条边总误差不超过 5px 才算匹配
        foreach ($w in $wins) {
            $err = [math]::Abs($w.F.L - $wantL) + [math]::Abs($w.F.T - $wantT) +
                   [math]::Abs($w.F.R - $wantR) + [math]::Abs($w.F.B - $wantB)
            if ($err -lt $bestErr) { $bestErr = $err; $best = $w }
        }
        if ($best) {
            $hit += $best
            continue
        }
        # 尺寸对不上就直接问系统：边框中心那一点归谁。窗口正在改变大小时这是唯一可靠的办法。
        $cx = [int](($b.R.L + $b.R.R) / 2)
        $cy = [int](($b.R.T + $b.R.B) / 2)
        $root = [LT]::RootAt($cx, $cy)
        if ($root -ne [IntPtr]::Zero -and [LT]::Cls($root) -ne 'WindowMark.WindowBorder') {
            $hit += [pscustomobject]@{ H = $root; F = [LT]::Frame($root); Cls = [LT]::Cls($root); Loose = $true }
        } else {
            $miss += $b
        }
    }
    return @{ Hit = $hit; Miss = $miss; BorderCount = $borders.Count }
}

if ($Watch -le 0) {
    $s = Sample
    Write-Host "此刻有边框的窗口（边框窗口 $($s.BorderCount) 个，匹配上 $($s.Hit.Count) 个）:"
    $s.Hit | ForEach-Object {
        [pscustomobject]@{ 进程 = ProcName $_.H; 类名 = $_.Cls; 尺寸 = "$($_.F.R - $_.F.L)x$($_.F.B - $_.F.T)"; 标题 = [LT]::Txt($_.H) }
    } | Sort-Object 进程, 类名 | Format-Table -AutoSize | Out-String -Width 220 | Write-Output
    foreach ($m in $s.Miss) {
        Write-Host ("  未匹配的边框: ({0},{1})-({2},{3})" -f $m.R.L, $m.R.T, $m.R.R, $m.R.B) -ForegroundColor Yellow
    }
    exit 0
}

Write-Host ""
Write-Host "开始抓取 $Watch 秒。这段时间请把要排查的都做一遍："
Write-Host "  - 打几个中文字，让候选框出来"
Write-Host "  - 按住 Win 再按空格，把切换面板停住"
Write-Host "  - 点任务栏托盘的向上箭头，展开溢出区"
Write-Host ""

$seen = @{}
$missSeen = @{}
$sw = [Diagnostics.Stopwatch]::StartNew()
$ticks = 0
$lastReport = 0
while ($sw.Elapsed.TotalSeconds -lt $Watch) {
    $ticks++
    $s = Sample
    # 同一类可能同时有多个窗口，一次采样只能算一次，否则比例会超过 100%
    $thisTick = @{}
    foreach ($w in $s.Hit) {
        $key = "$(ProcName $w.H)|$($w.Cls)"
        if (-not $seen.ContainsKey($key)) {
            $seen[$key] = [pscustomobject]@{
                进程 = ProcName $w.H; 类名 = $w.Cls
                尺寸 = "$($w.F.R - $w.F.L)x$($w.F.B - $w.F.T)"
                标题 = [LT]::Txt($w.H)
                出现次数 = 0; 同时最多 = 0
                # 判断「一闪而过」不能只看总次数：第 20 秒才打开、之后一直开着的窗口
                # 也只占 33%，会被误判。要看的是连续性。
                段数 = 0; 最长连续 = 0; 本段长度 = 0; 上次出现的采样 = -99
            }
        }
        $thisTick[$key] = ($thisTick[$key] + 1)
    }
    foreach ($key in $thisTick.Keys) {
        $e = $seen[$key]
        $e.出现次数++
        if ($thisTick[$key] -gt $e.同时最多) { $e.同时最多 = $thisTick[$key] }
        if ($e.上次出现的采样 -eq $ticks - 1) {
            $e.本段长度++
        } else {
            $e.段数++
            $e.本段长度 = 1
        }
        if ($e.本段长度 -gt $e.最长连续) { $e.最长连续 = $e.本段长度 }
        $e.上次出现的采样 = $ticks
    }
    foreach ($m in $s.Miss) {
        $k = "$($m.R.L),$($m.R.T),$($m.R.R),$($m.R.B)"
        $missSeen[$k] = ($missSeen[$k] + 1)
    }
    $elapsed = [int]$sw.Elapsed.TotalSeconds
    if ($elapsed -ge $lastReport + 5) {
        $lastReport = $elapsed
        Write-Host ("  ...{0}s / {1}s，已记录 {2} 类" -f $elapsed, $Watch, $seen.Count)
    }
    Start-Sleep -Milliseconds 150
}

Write-Host ""
Write-Host "=== 这 $Watch 秒里被画过边框的窗口（共 $($seen.Count) 类，采样 $ticks 次）==="
Write-Host ""
$tickSec = 0.15
$rows = $seen.Values | ForEach-Object {
    $pct = [int](100.0 * $_.出现次数 / $ticks)
    $atEnd = ($_.上次出现的采样 -ge $ticks - 2)   # 最后两次采样里还在，就算「结束时仍在」
    # 一闪而过 = 出现过又消失了，或者反复出现过多次。
    # 中途打开之后一直开着的窗口不算 —— 它只有一段，而且结束时还在。
    $flyout = (-not $atEnd) -or ($_.段数 -ge 2)
    [pscustomobject]@{
        进程 = $_.进程; 类名 = $_.类名
        出现 = "$pct%"; 段数 = $_.段数
        最长连续 = "{0:N1}s" -f ($_.最长连续 * $tickSec)
        结束时 = if ($atEnd) { '仍在' } else { '已消失' }
        个数 = $_.同时最多
        尺寸 = $_.尺寸; 标题 = $_.标题.Substring(0, [Math]::Min(24, $_.标题.Length))
        _f = $flyout; _p = $pct
    }
}
$rows | Sort-Object -Property @{E = { -[int]$_._f }}, _p |
    Format-Table 出现, 段数, 最长连续, 结束时, 个数, 进程, 类名, 尺寸, 标题 -AutoSize |
    Out-String -Width 240 | Write-Output

$transient = $rows | Where-Object { $_._f }
if ($transient) {
    Write-Host "下面这些是【出现后又消失】或【反复出现】的浮出窗口，通常不该有边框。"
    Write-Host "（只是中途打开、之后一直开着的窗口不会出现在这里——它只有一段且结束时仍在。）"
    Write-Host ""
    foreach ($r in ($transient | Sort-Object _p)) {
        Write-Host ("    L`"{0}`",{1}// {2}  {3}" -f $r.类名, (' ' * [Math]::Max(1, 42 - $r.类名.Length)), $r.进程, $r.标题)
    }
} else {
    Write-Host "没有出现后又消失的窗口被画边框。"
}

if ($missSeen.Count -gt 0) {
    Write-Host ""
    Write-Host "=== 有边框、但反查不到对应窗口的矩形 ===" -ForegroundColor Yellow
    Write-Host "（边框画出来了，但那一刻找不到尺寸对得上的窗口。浮出窗口关得太快时会这样。"
    Write-Host "  这些坐标就是你看到的那圈边框的位置，贴给我。）"
    foreach ($k in ($missSeen.Keys | Sort-Object { -$missSeen[$_] })) {
        $p = $k -split ','
        Write-Host ("    ({0},{1})-({2},{3})   {4}x{5}   出现 {6} 次" -f `
            $p[0], $p[1], $p[2], $p[3], ([int]$p[2] - [int]$p[0]), ([int]$p[3] - [int]$p[1]), $missSeen[$k])
    }
}
