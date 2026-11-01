param([Parameter(Mandatory=$true)][string]$PlanPath)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -lt 7){throw 'Use PowerShell 7 for the frozen NativeMeter implementation.'}
$root=Split-Path -Parent $PlanPath
$plan=Get-Content -Raw -Encoding utf8 -LiteralPath $PlanPath | ConvertFrom-Json
$formal=Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $plan.reference_root 'PROOFS_FULL_LF.json') | ConvertFrom-Json
if(-not $formal.all_checks_pass){throw 'Formal processes must finish before timing.'}
if(Test-Path -LiteralPath (Join-Path $root 'NATIVE_COMPLETE.json')){throw 'Do not overwrite a completed campaign.'}
$beforeAbc=$env:ABC
$beforeData=$env:YOSYS_DATDIR
$env:ABC=$plan.abc
Remove-Item Env:YOSYS_DATDIR -ErrorAction SilentlyContinue
Add-Type -Path $plan.meter
$utf8=[Text.UTF8Encoding]::new($false)
function Save-Json($file,$data){[IO.File]::WriteAllText($file,($data|ConvertTo-Json -Depth 18),$utf8)}
function Invoke-Case($session,$group,$case,[string]$arm,[string]$phase,[int]$index){
    $exe=if($arm -eq 'baseline'){$plan.baseline}else{$plan.candidate}
    $prefix=Join-Path $case.folder ($phase+'_'+$index+'_'+$arm)
    if(Test-Path -LiteralPath ($prefix+'.log')){throw 'Existing samples may not be overwritten.'}
    try{
        $measurement=$null
        $duration=Measure-Command {
            $measurement=[NativeMeter]::Run($exe,('-Q -T -s "'+$case.script+'"'),($prefix+'.log'),$plan.cwd,180000)
        }
        [NativeMeter]::Finish($measurement)
        $status=if($measurement.exit -ne 0){'MAPPING_FAILED'}elseif(-not $measurement.final_peak_read_succeeded){'MEMORY_UNVERIFIED'}else{'PASS'}
        $row=[ordered]@{policy=$session.policy;group=$group;case=$case.case;top=$case.top;arm=$arm;phase=$phase;index=$index;status=$status;elapsed_seconds=$duration.TotalSeconds;measurement=$measurement;prefix=$prefix;script=$case.script}
        if($measurement.exit -eq 0){
            foreach($extension in @('il','json')){Move-Item -LiteralPath ($case.output_prefix+'.'+$extension) -Destination ($prefix+'.'+$extension)}
        }
    }catch{
        $row=[ordered]@{policy=$session.policy;group=$group;case=$case.case;arm=$arm;phase=$phase;index=$index;status='ERROR';error=$_.Exception.ToString();prefix=$prefix}
    }
    $records.Add($row)
    [IO.File]::AppendAllText((Join-Path $session.root 'RUNS.jsonl'),(($row|ConvertTo-Json -Depth 14 -Compress)+[Environment]::NewLine),$utf8)
    return $row
}
$context=[ordered]@{started=(Get-Date).ToString('o');antivirus_changed=$false;formal_finished=$formal.all_checks_pass;formal_passed=$formal.positive_passed;windows=[Environment]::OSVersion.VersionString;pwsh=$PSVersionTable.PSVersion.ToString();defender_query='unavailable';running_word_processes=@((Get-Process WINWORD -ErrorAction SilentlyContinue).Id)}
try{
    $mp=Get-MpComputerStatus -ErrorAction Stop
    $context.defender_query='success'
    $context.defender=@{antivirus_enabled=$mp.AntivirusEnabled;realtime_protection_enabled=$mp.RealTimeProtectionEnabled;amservice_enabled=$mp.AMServiceEnabled}
}catch{$context.defender_error=$_.Exception.Message}
Save-Json (Join-Path $root 'NATIVE_CONTEXT.json') $context
Push-Location $plan.cwd
try{
    foreach($session in $plan.sessions){
        $runsFile=Join-Path $session.root 'RUNS.jsonl'
        if(Test-Path -LiteralPath $runsFile){throw 'Do not rerun or append to an existing session.'}
        $records=[System.Collections.Generic.List[object]]::new()
        $decisions=[System.Collections.Generic.List[object]]::new()
        Write-Output ('Session '+$session.policy+': one warmup phase, '+$session.warmup_runs+' original runs')
        foreach($case in $session.warmups){
            for($index=0;$index -lt 10;$index++){$null=Invoke-Case $session 0 $case 'baseline' 'warmup' $index}
        }
        foreach($group in $session.groups){
            foreach($case in $group.cases){
                $base=Invoke-Case $session $group.group $case 'baseline' 'initial' 0
                $candidate=Invoke-Case $session $group.group $case 'candidate' 'initial' 0
                $valid=$base.status -eq 'PASS' -and $candidate.status -eq 'PASS'
                $decision=[ordered]@{policy=$session.policy;group=$group.group;case=$case.case;initial_valid=$valid;review_triggered=$false;review_pairs=0}
                if($valid){
                    $decision.initial_time_ratio=$candidate.elapsed_seconds/$base.elapsed_seconds
                    $decision.initial_memory_ratio=$candidate.measurement.main_peak_working_set_bytes/$base.measurement.main_peak_working_set_bytes
                    if($decision.initial_time_ratio -gt 1.05 -and $base.elapsed_seconds -le 10.0 -and $candidate.elapsed_seconds -le 10.0){
                        $decision.review_triggered=$true
                        for($review=0;$review -lt 5;$review++){
                            $null=Invoke-Case $session $group.group $case 'baseline' 'review' $review
                            $null=Invoke-Case $session $group.group $case 'candidate' 'review' $review
                        }
                        $decision.review_pairs=5
                    }
                }
                $decisions.Add($decision)
                Save-Json (Join-Path $session.root 'DECISIONS.json') ($decisions.ToArray())
                Write-Output ($session.policy+' group='+$group.group+' '+$case.case+' ratio='+$decision.initial_time_ratio+' review='+$decision.review_triggered)
            }
        }
        Save-Json (Join-Path $session.root 'RUNS.json') ($records.ToArray())
        Write-Output ('Completed '+$session.policy+': '+$records.Count+' native runs')
    }
    Save-Json (Join-Path $root 'NATIVE_COMPLETE.json') @{complete=$true;finished=(Get-Date).ToString('o');sessions=1;groups=$plan.sessions[0].groups.Count;judgments=($plan.sessions[0].groups.Count*$plan.sessions[0].warmups.Count);not_official_commit_rebuilt=$true;antivirus_changed=$false}
}finally{
    Pop-Location
    $env:ABC=$beforeAbc
    $env:YOSYS_DATDIR=$beforeData
}
