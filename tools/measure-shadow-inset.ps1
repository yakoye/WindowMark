# 量出一个窗口自己画在窗口矩形里的阴影有多宽，输出可以直接粘进 settings.conf 的一行。
#
# 为什么需要手工量：GTK 这类客户端自绘装饰（CSD）把阴影画在自己的窗口矩形内部，
# 而 GetWindowRect、DWMWA_EXTENDED_FRAME_BOUNDS、GetClientRect 三者返回同一个矩形，
# 命中测试也覆盖阴影区，没有任何 Windows 接口能告诉你不透明内容从哪里开始。
#
# 这个工具用 PrintWindow 抓窗口位图再读 alpha 通道。那会让目标程序重绘一次，
# 所以它只放在这个「你主动运行」的工具里，常驻的 WindowMark 不做这件事。
#
# 用法：
#   measure-shadow-inset.ps1              倒数 5 秒后量光标所在的窗口
#   measure-shadow-inset.ps1 Czkawka      直接量标题含该关键字的窗口

$ErrorActionPreference = 'Stop'
$keyword = if ($args.Count -gt 0) { $args[0] } else { $null }

Add-Type -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public class MSI {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFOHEADER {
    public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount;
    public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter;
    public uint biClrUsed, biClrImportant;
  }
  public delegate bool EnumProc(IntPtr h,IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb,IntPtr p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h,uint f);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h,IntPtr dc);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bmi, uint usage, out IntPtr bits, IntPtr sect, uint off);
  [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc,IntPtr o);
  [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr dc);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h,int a,out RECT r,int sz);
  public static void Aware(){ SetProcessDpiAwarenessContext(new IntPtr(-4)); }
  public static string Cls(IntPtr h){ var s=new StringBuilder(256); GetClassNameW(h,s,256); return s.ToString(); }
  public static string Txt(IntPtr h){ var s=new StringBuilder(256); GetWindowTextW(h,s,256); return s.ToString(); }
  public static List<IntPtr> All(){ var l=new List<IntPtr>(); EnumWindows((h,p)=>{l.Add(h);return true;},IntPtr.Zero); return l; }
  public static IntPtr UnderCursor(){
    POINT p; GetCursorPos(out p);
    IntPtr h=WindowFromPoint(p);
    if(h==IntPtr.Zero) return IntPtr.Zero;
    return GetAncestor(h,2);
  }
  public static byte[] Grab(IntPtr hwnd,int w,int h){
    IntPtr screen=GetWindowDC(IntPtr.Zero);
    IntPtr mem=CreateCompatibleDC(screen);
    var bmi=new BITMAPINFOHEADER();
    bmi.biSize=(uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
    bmi.biWidth=w; bmi.biHeight=-h; bmi.biPlanes=1; bmi.biBitCount=32; bmi.biCompression=0;
    IntPtr bits;
    IntPtr dib=CreateDIBSection(mem,ref bmi,0,out bits,IntPtr.Zero,0);
    if(dib==IntPtr.Zero){ DeleteDC(mem); ReleaseDC(IntPtr.Zero,screen); return null; }
    IntPtr old=SelectObject(mem,dib);
    bool ok=PrintWindow(hwnd,mem,0);
    byte[] data=null;
    if(ok){ data=new byte[w*h*4]; Marshal.Copy(bits,data,0,data.Length); }
    SelectObject(mem,old); DeleteObject(dib); DeleteDC(mem); ReleaseDC(IntPtr.Zero,screen);
    return data;
  }
}
'@
[MSI]::Aware() | Out-Null

$target = [IntPtr]::Zero
if ($keyword) {
    foreach ($h in [MSI]::All()) {
        if (-not [MSI]::IsWindowVisible($h)) { continue }
        if ([MSI]::Txt($h) -notmatch $keyword) { continue }
        $target = $h; break
    }
    if ($target -eq [IntPtr]::Zero) { Write-Host ("找不到标题含「{0}」的窗口" -f $keyword) -ForegroundColor Red; exit 1 }
} else {
    Write-Host '把鼠标移到要测量的窗口上，保持不动。'
    for ($i = 5; $i -ge 1; $i--) { Write-Host ("  {0}..." -f $i); Start-Sleep -Seconds 1 }
    $target = [MSI]::UnderCursor()
    if ($target -eq [IntPtr]::Zero) { Write-Host '光标下没有窗口' -ForegroundColor Red; exit 1 }
}

