param([ValidateSet('Smoke','Measure')][string]$Mode='Smoke',[ValidateRange(1,100)][int]$Groups=1,[string]$Case='all')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'Verify.ps1')
if($Mode -eq 'Measure' -and $PSVersionTable.PSVersion.Major -lt 7){throw 'Measurement requires PowerShell 7 (pwsh). Smoke also supports Windows PowerShell 5.1.'}
$manifest=Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'INPUT_MANIFEST.json') | ConvertFrom-Json
$expected=Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'EXPECTED_RESULTS.json') | ConvertFrom-Json
$capture=Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'evidence/current/CAPTURE_FULL.json') | ConvertFrom-Json
$cases=@($manifest.cases | Where-Object {$Case -eq 'all' -or $_.case_id -eq $Case})
if($cases.Count -eq 0){throw "Unknown case: $Case"}
$tag=(Get-Date -Format 'yyyyMMdd_HHmmss_fff')+'_'+[Guid]::NewGuid().ToString('N').Substring(0,6)
$runRelative='runs/'+$tag
$runRoot=Join-Path $root $runRelative
$null=New-Item -ItemType Directory -Path $runRoot
$utf8=[Text.UTF8Encoding]::new($false)
function Save-Json($path,$data){[IO.File]::WriteAllText($path,($data | ConvertTo-Json -Depth 20),$utf8)}
function Relative-YosysPath([string]$path){return $path.Replace('\','/')}
function Write-CaseScript($case,$folderRelative){
    $commands=[System.Collections.Generic.List[string]]::new()
    $options='-sv'
    foreach($inc in $case.include_dirs){$options+=' -I"inputs/'+(Relative-YosysPath $inc)+'"'}
    $sources=@($case.sources | ForEach-Object {'"inputs/'+(Relative-YosysPath $_)+'"'}) -join ' '
    $commands.Add('read_verilog '+$options+'  '+$sources)
    foreach($parameter in $case.parameters.PSObject.Properties){$commands.Add('chparam -set '+$parameter.Name+' '+$parameter.Value+' '+$case.top)}
    $commands.Add('hierarchy -check -top '+$case.top)
    $commands.Add('proc')
    $commands.Add('setundef -init -params -zero')
    $commands.Add('synth_intel -family cycloneiv -top '+$case.top)
    $commands.Add('check -assert')
    $commands.Add('write_rtlil "'+$folderRelative+'/output.il"')
    $commands.Add('write_json "'+$folderRelative+'/output.json"')
    $relative=$folderRelative+'/synthesis.ys'
    [IO.File]::WriteAllText((Join-Path $root $relative),($commands -join '; ')+[Environment]::NewLine,$utf8)
    return $relative
}
function Read-Resources($file,$top){
    $raw=Get-Content -Raw -Encoding UTF8 -LiteralPath $file
    if($PSVersionTable.PSVersion.Major -ge 7){$json=ConvertFrom-Json -InputObject $raw -AsHashtable}
    else{
        Add-Type -AssemblyName System.Web.Extensions
        $reader=New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $reader.MaxJsonLength=[int]::MaxValue
        $json=$reader.DeserializeObject($raw)
    }
    $module=$json['modules'][$top]
    if($null -eq $module){throw "Missing top module $top"}
    $types=@{};$total=0
    foreach($cell in $module['cells'].Values){
        $type=$cell['type'];$total++
        if(-not $types.ContainsKey($type)){$types[$type]=0};$types[$type]++
        if($type -eq '$not' -and [Convert]::ToInt32($cell['parameters']['Y_WIDTH'],2) -ne 1){throw 'Non-scalar NOT'}
    }
    foreach($t in $types.Keys){if($t -notin @('cycloneiv_lcell_comb','cycloneive_lcell_comb','dffeas','$not','$print','$scopeinfo')){throw "Unclassified cell $t"}}
    $lut=[int]$types['cycloneiv_lcell_comb']+[int]$types['cycloneive_lcell_comb']
    return [ordered]@{logic=$lut+[int]$types['$not'];lut=$lut;not_count=[int]$types['$not'];ff=[int]$types['dffeas'];cells=$total;types=$types}
}
$oldAbc=$env:ABC;$oldData=$env:YOSYS_DATDIR
Push-Location $root
try{
    $env:ABC=(Join-Path $root 'runtime/candidate/yosys-abc.exe')
    Remove-Item Env:YOSYS_DATDIR -ErrorAction SilentlyContinue
    if($Mode -eq 'Smoke'){
        $records=[System.Collections.Generic.List[object]]::new()
        foreach($c in $cases){
            $relative=$runRelative+'/'+$c.case_id;$folder=Join-Path $root $relative
            $null=New-Item -ItemType Directory -Path $folder
            $script=Write-CaseScript $c $relative
            foreach($arm in @('baseline','candidate')){
                $exe=Join-Path $root ('runtime/'+$arm+'/yosys.exe')
                & $exe -Q -T -s $script *> (Join-Path $folder ($arm+'.log'))
                if($LASTEXITCODE -ne 0){throw "Synthesis failed: $($c.case_id) $arm. See $folder"}
                foreach($ext in @('il','json')){Move-Item -LiteralPath (Join-Path $folder ('output.'+$ext)) -Destination (Join-Path $folder ($arm+'.'+$ext))}
                $actual=Read-Resources (Join-Path $folder ($arm+'.json')) $c.top
                $reference=$capture.cases.PSObject.Properties[$c.case_id].Value.arms.PSObject.Properties[$arm].Value.resources
                foreach($metric in @('logic','lut','not_count','ff','cells')){if($actual[$metric] -ne $reference.$metric){throw "Resource mismatch: $($c.case_id)/$arm/$metric"}}
                foreach($type in $actual.types.Keys){if($actual.types[$type] -ne $reference.types.PSObject.Properties[$type].Value){throw "Cell type mismatch: $type"}}
                if($actual.types.Count -ne @($reference.types.PSObject.Properties).Count){throw 'Cell type set mismatch'}
                $records.Add([ordered]@{case=$c.case_id;arm=$arm;status='PASS';mapped=$actual})
                Write-Host "$($c.case_id) $arm PASS logic=$($actual.logic) cells=$($actual.cells)"
            }
        }
        Save-Json (Join-Path $runRoot 'SMOKE_RESULTS.json') ([ordered]@{mode='portable packaging smoke, not efficiency scoring';cases=$cases.Count;records=$records;all_resource_dicts_equal_frozen=$true})
        Write-Host "Smoke complete. Results: $runRelative"
    }else{
        $warmups=[System.Collections.Generic.List[object]]::new()
        foreach($c in $cases){
            $cr=$runRelative+'/warmup/'+$c.case_id;$cf=Join-Path $root $cr;$null=New-Item -ItemType Directory -Path $cf
            $script=Write-CaseScript $c $cr
            $warmups.Add([ordered]@{case=$c.case_id;top=$c.top;folder=$cf;script=(Join-Path $root $script);output_prefix=(Join-Path $cf 'output')})
        }
        $groupPlans=[System.Collections.Generic.List[object]]::new()
        for($g=1;$g -le $Groups;$g++){
            $planCases=[System.Collections.Generic.List[object]]::new()
            foreach($c in $cases){
                $cr=$runRelative+('/group{0:d2}/' -f $g)+$c.case_id;$cf=Join-Path $root $cr;$null=New-Item -ItemType Directory -Path $cf
                $script=Write-CaseScript $c $cr
                $planCases.Add([ordered]@{case=$c.case_id;top=$c.top;folder=$cf;script=(Join-Path $root $script);output_prefix=(Join-Path $cf 'output')})
            }
            $groupPlans.Add([ordered]@{group=$g;cases=$planCases.ToArray()})
        }
        $planFile=Join-Path $runRoot 'NATIVE_PLAN.json'
        Save-Json $planFile ([ordered]@{version='PMUX multi-output sharing and LUT decomposition';baseline=(Join-Path $root 'runtime/baseline/yosys.exe');candidate=(Join-Path $root 'runtime/candidate/yosys.exe');abc=$env:ABC;cwd=$root;meter=(Join-Path $PSScriptRoot 'NativeMeter.cs');reference_root=(Join-Path $root 'evidence/current');sessions=@([ordered]@{policy='each_case10_once';root=$runRoot;warmups=$warmups.ToArray();warmup_runs=10*$cases.Count;groups=$groupPlans.ToArray()});protocol='At session start 10 original warmups per case, once. Original then candidate. Initial >5% and both <=10s: five fresh pairs, initial retained. Main physical peak WS; CPU separately reported.'})
        & (Get-Process -Id $PID).Path -NoProfile -File (Join-Path $PSScriptRoot 'Measure-current.ps1') -PlanPath $planFile
        if($LASTEXITCODE -ne 0){throw 'Measurement failed'}
        $runs=Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $runRoot 'RUNS.json') | ConvertFrom-Json
        foreach($r in $runs){
            if($r.status -ne 'PASS'){throw "Incomplete native run: $($r.prefix)"}
            $actual=Read-Resources ($r.prefix+'.json') $r.top
            $reference=$capture.cases.PSObject.Properties[$r.case].Value.arms.PSObject.Properties[$r.arm].Value.resources
            foreach($metric in @('logic','lut','not_count','ff','cells')){if($actual[$metric] -ne $reference.$metric){throw "Resource mismatch: $($r.prefix)/$metric"}}
        }
        $decisions=Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $runRoot 'DECISIONS.json') | ConvertFrom-Json
        $judgments=[Collections.Generic.List[object]]::new()
        foreach($d in $decisions){
            $relevant=@($runs|Where-Object {$_.group -eq $d.group -and $_.case -eq $d.case})
            $phase=if($d.review_triggered){'review'}else{'initial'}
            $selected=@($relevant|Where-Object {$_.phase -eq $phase})
            $b=($selected|Where-Object {$_.arm -eq 'baseline'}|Measure-Object elapsed_seconds -Average).Average
            $c=($selected|Where-Object {$_.arm -eq 'candidate'}|Measure-Object elapsed_seconds -Average).Average
            $maxMemory=0.0
            foreach($row in @($relevant|Where-Object {$_.arm -eq 'baseline'})){
                $other=@($relevant|Where-Object {$_.arm -eq 'candidate' -and $_.phase -eq $row.phase -and $_.index -eq $row.index})
                if($other.Count -ne 1){throw 'Incomplete paired sample'}
                $ratio=$other[0].measurement.main_peak_working_set_bytes/$row.measurement.main_peak_working_set_bytes
                $maxMemory=[Math]::Max($maxMemory,$ratio)
            }
            $judgments.Add([ordered]@{group=$d.group;case=$d.case;review=$d.review_triggered;time_ratio=$c/$b;worst_memory_ratio=$maxMemory;time_pass=($c/$b -le 1.05);memory_pass=($maxMemory -le 1.05)})
        }
        Save-Json (Join-Path $runRoot 'JUDGMENTS.json') $judgments.ToArray()
        $timePass=@($judgments|Where-Object {$_.time_pass}).Count;$memoryPass=@($judgments|Where-Object {$_.memory_pass}).Count
        Write-Host "New machine campaign: time $timePass/$($judgments.Count), memory $memoryPass/$($judgments.Count). All samples retained: $runRelative"
        if($timePass -ne $judgments.Count -or $memoryPass -ne $judgments.Count){throw 'This new campaign has efficiency overruns; inspect JUDGMENTS.json'}
    }
}finally{
    $env:ABC=$oldAbc;$env:YOSYS_DATDIR=$oldData;Pop-Location
}
