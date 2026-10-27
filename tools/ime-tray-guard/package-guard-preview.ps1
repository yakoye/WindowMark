param([Parameter(Mandatory=$true)][string]$SourceDirectory,[Parameter(Mandatory=$true)][string]$Name)
$ErrorActionPreference='Stop'
if($Name -notmatch '^ime-guard-preview-[a-zA-Z0-9.-]+$'){throw 'Invalid preview name'}
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source=(Resolve-Path -LiteralPath $SourceDirectory).Path
$stage=Join-Path $root ('dist/test/ime-tray-guard/'+$Name)
if(Test-Path -LiteralPath $stage){throw 'Package exists; never overwrite tested evidence'}
$null=New-Item -ItemType Directory -Path $stage
foreach($file in @('WindowMarkImeTrayGuard.exe','WindowMarkXamlContainerTrial.exe','WindowMarkXamlContainerTap.dll')){
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination (Join-Path $stage $file)
    if((Get-FileHash -LiteralPath (Join-Path $source $file)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $stage $file)).Hash){throw 'Build hash mismatch'}
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'GUARD-PREVIEW-README.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run-guard-preview.bat') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'docs/tests/IME-Tray-Guard-Crash-Investigation.md') -Destination (Join-Path $stage 'TEST-REPORT.md')
Copy-Item -LiteralPath (Join-Path $root 'docs/features/IME-Tray-Guard-Plan.md') -Destination (Join-Path $stage 'PLAN.md')
$files=@(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name)
$hashes=@($files | ForEach-Object {'{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant(),$_.Name})
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS'),$hashes,[Text.UTF8Encoding]::new($false))
$zip=$stage+'.zip'
if(Test-Path -LiteralPath $zip){throw 'ZIP exists'}
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
$destinations=@(
    ('//192.168.7.100/Downloads/share_chatgpt/WindowMark-IME-Tray-Guard/'+$Name),
    ('C:/Users/color/Downloads/yakoye-release/WindowMark/preview/'+$Name),
    ('//192.168.7.100/Downloads/yakoye-release/WindowMark/preview/'+$Name)
)
foreach($destination in $destinations){
    if(Test-Path -LiteralPath $destination){throw "Destination exists: $destination"}
    $null=New-Item -ItemType Directory -Path $destination
    foreach($file in Get-ChildItem -LiteralPath $stage -File){
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $destination $file.Name)
        if((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $destination $file.Name)).Hash){throw "Archive mismatch: $destination"}
    }
    Copy-Item -LiteralPath $zip -Destination $destination
    if((Get-FileHash -LiteralPath $zip).Hash -ne (Get-FileHash -LiteralPath (Join-Path $destination (Split-Path -Leaf $zip))).Hash){throw 'ZIP archive mismatch'}
    Write-Output "Verified preview archive: $destination"
}
Write-Output "Local package: $stage"
