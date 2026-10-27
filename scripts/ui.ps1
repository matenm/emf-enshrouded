# Minimal desktop control for unattended in-game tests (screenshots + synthetic input).
#   ui.ps1 shot <out.png> [maxWidth]   screenshot of the primary screen, downscaled; prints scale factor
#   ui.ps1 click <x> <y> [right]       click at SCREEN pixel coordinates
#   ui.ps1 move <x> <y>                move the mouse
#   ui.ps1 key <name> [holdMs]         press a key by scan code (Esc, Enter, Space, Tab, W, A, S, D, F1..F12, Up, Down, Left, Right, Backspace, E, I, M, Tilde = the key under Esc, PageUp, PageDown)
#   ui.ps1 type <text>                 type unicode text
#   ui.ps1 focus                       bring the Enshrouded window to the front
#   ui.ps1 rect                        print the Enshrouded window rectangle
#   ui.ps1 place <x> <y>               move the Enshrouded window (physical pixels)
#   ui.ps1 press [right]               click where the mouse is (no cursor move)
# click/press/look/key/type refuse (exit 3) unless the game window is in front; click also refuses points outside it.
param([Parameter(Position = 0)][string]$Cmd, [Parameter(Position = 1)][string]$A, [Parameter(Position = 2)][string]$B, [Parameter(Position = 3)][string]$C)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Ui {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
  public static void Mouse(uint flags) {
    var i = new INPUT[1]; i[0].type = 0; i[0].u.mi.dwFlags = flags; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void MoveRel(int dx, int dy) {
    var i = new INPUT[1]; i[0].type = 0; i[0].u.mi.dx = dx; i[0].u.mi.dy = dy; i[0].u.mi.dwFlags = 0x0001;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Scan(ushort scan, bool up, bool extended) {
    var i = new INPUT[1]; i[0].type = 1; i[0].u.ki.wScan = scan;
    i[0].u.ki.dwFlags = 0x0008u | (up ? 0x0002u : 0u) | (extended ? 0x0001u : 0u);
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Unicode(char ch, bool up) {
    var i = new INPUT[1]; i[0].type = 1; i[0].u.ki.wScan = ch; i[0].u.ki.dwFlags = 0x0004u | (up ? 0x0002u : 0u);
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
}
"@
[void][Ui]::SetProcessDPIAware()

$scan = @{
    "Esc" = @(0x01, $false); "Enter" = @(0x1C, $false); "Space" = @(0x39, $false); "Tab" = @(0x0F, $false)
    "Backspace" = @(0x0E, $false); "W" = @(0x11, $false); "A" = @(0x1E, $false); "S" = @(0x1F, $false); "D" = @(0x20, $false)
    "E" = @(0x12, $false); "I" = @(0x17, $false); "M" = @(0x32, $false); "Q" = @(0x10, $false); "F" = @(0x21, $false); "K" = @(0x25, $false); "G" = @(0x22, $false); "L" = @(0x26, $false)
    "Up" = @(0x48, $true); "Down" = @(0x50, $true); "Left" = @(0x4B, $true); "Right" = @(0x4D, $true)
    "F1" = @(0x3B, $false); "F2" = @(0x3C, $false); "F3" = @(0x3D, $false); "F4" = @(0x3E, $false); "F5" = @(0x3F, $false)
    "F6" = @(0x40, $false); "F7" = @(0x41, $false); "F8" = @(0x42, $false); "F9" = @(0x43, $false); "F10" = @(0x44, $false)
    "F11" = @(0x57, $false); "F12" = @(0x58, $false); "Shift" = @(0x2A, $false); "Tilde" = @(0x29, $false); "PageUp" = @(0x49, $true); "PageDown" = @(0x51, $true); "Ctrl" = @(0x1D, $false); "Alt" = @(0x38, $false)
}

function Get-GameWindow {
    $p = Get-Process enshrouded -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p -or $p.MainWindowHandle -eq 0) { return [IntPtr]::Zero }
    return $p.MainWindowHandle
}

# Input goes only to the game: refuse when another window is in front (the user may be using the PC), and refuse clicks
# outside the game window.
function Assert-GameFront {
    $p = Get-Process enshrouded -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { "refused: the game is not running"; exit 3 }
    $fgPid = [uint32]0
    [void][Ui]::GetWindowThreadProcessId([Ui]::GetForegroundWindow(), [ref]$fgPid)
    if ($fgPid -ne $p.Id) { "refused: the game window is not in front (input would go to another program)"; exit 3 }
}
function Assert-InGame([int]$x, [int]$y) {
    $h = Get-GameWindow
    $r = New-Object Ui+RECT; [void][Ui]::GetWindowRect($h, [ref]$r)
    if ($x -lt $r.Left -or $x -ge $r.Right -or $y -lt $r.Top -or $y -ge $r.Bottom) { "refused: $x,$y is outside the game window"; exit 3 }
}
if ($Cmd -in @("click", "press", "look", "key", "type")) { Assert-GameFront }
if ($Cmd -eq "click") { Assert-InGame ([int]$A) ([int]$B) }

switch ($Cmd) {
    "shot" {
        $w = [Ui]::GetSystemMetrics(0); $h = [Ui]::GetSystemMetrics(1)
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
        $max = if ($B) { [int]$B } else { 1600 }
        $scale = [Math]::Min(1.0, $max / $w)
        $out = New-Object System.Drawing.Bitmap ([int]($w * $scale)), ([int]($h * $scale))
        $g2 = [System.Drawing.Graphics]::FromImage($out)
        $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g2.DrawImage($bmp, 0, 0, $out.Width, $out.Height)
        $out.Save($A, [System.Drawing.Imaging.ImageFormat]::Png)
        "screen ${w}x${h} saved ${A} at scale $scale (screen = image / $scale)"
    }
    "click" {
        [void][Ui]::SetCursorPos([int]$A, [int]$B); Start-Sleep -Milliseconds 80
        if ($C -eq "right") { [Ui]::Mouse(0x0008); Start-Sleep -Milliseconds 60; [Ui]::Mouse(0x0010) }
        else { [Ui]::Mouse(0x0002); Start-Sleep -Milliseconds 60; [Ui]::Mouse(0x0004) }
        "clicked $A,$B $C"
    }
    "move" { [void][Ui]::SetCursorPos([int]$A, [int]$B); "moved $A,$B" }
    "press" {
        # a click where the mouse is, without moving the system cursor (EMF's console cursor follows raw movement)
        if ($A -eq "right") { [Ui]::Mouse(0x0008); Start-Sleep -Milliseconds 60; [Ui]::Mouse(0x0010) }
        else { [Ui]::Mouse(0x0002); Start-Sleep -Milliseconds 60; [Ui]::Mouse(0x0004) }
        "pressed $A"
    }
    "look" {
        # relative mouse motion in small steps (turns the in-game camera)
        $steps = 20; $dx = [int]([int]$A / $steps); $dy = [int]([int]$B / $steps)
        for ($k = 0; $k -lt $steps; $k++) { [Ui]::MoveRel($dx, $dy); Start-Sleep -Milliseconds 10 }
        "looked $A,$B"
    }
    "key" {
        $k = $scan[$A]; if (-not $k) { throw "unknown key $A" }
        $hold = if ($B) { [int]$B } else { 60 }
        [Ui]::Scan([uint16]$k[0], $false, $k[1]); Start-Sleep -Milliseconds $hold; [Ui]::Scan([uint16]$k[0], $true, $k[1])
        "key $A ($hold ms)"
    }
    "type" { foreach ($ch in $A.ToCharArray()) { [Ui]::Unicode($ch, $false); [Ui]::Unicode($ch, $true); Start-Sleep -Milliseconds 30 }; "typed '$A'" }
    "focus" {
        $h = Get-GameWindow; if ($h -eq [IntPtr]::Zero) { "no game window"; exit 1 }
        [Ui]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [Ui]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)  # Alt tap allows SetForegroundWindow
        [void][Ui]::ShowWindow($h, 9); [void][Ui]::SetForegroundWindow($h); "focused"
    }
    "place" {
        # moves the game window's top-left corner to x,y (physical pixels) without resizing it
        $h = Get-GameWindow; if ($h -eq [IntPtr]::Zero) { "no game window"; exit 1 }
        [void][Ui]::SetWindowPos($h, [IntPtr]::Zero, [int]$A, [int]$B, 0, 0, 0x0001 -bor 0x0004); "placed at $A,$B"
    }
    "rect" {
        $h = Get-GameWindow; if ($h -eq [IntPtr]::Zero) { "no game window"; exit 1 }
        $r = New-Object Ui+RECT; [void][Ui]::GetWindowRect($h, [ref]$r); "rect $($r.Left) $($r.Top) $($r.Right) $($r.Bottom)"
    }
    default { "usage: ui.ps1 shot|click|move|key|type|focus|rect ..." ; exit 2 }
}
