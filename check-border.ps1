# 边框现场诊断。看到某条边没出来时，让那个窗口保持在前台，然后跑：
#
#   powershell -ExecutionPolicy Bypass -File .\check-border.ps1
#   powershell -ExecutionPolicy Bypass -File .\check-border.ps1 -Title 安装
#   powershell -ExecutionPolicy Bypass -File .\check-border.ps1 -Delay 5    # 5 秒后再采样
#
# 它会分别回答两个不同的问题：边框到底画没画，以及画了但是不是被别的窗口盖住了。
param(
    [string]$Title = '',
    [int]$Delay = 0
)

if ($Delay -gt 0) {
    Write-Host "$Delay 秒后开始采样，请把目标窗口切到前台..."
    Start-Sleep -Seconds $Delay
}

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public class CB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int sz);
  public static void Aware(){ SetProcessDpiAwarenessContext(new IntPtr(-4)); }
  public static string Cls(IntPtr h){ var s=new StringBuilder(256); GetClassNameW(h,s,256); return s.ToString(); }
  public static string Txt(IntPtr h){ var s=new StringBuilder(512); GetWindowTextW(h,s,512); return s.ToString(); }
  public static List<IntPtr> Order(){ var l=new List<IntPtr>(); EnumWindows((h,p)=>{l.Add(h);return true;}, IntPtr.Zero); return l; }
}
'@ -ErrorAction Stop
[CB]::Aware() | Out-Null

