param([switch]$Full)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$manifest=Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'MANIFEST_SHA256.json') | ConvertFrom-Json
$count=0
foreach($entry in $manifest.files.PSObject.Properties){
    $name=$entry.Name
    if(-not $Full -and $name -notmatch '^(runtime/|inputs/|source/overlay_files/|BUILD.json$|EXPECTED_RESULTS.json$|scripts/)'){continue}
    $file=Join-Path $root $name
    if(-not (Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing file: $name"}
    $stream=[IO.File]::OpenRead($file)
    $hasher=[Security.Cryptography.SHA256]::Create()
    try{$actual=([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-','').ToLowerInvariant()}
    finally{$stream.Dispose();$hasher.Dispose()}
    if($actual -ne $entry.Value){throw "SHA256 mismatch: $name"}
    $count++
}
Write-Host "Verified $count files."
