param([ValidateRange(1,11)][int]$Index,[string]$WorkRoot,[string]$ScreenRoot=(Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference='Stop'
if(-not $WorkRoot){$WorkRoot=Join-Path ([IO.Path]::GetTempPath()) ('pmux_capture_'+$PID)}
[IO.Directory]::CreateDirectory($WorkRoot) | Out-Null
$cases=@('test1','test2','test3','test4','pmux_case01_balanced','pmux_case02_pipeline','pmux_case03_crossbar','pmux_case04_feedback','pmux_case05_nested')
$caseID=if($Index -le 9){$cases[$Index-1]}elseif($Index -eq 10){'EQY'}else{'EFFICIENCY'}
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;using System.Runtime.InteropServices;
public class WindowCapture {
 [StructLayout(LayoutKind.Sequential)] public struct RECT {public int Left,Top,Right,Bottom;}
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w,out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w,IntPtr after,int x,int y,int cx,int cy,uint flags);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
}
'@
$ready=Join-Path $WorkRoot "case$Index.ready";$close=Join-Path $WorkRoot "case$Index.close"
$errorLog=Join-Path $WorkRoot "case$Index.error.txt"
foreach($p in @($ready,$close,$errorLog)){if(Test-Path -LiteralPath $p){Remove-Item -LiteralPath $p}}
$pwsh=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$show=Join-Path $PSScriptRoot 'show_case.ps1'
$argLine='"{0}" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "{1}" -CaseID "{2}" -Index {3} -WorkRoot "{4}" -ScreenRoot "{5}"' -f $pwsh,$show,$caseID,$Index,$WorkRoot,$ScreenRoot
$process=Start-Process -FilePath (Join-Path $env:SystemRoot 'System32/conhost.exe') -ArgumentList $argLine -WindowStyle Hidden -PassThru
try{
 $limit=[DateTime]::UtcNow.AddSeconds(25)
 while(-not(Test-Path -LiteralPath $ready)){
  if(Test-Path -LiteralPath $errorLog){throw [IO.File]::ReadAllText($errorLog)}
  if($process.HasExited){throw "Own console exited before rendering: $($process.ExitCode)"}
  if([DateTime]::UtcNow -gt $limit){throw 'Console not ready in 25 seconds'}
  Start-Sleep -Milliseconds 150
 }
 $window=[IO.File]::ReadAllText($ready) | ConvertFrom-Json
 $hwnd=[IntPtr]::new([long]$window.hwnd)
 [WindowCapture]::SetThreadDpiAwarenessContext([IntPtr]::new(-4)) | Out-Null
 # Render this owned console outside the desktop without activating it.
 [WindowCapture]::SetWindowPos($hwnd,[IntPtr]::Zero,-20000,-20000,0,0,0x0055) | Out-Null
 Start-Sleep -Milliseconds 350
 $rect=New-Object WindowCapture+RECT
 if(-not[WindowCapture]::GetWindowRect($hwnd,[ref]$rect)){throw 'Cannot read own window bounds'}
 $bitmap=[Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
 $gfx=[Drawing.Graphics]::FromImage($bitmap);$dc=$gfx.GetHdc()
 try{$ok=[WindowCapture]::PrintWindow($hwnd,$dc,2)}finally{$gfx.ReleaseHdc($dc)}
 if(-not $ok){throw 'Native PrintWindow failed'}
 $name=if($Index -le 9){('{0:D2}_{1}_原版与优化版综合结果.png' -f $Index,$caseID)}elseif($Index -eq 10){'10_EQY九例验证记录.png'}else{'11_十组九例效率测量记录.png'}
 $bitmap.Save((Join-Path $ScreenRoot $name),[Drawing.Imaging.ImageFormat]::Png)
 $gfx.Dispose();$bitmap.Dispose()
 [IO.File]::WriteAllText((Join-Path $WorkRoot "case$Index.capture.json"),(@{case=$caseID;file=$name;window=$window;bounds=$rect;time=[DateTimeOffset]::Now.ToString('o')} | ConvertTo-Json -Depth 5))
 Write-Output $name
}finally{
 [IO.File]::WriteAllText($close,'close')
 if(-not $process.WaitForExit(5000)){Stop-Process -Id $process.Id}
}
