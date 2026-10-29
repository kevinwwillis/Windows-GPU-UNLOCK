# GPL-2.0-only. Stage an exact local internal build; never changes firmware boot entries.
param([Parameter(Mandatory)][ValidatePattern('^[D-Zd-z]$')][string]$Drive,
 [Parameter(Mandatory)][string]$Assets)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repo 'internal-boot/unlock/local-profile.ps1')
$manifest=Get-Content -Raw -LiteralPath (Join-Path $repo 'internal-boot/boot/build/headless/local-internal-manifest.json')|ConvertFrom-Json
$profile=Get-LocalProfile $repo;$evidence=Assert-LocalProfileEvidence $repo
if($manifest.Mode -ne 'experimental-local-internal' -or $manifest.HardwareTested -ne $false -or $manifest.HardwareProfile -ne $profile.Name -or $manifest.HeaderSha256 -ne $profile.HeaderSha256 -or $manifest.EvidenceTreeSha256 -ne $evidence.TreeSha256){throw 'Local profile or test evidence differs from build'}
$part=Get-Partition -DriveLetter $Drive;$disk=Get-Disk -Number $part.DiskNumber;$volume=$part|Get-Volume
if($disk.BusType -eq 'USB' -or $disk.PartitionStyle -ne 'GPT' -or $part.IsSystem -or $part.GptType -ne '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -or $part.Size -lt 1GB -or $volume.FileSystem -ne 'FAT32' -or [guid]$part.Guid -ne [guid]$manifest.InternalEspGuid){throw 'Target is not the pinned dedicated internal FAT32 ESP'}
$windows=@(Get-Partition|Where-Object {$_.Guid -and [guid]$_.Guid -eq [guid]$manifest.WindowsEspGuid})
if($windows.Count -ne 1 -or !$windows[0].IsSystem -or [guid]$part.Guid -eq [guid]$manifest.WindowsEspGuid){throw 'Windows ESP identity changed'}
if((Get-GuSha256 (Join-Path $repo 'internal-boot/boot/platform-config.h')) -ne $manifest.PartitionConfigSha256){throw 'Partition configuration changed'}
$files=@(
 @{Source=(Join-Path $repo 'internal-boot/boot/build/headless/HEADLESS.EFI');Dest='EFI/BOOT/NEXT.EFI';Hash=$manifest.WrapperSha256},
 @{Source=(Join-Path $repo 'internal-boot/unlock/build/experimental/entryLatchedSs1Full/internal/automatic/headless-research/RTX3090-EXPERIMENTAL-entryLatchedSs1Full-INTERNAL-AUTO-HEADLESS-RESEARCH.EFI');Dest='EFI/WindowsGPUUnlock/UNLOCK-HEADLESS.EFI';Hash=$manifest.ChildSha256}
)
foreach($a in (Get-Content -Raw -LiteralPath (Join-Path $repo 'internal-boot/unlock/assets.json')|ConvertFrom-Json)){$files+=@{Source=(Join-Path $Assets $a.Name);Dest=('RTX3090-UNLOCK/ASSETS/'+$a.Name);Hash=$a.Sha256}}
$boot=@{Source=(Join-Path $repo 'research/msi-x570-3090/prestep/build/internal-local/PRESTEP-INTERNAL.EFI');Dest='EFI/BOOT/BOOTX64.EFI';Hash=$manifest.PrestepSha256}
$root=$Drive.ToUpperInvariant()+':\'
foreach($f in @($files)+@($boot)){
 if(!(Test-Path -LiteralPath $f.Source -PathType Leaf) -or (Get-GuSha256 $f.Source) -ne $f.Hash){throw ('Missing or mismatched source: '+$f.Dest)}
 if(Test-Path -LiteralPath (Join-Path $root $f.Dest)){throw ('Destination already exists; preserve it before staging: '+$f.Dest)}
}
foreach($f in @($files)+@($boot)){
 $target=Join-Path $root $f.Dest;[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
 [IO.File]::Copy($f.Source,$target,$false)
 if((Get-GuSha256 $target) -ne $f.Hash){throw ('Staged readback mismatch: '+$f.Dest)}
 Write-Output ('Staged and verified '+$f.Dest)
}
Write-VolumeCache -DriveLetter $Drive
Write-Output 'Files staged; no firmware entry registered and boot priority unchanged.'