$r = New-Object MSI+RECT; [MSI]::GetWindowRect($target, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h2 = $r.B - $r.T
$cls = [MSI]::Cls($target)
Write-Host ''
Write-Host ("窗口「{0}」" -f [MSI]::Txt($target))
Write-Host ("  类名 {0}   {1}x{2}   最大化={3}" -f $cls, $w, $h2, [MSI]::IsZoomed($target))

if ([MSI]::IsZoomed($target)) {
    Write-Host ''
    Write-Host '窗口是最大化的，这时通常没有阴影，量不出内缩量。' -ForegroundColor Yellow
    Write-Host '请先还原窗口再测。'
    exit 1
}

$ef = New-Object MSI+RECT
if ([MSI]::DwmGetWindowAttribute($target, 9, [ref]$ef, 16) -eq 0) {
    $same = ($ef.L -eq $r.L -and $ef.T -eq $r.T -and $ef.R -eq $r.R -and $ef.B -eq $r.B)
    Write-Host ("  DWM 边界与窗口矩形{0}" -f $(if ($same) { '相同（客户端自绘装饰的特征）' } else { '有差值（普通 Win32 窗口）' }))
}

$d = [MSI]::Grab($target, $w, $h2)
if ($null -eq $d) { Write-Host 'PrintWindow 失败，量不了这个窗口' -ForegroundColor Red; exit 1 }

$thr = 200
$cap = [int][Math]::Min(150, [Math]::Min($w, $h2) / 3)

function Mode($list) {
    if ($list.Count -eq 0) { return -1 }
    $g = $list | Group-Object | Sort-Object Count -Descending
    return [int]$g[0].Name
}

$lefts = @(); $rights = @(); $tops = @(); $bottoms = @()
for ($y = [int]($h2 * 0.2); $y -lt [int]($h2 * 0.8); $y += 5) {
    $i = 0; while ($i -lt $cap -and $d[(($y * $w) + $i) * 4 + 3] -lt $thr) { $i++ }
    if ($i -lt $cap) { $lefts += $i }
    $i = 0; while ($i -lt $cap -and $d[(($y * $w) + ($w - 1 - $i)) * 4 + 3] -lt $thr) { $i++ }
    if ($i -lt $cap) { $rights += $i }
}
for ($x = [int]($w * 0.2); $x -lt [int]($w * 0.8); $x += 5) {
    $i = 0; while ($i -lt $cap -and $d[(($i * $w) + $x) * 4 + 3] -lt $thr) { $i++ }
    if ($i -lt $cap) { $tops += $i }
    $i = 0; while ($i -lt $cap -and $d[((($h2 - 1 - $i) * $w) + $x) * 4 + 3] -lt $thr) { $i++ }
    if ($i -lt $cap) { $bottoms += $i }
}

$L = Mode $lefts; $T = Mode $tops; $R = Mode $rights; $B = Mode $bottoms
$total = $lefts.Count + $tops.Count + $rights.Count + $bottoms.Count

Write-Host ''
if ($L -lt 0 -or $T -lt 0 -or $R -lt 0 -or $B -lt 0 -or $total -lt 40) {
    Write-Host '这个窗口的位图里读不到可用的 alpha —— 此法对它无效。' -ForegroundColor Red
    Write-Host '（多数普通 Win32 窗口都是这样，它们本来也不需要内缩。）'
    exit 1
}
if ($L -eq 0 -and $T -eq 0 -and $R -eq 0 -and $B -eq 0) {
    Write-Host '四边内缩都是 0 —— 这个窗口没有自绘阴影，不需要配置。' -ForegroundColor Green
    exit 0
}

Write-Host ("测得内缩量：左 {0}  上 {1}  右 {2}  下 {3}   （{4} 条扫描线）" -f $L, $T, $R, $B, $total) -ForegroundColor Green
Write-Host ''
Write-Host '把下面这行写进 %LOCALAPPDATA%\WindowMark\settings.conf，然后重启 WindowMark：' -ForegroundColor Cyan
Write-Host ("  tracking.shadow_insets={0}:{1},{2},{3},{4}" -f $cls, $L, $T, $R, $B) -ForegroundColor Yellow
Write-Host ''
Write-Host '已经有这一行的话，用 | 分隔多个应用，例如：'
Write-Host ("  tracking.shadow_insets=gdkSurfaceToplevel:22,12,22,38|OtherClass:10,0,10,10")
