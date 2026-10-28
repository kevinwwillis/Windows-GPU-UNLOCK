# TASK-22.6. Put the pre-step in front of an EFI image on the USB stick:
#   EFI\BOOT\BOOTX64.EFI = PRESTEP-<Mode>.EFI, EFI\BOOT\NEXT.EFI = -Next (the probe or unlock wrapper).
# -Mode Probe only in front of the project's read-only probe (its build manifest's hash): a probe
# pre-step always starts NEXT.EFI, so in front of the unlock wrapper it would defeat the gate.
# -Mode Unlock in front of the unlock wrapper. The project's own stage-usb.ps1 /
# stage-observe-usb.ps1 / new-machine.ps1 overwrite BOOTX64.EFI: run this again after any of them.
# -Next defaults to the image currently at BOOTX64.EFI. A pre-step (any version, found by content)
# is never staged as NEXT.EFI. Previous files are kept in RTX3090-UNLOCK\backup\ by hash; every
# copy is flushed to the stick and verified. Files only; touches nothing else.
param([Parameter(Mandatory)][ValidateSet('Probe','Unlock')][string]$Mode,[ValidatePattern('^[D-Zd-z]$')][string]$Drive='D',[string]$Next,
      [string]$Repo='C:\temp\gpu-unlock-3090')
$ErrorActionPreference='Stop'
$vol=Get-Volume -DriveLetter $Drive;$part=Get-Partition -DriveLetter $Drive;$disk=Get-Disk -Number $part.DiskNumber
if($disk.BusType -ne 'USB' -or $vol.FileSystem -ne 'FAT32'){throw "$($Drive): is not a FAT32 USB stick"}
$root="$($Drive.ToUpperInvariant()):";$boot=Join-Path $root 'EFI\BOOT';$bak=Join-Path $root 'RTX3090-UNLOCK\backup'
function Sha($p){(Get-FileHash -LiteralPath $p).Hash.ToLowerInvariant()}
function Pinned($name){(Get-Content -LiteralPath (Join-Path $PSScriptRoot "build\$name.sha256")).Split(' ')[0]}
function IsPrestep($p){[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($p)).Contains('prestep TASK-22.6')}
$pre=Join-Path $PSScriptRoot "build\PRESTEP-$($Mode.ToUpperInvariant()).EFI"
$preHash=Pinned "PRESTEP-$($Mode.ToUpperInvariant()).EFI"
if((Sha $pre) -ne $preHash){throw "$pre does not match its recorded hash"}
$bootFile=Join-Path $boot 'BOOTX64.EFI';$nextFile=Join-Path $boot 'NEXT.EFI'
if(!$Next){ if(IsPrestep $bootFile){throw 'BOOTX64.EFI is already a pre-step; pass -Next explicitly'}; $Next=$bootFile }
if(IsPrestep $Next){throw "$Next is a pre-step; it cannot be NEXT.EFI"}
$nextHash=Sha $Next
if($Mode -eq 'Probe'){
  $probeHash=(Get-Content -Raw -LiteralPath (Join-Path $Repo 'internal-boot\boot\build\probe-generic\manifest.json') | ConvertFrom-Json).EfiSha256
  if($nextHash -ne $probeHash){throw "-Mode Probe needs the read-only probe ($($probeHash.Substring(0,12))) as NEXT.EFI; got $($nextHash.Substring(0,12))"}
}
New-Item -ItemType Directory -Force $bak | Out-Null
foreach($f in $bootFile,$nextFile){ if(Test-Path -LiteralPath $f){ $h=Sha $f
  $k=Join-Path $bak ((Split-Path $f -Leaf).Replace('.EFI','')+'-'+$h.Substring(0,12)+'.EFI')
  if(!(Test-Path -LiteralPath $k)){Copy-Item -LiteralPath $f -Destination $k}; if((Sha $k) -ne $h){throw "Backup verify failed: $k"}
  "kept $(Split-Path $f -Leaf) ($($h.Substring(0,12))) as $k" } }
$tmp=Join-Path $bak ('next-staging-'+$nextHash.Substring(0,12)+'.EFI'); Copy-Item -LiteralPath $Next -Destination $tmp -Force
Copy-Item -LiteralPath $tmp -Destination $nextFile -Force
Copy-Item -LiteralPath $pre -Destination $bootFile -Force
Write-VolumeCache -DriveLetter $Drive
if((Sha $nextFile) -ne $nextHash -or (Sha $bootFile) -ne $preHash){throw 'Staged files do not verify'}
"staged: BOOTX64.EFI = pre-step $Mode ($($preHash.Substring(0,12))), NEXT.EFI = $($nextHash.Substring(0,12))"
