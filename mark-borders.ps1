# 给屏幕上每个边框编号，方便你直接说「3 号我不要」。
#
# 先盯一段时间（默认 20 秒），把期间所有被画过边框的窗口都记下来，然后统一编号：
#   - 还在屏幕上的，在它左上角贴一个黄色数字牌
#   - 已经消失的（浮出窗口关掉了），只在列表里列出来，编号一样
# 看完按回车收掉数字牌。
#
#   mark-borders.ps1              盯 20 秒
#   mark-borders.ps1 -Watch 40    盯 40 秒
#   mark-borders.ps1 -Watch 0     只看此刻
#
# 拿到编号后，把不想要的那个的类名填进设置里的「排除窗口类名」，或者直接告诉我。
param(
    [int]$Watch = 20
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public class MB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern int SetWindowLongW(IntPtr h, int i, int v);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int sz);
  public static void Aware(){ SetProcessDpiAwarenessContext(new IntPtr(-4)); }
  public static string Cls(IntPtr h){ var s=new StringBuilder(256); GetClassNameW(h,s,256); return s.ToString(); }
  public static string Txt(IntPtr h){ var s=new StringBuilder(512); GetWindowTextW(h,s,512); return s.ToString(); }
  public static List<IntPtr> Order(){ var l=new List<IntPtr>(); EnumWindows((h,p)=>{l.Add(h);return true;}, IntPtr.Zero); return l; }
  public static RECT Frame(IntPtr h){
    RECT r;
    if (DwmGetWindowAttribute(h, 9, out r, 16) == 0) return r;
    GetWindowRect(h, out r);
    return r;
  }
  // 数字牌自己也是窗口，必须挂上 WS_EX_TOOLWINDOW，否则 WindowMark 会给它也画个边框
  public static void MakeToolWindow(IntPtr h){
    int ex = GetWindowLongW(h, -20);
    SetWindowLongW(h, -20, ex | 0x80);
  }
  public static IntPtr RootAt(int x, int y){
    POINT p; p.X = x; p.Y = y;
    IntPtr h = WindowFromPoint(p);
    if (h == IntPtr.Zero) return IntPtr.Zero;
    IntPtr root = GetAncestor(h, 2);
    return root == IntPtr.Zero ? h : root;
  }
}
'@ -ErrorAction Stop
[MB]::Aware() | Out-Null

$conf = Join-Path $env:LOCALAPPDATA 'WindowMark\settings.conf'
$bw = 4; $bo = -1
if (Test-Path $conf) {
    foreach ($line in (Get-Content $conf)) {
        if ($line -match '^border\.width=(-?\d+)') { $bw = [int]$Matches[1] }
        if ($line -match '^border\.offset=(-?\d+)') { $bo = [int]$Matches[1] }
    }
}
$reach = [Math]::Max(0, [Math]::Max(1, $bw) + $bo)

