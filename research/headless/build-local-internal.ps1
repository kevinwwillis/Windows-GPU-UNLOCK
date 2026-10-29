# GPL-2.0-only. Experimental LOCAL internal build; does not stage or register EFI.
param([Parameter(Mandatory)][guid]$InternalEspGuid,[Parameter(Mandatory)][guid]$WindowsEspGuid,
 [string]$Clang='clang',[string]$LldLink='lld-link',[string]$Python='python',
 [ValidatePattern('^[A-Za-z0-9_.-]+$')][string]$WslDistro='Ubuntu-24.04')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$boot=Join-Path $repo 'internal-boot/boot';$unlock=Join-Path $repo 'internal-boot/unlock'
. (Join-Path $unlock 'local-profile.ps1')
if($InternalEspGuid -eq [guid]::Empty -or $WindowsEspGuid -eq [guid]::Empty -or $InternalEspGuid -eq $WindowsEspGuid){throw 'Two distinct nonzero GPT partition GUIDs are required'}
$parts=@(Get-Partition)
$internal=@($parts|Where-Object {$_.Guid -and [guid]$_.Guid -eq $InternalEspGuid})
$windows=@($parts|Where-Object {$_.Guid -and [guid]$_.Guid -eq $WindowsEspGuid})
if($internal.Count -ne 1 -or $windows.Count -ne 1 -or !$windows[0].IsSystem -or $internal[0].IsSystem){throw 'Partition identities do not identify a separate internal ESP and current Windows ESP'}
$disk=Get-Disk -Number $internal[0].DiskNumber;$volume=$internal[0]|Get-Volume
if($disk.BusType -eq 'USB' -or $disk.PartitionStyle -ne 'GPT' -or $internal[0].GptType -ne '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -or $internal[0].Size -lt 1GB -or $volume.FileSystem -ne 'FAT32'){throw 'Local internal media must be a separate non-USB GPT FAT32 ESP of at least 1 GiB'}
function GuidC([guid]$value){$b=$value.ToByteArray();$tail=($b[8..15]|ForEach-Object {'0x{0:x2}' -f $_}) -join ',';return ('{{0x{0:x8},0x{1:x4},0x{2:x4},{{{3}}}}}' -f [BitConverter]::ToUInt32($b,0),[BitConverter]::ToUInt16($b,4),[BitConverter]::ToUInt16($b,6),$tail)}
$local=Get-LocalProfile $repo;$evidence=Assert-LocalProfileEvidence $repo
& (Join-Path $PSScriptRoot 'test-local-internal-guard.ps1')
if(!$?){throw 'Internal policy negative tests failed'}
$config=Join-Path $boot 'platform-config.h'
@('/* Local internal partition configuration; never distribute. */',('#define GU_INTERNAL_PARTITION_INITIALIZER '+(GuidC $InternalEspGuid)),('#define GU_WINDOWS_PARTITION_INITIALIZER '+(GuidC $WindowsEspGuid)))|Set-Content -LiteralPath $config -Encoding ascii
$testPath=Join-Path $repo 'research/msi-x570-3090/prestep/test-internal-prestep.sh'
$linuxPath=(& wsl.exe -d $WslDistro -- wslpath -a ($testPath.Replace('\','/')) | Out-String).Trim()
if($LASTEXITCODE -ne 0 -or !$linuxPath.StartsWith('/')){throw 'Cannot resolve internal pre-step tests in WSL'}
& wsl.exe -d $WslDistro -- bash $linuxPath
if($LASTEXITCODE -ne 0){throw 'Internal pre-step mock tests failed'}
& (Join-Path $unlock 'build.ps1') -ExperimentalUnlock -StockProfile entryLatchedSs1Full -InternalBoot -AutomaticBoot -HeadlessResearch -ExperimentalLocalInternal -HardwareProfile local -Clang $Clang -LldLink $LldLink -Python $Python
if(!$?){throw 'Child build failed'}
$child=Join-Path $unlock 'build/experimental/entryLatchedSs1Full/internal/automatic/headless-research/RTX3090-EXPERIMENTAL-entryLatchedSs1Full-INTERNAL-AUTO-HEADLESS-RESEARCH.EFI'
$manifest=Get-Content -Raw -LiteralPath (Join-Path (Split-Path $child) 'build-manifest.json')|ConvertFrom-Json
$hash=Get-GuSha256 $child
if(!$manifest.CanArm -or $manifest.HardwareProfile -ne $local.Name -or $manifest.EfiSha256 -ne $hash -or $manifest.Defines -contains '-DGU_USB_MEDIA'){throw 'Child contract mismatch'}
Push-Location -LiteralPath $boot
try {
 [void][IO.Directory]::CreateDirectory((Join-Path $boot 'build/headless'))
 @('/* Internal local child contract; GPL-2.0-only. */',('#define GU_UNLOCK_SHA256 "'+$hash+'"'),('#define GU_UNLOCK_BYTES '+(Get-Item $child).Length))|Set-Content 'build/headless/child-contract.h' -Encoding ascii
 $sources=@('handoff.c','full-release.c','full-flow.c','diagnostic.c','../unlock/checks.c','../unlock/sha256.c','../efi_runtime.c');$objects=@()
 foreach($source in $sources){$obj='build/headless/'+[IO.Path]::GetFileNameWithoutExtension($source)+'.obj'; & $Clang -target x86_64-pc-windows-msvc -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -fshort-wchar -Os -Wall -Wextra -Werror -DGU_HEADLESS_RESEARCH -DGU_SINGLE_RTX_CONSOLE -DGU_FULL_UNLOCK -DGU_AUTOMATIC_BOOT -DGU_PROFILE_LOCAL -DGU_INTERNAL_LOCAL -I ../../deps/edk2/MdePkg/Include -I ../../deps/edk2/MdePkg/Include/X64 -c $source -o $obj; if($LASTEXITCODE -ne 0){throw "Wrapper compile failed: $source"};$objects+=$obj}
 & $LldLink /nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /machine:x64 /base:0 /dynamicbase /nxcompat /timestamp:0 /opt:ref /opt:icf /map:build/headless/HEADLESS.map /out:build/headless/HEADLESS.EFI @objects
 if($LASTEXITCODE -ne 0){throw 'Wrapper link failed'}
 & $Python -I ../unlock/verify_pe.py build/headless/HEADLESS.EFI build/headless/HEADLESS.map --output build/headless/pe-verification.json
 if($LASTEXITCODE -ne 0){throw 'Wrapper verification failed'}
 $pre=Join-Path $repo 'research/msi-x570-3090/prestep';$preOut=Join-Path $pre 'build/internal-local'
 [void][IO.Directory]::CreateDirectory($preOut);$preObjects=@()
 foreach($source in @('prestep.c','decode-sync.c')){
  $obj=Join-Path $preOut ([IO.Path]::GetFileNameWithoutExtension($source)+'.obj')
  & $Clang -target x86_64-pc-windows-msvc -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -fshort-wchar -Os -Wall -Wextra -Werror -DPRESTEP_GATE=1 -DPRESTEP_INTERNAL=1 -I ../../deps/edk2/MdePkg/Include -I ../../deps/edk2/MdePkg/Include/X64 -I $pre -c (Join-Path $pre $source) -o $obj
  if($LASTEXITCODE -ne 0){throw "Prestep compile failed: $source"};$preObjects+=$obj
 }
 $preEfi=Join-Path $preOut 'PRESTEP-INTERNAL.EFI';$preMap=Join-Path $preOut 'PRESTEP-INTERNAL.map'
 & $LldLink /nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /machine:x64 /base:0 /dynamicbase /nxcompat /timestamp:0 /opt:ref /opt:icf "/map:$preMap" "/out:$preEfi" @preObjects
 if($LASTEXITCODE -ne 0){throw 'Prestep link failed'}
 & $Python -I ../unlock/verify_pe.py $preEfi $preMap --output (Join-Path $preOut 'pe-verification.json')
 if($LASTEXITCODE -ne 0){throw 'Prestep verification failed'}
 $after=Assert-LocalProfileEvidence $repo
 if($after.TreeSha256 -ne $evidence.TreeSha256 -or (Get-LocalProfile $repo).HeaderSha256 -ne $local.HeaderSha256){throw 'Inputs changed during build'}
 [ordered]@{Mode='experimental-local-internal';HardwareTested=$false;HardwareProfile=$local.Name;InternalEspGuid=$InternalEspGuid.ToString();WindowsEspGuid=$WindowsEspGuid.ToString();ChildSha256=$hash;WrapperSha256=(Get-GuSha256 'build/headless/HEADLESS.EFI');PrestepSha256=(Get-GuSha256 $preEfi);PartitionConfigSha256=(Get-GuSha256 $config);HeaderSha256=$local.HeaderSha256;EvidenceTreeSha256=$evidence.TreeSha256}|ConvertTo-Json|Set-Content 'build/headless/local-internal-manifest.json' -Encoding utf8
}finally{Pop-Location}
