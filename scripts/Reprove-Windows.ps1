param(
    [Parameter(Mandatory=$true)][string]$OssRoot,
    [string]$MakeBin='',
    [string]$ShellBin='',
    [string]$Case='all',
    [ValidateRange(1,4)][int]$Jobs=2,
    [string]$WorkRoot=''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$oss=(Resolve-Path -LiteralPath $OssRoot).Path
if(-not $MakeBin){
    foreach($name in @('make.exe','mingw32-make.exe')){
        $command=Get-Command $name -ErrorAction SilentlyContinue
        if($command){$MakeBin=Split-Path -Parent $command.Source;break}
    }
}
if(-not $MakeBin){throw 'GNU Make is required. Supply -MakeBin with the directory containing make.exe or mingw32-make.exe.'}
if(-not $ShellBin){
    $candidates=@((Join-Path $oss 'bin'),(Join-Path $oss 'lib'))
    $git=Get-Command git.exe -ErrorAction SilentlyContinue
    if($git){$candidates+=Join-Path (Split-Path -Parent (Split-Path -Parent $git.Source)) 'usr/bin'}
    foreach($folder in $candidates){
        if((Test-Path -LiteralPath (Join-Path $folder 'bash.exe')) -and (Test-Path -LiteralPath (Join-Path $folder 'sh.exe'))){$ShellBin=$folder;break}
    }
}
if(-not $ShellBin){throw 'Git Bash or a compatible shell is required. Supply -ShellBin with bash.exe and sh.exe.'}
$python=Join-Path $oss 'lib/python3.exe'
if(-not (Test-Path -LiteralPath $python)){throw "Missing Windows OSS CAD Suite Python: $python"}
$arguments=@((Join-Path $PSScriptRoot 'prove_windows.py'),'--oss-root',$oss,'--make-bin',(Resolve-Path -LiteralPath $MakeBin).Path,
    '--shell-bin',(Resolve-Path -LiteralPath $ShellBin).Path,'--case',$Case,'--jobs',"$Jobs")
if($WorkRoot){$arguments+=@('--work-root',$WorkRoot)}
$beforePath=$env:PATH
try{
    $env:PATH=(Join-Path $oss 'bin')+';'+(Join-Path $oss 'lib')+';'+$beforePath
    & $python @arguments
    if($LASTEXITCODE -ne 0){throw "Windows verification failed with exit code $LASTEXITCODE. Inspect the new runs/eqy_windows_* directory."}
}finally{$env:PATH=$beforePath}