function GetRect($h) { $r = New-Object CB+RECT; [CB]::GetWindowRect($h, [ref]$r) | Out-Null; $r }
function Describe($h) {
    if ($h -eq [IntPtr]::Zero) { return '<无>' }
    $root = [CB]::GetAncestor($h, 2)
    $procId = 0; [CB]::GetWindowThreadProcessId($root, [ref]$procId) | Out-Null
    $pn = try { (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { '?' }
    return ("{0} [{1}] '{2}'" -f $pn, [CB]::Cls($root), [CB]::Txt($root))
}
function Pixel($x, $y) {
    $bm = New-Object System.Drawing.Bitmap(1, 1)
    $g = [System.Drawing.Graphics]::FromImage($bm)
    $g.CopyFromScreen([int]$x, [int]$y, 0, 0, (New-Object System.Drawing.Size(1, 1)))
    $p = $bm.GetPixel(0, 0)
    $g.Dispose(); $bm.Dispose()
    return ("{0:X2}{1:X2}{2:X2}" -f $p.R, $p.G, $p.B)
}
function Owner($x, $y) {
    $pt = New-Object CB+POINT; $pt.X = [int]$x; $pt.Y = [int]$y
    return [CB]::WindowFromPoint($pt)
}

# ---- 目标窗口 ----
$t = [IntPtr]::Zero
if ($Title) {
    foreach ($h in [CB]::Order()) {
        if ([CB]::IsWindowVisible($h) -and [CB]::Txt($h) -like "*$Title*" -and [CB]::Cls($h) -notlike 'WindowMark.*') { $t = $h; break }
    }
    if ($t -eq [IntPtr]::Zero) { Write-Host "没找到标题含「$Title」的窗口"; exit 1 }
} else {
    $t = [CB]::GetForegroundWindow()
}

$wr = GetRect $t
$fb = New-Object CB+RECT
$hasFrame = [CB]::DwmGetWindowAttribute($t, 9, [ref]$fb, 16) -eq 0
$ord = [CB]::Order()
$ti = $ord.IndexOf($t)

Write-Host ''
Write-Host "目标: $(Describe $t)"
Write-Host ("  GetWindowRect  ({0},{1})-({2},{3})" -f $wr.L, $wr.T, $wr.R, $wr.B)
if ($hasFrame) {
    Write-Host ("  DWM 扩展边界   ({0},{1})-({2},{3})   内缩 L{4} T{5} R{6} B{7}" -f `
        $fb.L, $fb.T, $fb.R, $fb.B, ($fb.L - $wr.L), ($fb.T - $wr.T), ($wr.R - $fb.R), ($wr.B - $fb.B))
} else {
    Write-Host '  DWM 扩展边界   查询失败（会退回 GetWindowRect）'
    $fb = $wr
}
$ex = [CB]::GetWindowLongW($t, -20)
$owner = [CB]::GetWindow($t, 4)
Write-Host ("  exstyle 0x{0:X8}  TOPMOST={1}  TOOLWINDOW={2}  有属主={3}" -f `
    $ex, [bool]($ex -band 0x8), [bool]($ex -band 0x80), ($owner -ne [IntPtr]::Zero))
if (($ex -band 0x80) -or ($owner -ne [IntPtr]::Zero)) {
    Write-Host '  -> 这个窗口本来就不在跟踪范围内（tool window 或有属主），不会有边框' -ForegroundColor Yellow
}

# ---- 找它的边框窗口 ----
$cx = ($wr.L + $wr.R) / 2.0; $cy = ($wr.T + $wr.B) / 2.0
$b = [IntPtr]::Zero
foreach ($h in $ord) {
    if ([CB]::Cls($h) -ne 'WindowMark.WindowBorder') { continue }
    $r = GetRect $h
    if ([math]::Abs((($r.L + $r.R) / 2.0) - $cx) -lt 60 -and [math]::Abs((($r.T + $r.B) / 2.0) - $cy) -lt 60) { $b = $h; break }
}
Write-Host ''
if ($b -eq [IntPtr]::Zero) {
    Write-Host '边框窗口: 没有。这个窗口根本没被创建边框。' -ForegroundColor Red
    exit 0
}
$br = GetRect $b
$bi = $ord.IndexOf($b)
Write-Host "边框窗口: 0x$('{0:X}' -f [int64]$b)"
Write-Host ("  rect ({0},{1})-({2},{3})   相对 DWM 边界外扩 L{4} T{5} R{6} B{7}" -f `
    $br.L, $br.T, $br.R, $br.B, ($fb.L - $br.L), ($fb.T - $br.T), ($br.R - $fb.R), ($br.B - $fb.B))
Write-Host ("  可见={0}   z: 边框={1} 目标={2} -> {3}" -f `
    [CB]::IsWindowVisible($b), $bi, $ti, $(if ($bi -lt $ti) { '边框在上 OK' } else { "边框被压 $($bi - $ti) 位  <<< 错" }))

# ---- 逐边采样 ----
Write-Host ''
Write-Host '四条边采样（每条 5 个点，取最外那一像素）:'
# 前置逗号是必须的：不加的话 PowerShell 会把每个 @(x,y) 展平成两个独立元素
$edges = @(
    @{ n = '左'; pts = (1..5 | ForEach-Object { , @($br.L, ($br.T + ($br.B - $br.T) * $_ / 6)) }) },
    @{ n = '上'; pts = (1..5 | ForEach-Object { , @(($br.L + ($br.R - $br.L) * $_ / 6), $br.T) }) },
    @{ n = '右'; pts = (1..5 | ForEach-Object { , @(($br.R - 1), ($br.T + ($br.B - $br.T) * $_ / 6)) }) },
    @{ n = '下'; pts = (1..5 | ForEach-Object { , @(($br.L + ($br.R - $br.L) * $_ / 6), ($br.B - 1)) }) }
)
foreach ($e in $edges) {
    $cols = @()
    $blockers = @{}
    foreach ($p in $e.pts) {
        $cols += Pixel $p[0] $p[1]
        $o = Owner $p[0] $p[1]
        $root = if ($o -ne [IntPtr]::Zero) { [CB]::GetAncestor($o, 2) } else { [IntPtr]::Zero }
        if ($root -ne $b -and $root -ne $t) { $blockers[(Describe $o)] = $true }
    }
    Write-Host ("  {0}边: {1}" -f $e.n, ($cols -join ' '))
    if ($blockers.Count -gt 0) {
        foreach ($k in $blockers.Keys) { Write-Host ("       被这个窗口占着: {0}" -f $k) -ForegroundColor Yellow }
    }
}

Write-Host ''
$conf = Join-Path $env:LOCALAPPDATA 'WindowMark\settings.conf'
if (Test-Path $conf) {
    Select-String -Path $conf -Pattern '^border\.(width|offset|active_color|inactive_color)' |
        ForEach-Object { Write-Host ("  " + $_.Line) }
}
Write-Host ''
Write-Host '判读：某条边的颜色不是上面的 active/inactive 色，就是那条边没画出来或被盖住；'
Write-Host '      如果同时报了「被这个窗口占着」，那就是被盖住，不是没画。'
