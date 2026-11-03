param(
    [string]$OssRoot='',
    [string]$MakeBin='',
    [string]$ShellBin='',
    [string]$Case='all',
    [ValidateRange(1,4)][int]$Jobs=2,
    [string]$WorkRoot='',
    [string]$ConfigFile=''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
function Project-Path([string]$value){
    if([IO.Path]::IsPathRooted($value)){return [IO.Path]::GetFullPath($value)}
    return [IO.Path]::GetFullPath((Join-Path $root $value))
}
if($ConfigFile){
    $configurationPath=Project-Path $ConfigFile
    if(-not (Test-Path -LiteralPath $configurationPath -PathType Leaf)){throw "Missing local configuration: $configurationPath"}
}else{$configurationPath=Join-Path $root 'config/windows-tools.local.psd1'}
if(Test-Path -LiteralPath $configurationPath -PathType Leaf){
    $configuration=Import-PowerShellDataFile -LiteralPath $configurationPath
    foreach($name in @('OssRoot','MakeBin','ShellBin','WorkRoot')){
        if(-not $PSBoundParameters.ContainsKey($name) -and $configuration.ContainsKey($name)){
            Set-Variable -Name $name -Value ([string]$configuration[$name]) -Scope Local
        }
    }
}
if(-not $OssRoot){$OssRoot=$env:YOSYSHQ_ROOT}
if(-not $OssRoot){
    $yosys=Get-Command yosys.exe -ErrorAction SilentlyContinue
    if($yosys){$OssRoot=Split-Path -Parent (Split-Path -Parent $yosys.Source)}
}
if(-not $OssRoot){throw 'Set OssRoot in config/windows-tools.local.psd1, activate OSS CAD Suite, or supply -OssRoot.'}
$oss=(Resolve-Path -LiteralPath (Project-Path $OssRoot)).Path
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
$MakeBin=(Resolve-Path -LiteralPath (Project-Path $MakeBin)).Path
$ShellBin=(Resolve-Path -LiteralPath (Project-Path $ShellBin)).Path
if($WorkRoot){$WorkRoot=Project-Path $WorkRoot}
$python=Join-Path $oss 'lib/python3.exe'
if(-not (Test-Path -LiteralPath $python)){throw "Missing Windows OSS CAD Suite Python: $python"}
$arguments=@((Join-Path $PSScriptRoot 'prove_windows.py'),'--oss-root',$oss,'--make-bin',$MakeBin,
    '--shell-bin',$ShellBin,'--case',$Case,'--jobs',"$Jobs")
if($WorkRoot){$arguments+=@('--work-root',$WorkRoot)}
# Keep Unicode project paths out of the MinGW Python command-line decoder.
$payload=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($arguments | ConvertTo-Json -Compress)))
$bootstrap="import base64,json,os,runpy,sys; sys.argv=json.loads(base64.b64decode(sys.argv[1]).decode('utf-8')); sys.path.insert(0,os.path.dirname(sys.argv[0])); runpy.run_path(sys.argv[0],run_name='__main__')"
$beforePath=$env:PATH
$beforePythonEncoding=$env:PYTHONIOENCODING
try{
    $env:PATH=(Join-Path $oss 'bin')+';'+(Join-Path $oss 'lib')+';'+$beforePath
    $env:PYTHONIOENCODING='utf-8'
    & $python -c $bootstrap $payload
    if($LASTEXITCODE -ne 0){throw "Windows verification failed with exit code $LASTEXITCODE. Inspect the new runs/eqy_windows_* directory."}
}finally{
    $env:PATH=$beforePath
    if($null -eq $beforePythonEncoding){Remove-Item Env:PYTHONIOENCODING -ErrorAction SilentlyContinue}
    else{$env:PYTHONIOENCODING=$beforePythonEncoding}
}
