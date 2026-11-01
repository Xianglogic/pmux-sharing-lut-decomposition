param([string]$CaseID,[int]$Index,[string]$WorkRoot,[string]$ScreenRoot)
trap { $_ | Out-String | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $WorkRoot "case$Index.error.txt"); exit 1 }
$ErrorActionPreference='Stop'
$ui=$Host.UI.RawUI
$ui.WindowTitle="PMUX-NoPost-$Index-$CaseID-20261101"
Add-Type @'
using System;using System.Runtime.InteropServices;
public class ConsoleFont {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct INFO {public uint cbSize,nFont;public short X,Y;public int Family,Weight;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)] public string Face;}
 [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int n);
 [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern bool SetCurrentConsoleFontEx(IntPtr h,bool max,ref INFO info);
 public static void Set(){var f=new INFO();f.cbSize=(uint)Marshal.SizeOf(f);f.Y=18;f.Family=54;f.Weight=400;f.Face="Consolas";SetCurrentConsoleFontEx(GetStdHandle(-11),false,ref f);}
}
'@
[ConsoleFont]::Set()
$ui.BackgroundColor='Black';$ui.ForegroundColor='Gray'
$ui.WindowSize=[System.Management.Automation.Host.Size]::new(100,25)
$ui.BufferSize=[System.Management.Automation.Host.Size]::new(144,3000)
$max=$ui.MaxPhysicalWindowSize
$ui.WindowSize=[System.Management.Automation.Host.Size]::new([Math]::Min(144,$max.Width),[Math]::Min(54,$max.Height-1))
Clear-Host
if($Index -le 9){
 Write-Host "Yosys 0.69 | $CaseID | final mapping statistics | V53H-NoPost" -ForegroundColor Cyan
 Write-Host 'Experiment: 2026-11-01; initialization before synthesis.'
 Write-Host 'Baseline = original Yosys; candidate = V53H-NoPost. Same RTL, top, parameters and external script.'
 Write-Host 'Original synthesis log excerpts.'
 ''
 foreach($arm in @('baseline','candidate')){
  $path=Join-Path $ScreenRoot "原始日志/综合/$CaseID/$arm.full.log"
  Write-Host "SOURCE: $CaseID/$arm.full.log" -ForegroundColor Yellow
  $lines=Get-Content -LiteralPath $path -Encoding UTF8
  $first=-1
  for($i=0;$i -lt $lines.Count;$i++){if($lines[$i] -eq "=== $CaseID ==="){$first=$i}}
  if($first -lt 0){throw "Missing final statistics: $path"}
  $end=$first+3
  while($end -lt $lines.Count -and $lines[$end] -notmatch 'Executing CHECK pass'){$end++}
  if($end -ge $lines.Count){throw "Missing CHECK: $path"}
  $lines[$first..($end-1)]
  $lines[($end+1)..($end+2)]
  if($lines[$end+2] -ne 'Found and reported 0 problems.'){throw "Unexpected CHECK: $path"}
  ''
 }
}else{Get-Content -Encoding UTF8 -LiteralPath (Join-Path $ScreenRoot "截图脚本/screen$Index.txt")}
$rendered=$ui.CursorPosition.Y+2
if($rendered -gt $ui.WindowSize.Height){throw "Console output clipped: required=$rendered available=$($ui.WindowSize.Height)"}
$ui.WindowSize=[System.Management.Automation.Host.Size]::new($ui.WindowSize.Width,$rendered)
$meta=@{title=$ui.WindowTitle;hwnd=[ConsoleFont]::GetConsoleWindow().ToInt64();rows=$rendered;columns=$ui.WindowSize.Width;case=$CaseID}
[IO.File]::WriteAllText((Join-Path $WorkRoot "case$Index.ready"),($meta | ConvertTo-Json -Compress))
while(-not(Test-Path -LiteralPath (Join-Path $WorkRoot "case$Index.close"))){Start-Sleep -Milliseconds 150}