$procCache = @{}
function ProcName($h) {
    $procId = 0; [MB]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
    if (-not $procCache.ContainsKey($procId)) {
        $procCache[$procId] = try { (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { '?' }
    }
    return $procCache[$procId]
}

function Sample {
    $all = [MB]::Order()
    $borders = @(); $wins = @()
    foreach ($h in $all) {
        $cls = [MB]::Cls($h)
        if ($cls -eq 'WindowMark.WindowBorder') {
            if (-not [MB]::IsWindowVisible($h)) { continue }
            $r = New-Object MB+RECT; [MB]::GetWindowRect($h, [ref]$r) | Out-Null
            $borders += [pscustomobject]@{ R = $r }
            continue
        }
        if (-not [MB]::IsWindowVisible($h)) { continue }
        $f = [MB]::Frame($h)
        if (($f.R - $f.L) -le 1) { continue }
        $wins += [pscustomobject]@{ H = $h; F = $f; Cls = $cls }
    }
    $hit = @()
    foreach ($b in $borders) {
        $wantL = $b.R.L + $reach; $wantT = $b.R.T + $reach
        $wantR = $b.R.R - $reach; $wantB = $b.R.B - $reach
        $best = $null; $bestErr = 5
        foreach ($w in $wins) {
            $err = [math]::Abs($w.F.L - $wantL) + [math]::Abs($w.F.T - $wantT) +
                   [math]::Abs($w.F.R - $wantR) + [math]::Abs($w.F.B - $wantB)
            if ($err -lt $bestErr) { $bestErr = $err; $best = $w }
        }
        if (-not $best) {
            $root = [MB]::RootAt([int](($b.R.L + $b.R.R) / 2), [int](($b.R.T + $b.R.B) / 2))
            if ($root -ne [IntPtr]::Zero -and [MB]::Cls($root) -ne 'WindowMark.WindowBorder') {
                $best = [pscustomobject]@{ H = $root; F = [MB]::Frame($root); Cls = [MB]::Cls($root) }
            }
        }
        if ($best) { $hit += [pscustomobject]@{ W = $best; B = $b.R } }
    }
    return $hit
}

# ---- 收集 ----
$seen = [ordered]@{}
if ($Watch -le 0) {
    foreach ($x in Sample) {
        $key = "$(ProcName $x.W.H)|$($x.W.Cls)"
        if (-not $seen.Contains($key)) { $seen[$key] = @{ W = $x.W; B = $x.B; N = 0 } }
        $seen[$key].N++
    }
} else {
    Write-Host ""
    Write-Host "盯 $Watch 秒。这段时间把你不想要边框的东西都调出来一遍："
    Write-Host "  打中文让候选框出来、Win+空格、点托盘箭头、开右键菜单……"
    Write-Host ""
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $last = 0
    while ($sw.Elapsed.TotalSeconds -lt $Watch) {
        foreach ($x in Sample) {
            $key = "$(ProcName $x.W.H)|$($x.W.Cls)"
            if (-not $seen.Contains($key)) { $seen[$key] = @{ W = $x.W; B = $x.B; N = 0 } }
            $seen[$key].N++
            $seen[$key].B = $x.B   # 记最后一次的位置
            $seen[$key].W = $x.W
        }
        $e = [int]$sw.Elapsed.TotalSeconds
        if ($e -ge $last + 5) { $last = $e; Write-Host ("  ...{0}s / {1}s，已记录 {2} 个" -f $e, $Watch, $seen.Count) }
        Start-Sleep -Milliseconds 150
    }
}

if ($seen.Count -eq 0) { Write-Host "没有任何窗口被画边框。"; exit 0 }

# ---- 编号 + 列表 ----
$items = @()
$i = 0
foreach ($key in $seen.Keys) {
    $i++
    $e = $seen[$key]
    $alive = [MB]::IsWindow($e.W.H) -and [MB]::IsWindowVisible($e.W.H)
    $items += [pscustomobject]@{
        编号 = $i
        状态 = if ($alive) { '在屏幕上' } else { '已消失' }
        进程 = ProcName $e.W.H
        类名 = $e.W.Cls
        尺寸 = "$($e.B.R - $e.B.L)x$($e.B.B - $e.B.T)"
        标题 = [MB]::Txt($e.W.H)
        _r = $e.B; _alive = $alive
    }
}
Write-Host ""
Write-Host "=== 被画过边框的窗口 ==="
$items | Format-Table 编号, 状态, 进程, 类名, 尺寸, 标题 -AutoSize | Out-String -Width 240 | Write-Output

# ---- 在屏幕上贴数字牌 ----
$badges = @()
foreach ($it in ($items | Where-Object { $_._alive })) {
    $f = New-Object System.Windows.Forms.Form
    $f.FormBorderStyle = 'None'
    $f.ShowInTaskbar = $false
    $f.TopMost = $true
    $f.StartPosition = 'Manual'
    $f.BackColor = [System.Drawing.Color]::FromArgb(255, 210, 0)
    $f.Size = New-Object System.Drawing.Size(52, 52)
    $f.Location = New-Object System.Drawing.Point(($it._r.L + 4), ($it._r.T + 4))
    $lbl = New-Object System.Windows.Forms.Label
    $lbl.Text = "$($it.编号)"
    $lbl.Font = New-Object System.Drawing.Font('Segoe UI', 22, [System.Drawing.FontStyle]::Bold)
    $lbl.ForeColor = [System.Drawing.Color]::Black
    $lbl.TextAlign = 'MiddleCenter'
    $lbl.Dock = 'Fill'
    $f.Controls.Add($lbl)
    $f.Show()
    [MB]::MakeToolWindow($f.Handle)
    $badges += $f
}
[System.Windows.Forms.Application]::DoEvents()

Write-Host ("屏幕上已标出 {0} 个编号（黄色数字牌贴在各自边框的左上角）。" -f $badges.Count)
$gone = ($items | Where-Object { -not $_._alive }).Count
if ($gone -gt 0) { Write-Host "另有 $gone 个已经消失，只在上面的列表里，没有数字牌。" }
Write-Host ""
Write-Host "看完按回车收掉数字牌..."
[Console]::ReadLine() | Out-Null

foreach ($f in $badges) { $f.Close(); $f.Dispose() }
Write-Host ""
Write-Host "不想要哪个，把它的【类名】填进设置里的「排除窗口类名」（逗号分隔），或者直接告诉我编号。"
