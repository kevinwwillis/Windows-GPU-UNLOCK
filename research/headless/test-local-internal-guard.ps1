# GPL-2.0-only. Negative build-policy checks; never accesses the GPU or stages EFI.
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$build=Join-Path $PSScriptRoot 'build-local-internal.ps1'
$child=Join-Path $repo 'internal-boot/unlock/build.ps1'
$config=Join-Path $repo 'internal-boot/boot/platform-config.h'
$before=if(Test-Path $config){(Get-FileHash $config).Hash}else{$null}
$a=[guid]'11111111-1111-1111-1111-111111111111'
$b=[guid]'22222222-2222-2222-2222-222222222222'
$cases=@(
 @{Name='zero internal GUID';Expected='Two distinct nonzero';Action={& $build -InternalEspGuid ([guid]::Empty) -WindowsEspGuid $a}},
 @{Name='same GUID';Expected='Two distinct nonzero';Action={& $build -InternalEspGuid $a -WindowsEspGuid $a}},
 @{Name='unmapped partitions';Expected='Partition identities';Action={& $build -InternalEspGuid $a -WindowsEspGuid $b}},
 @{Name='non-local internal opt-in';Expected='ExperimentalLocalInternal requires';Action={& $child -ExperimentalUnlock -StockProfile entryLatchedSs1Full -InternalBoot -AutomaticBoot -HeadlessResearch -ExperimentalLocalInternal -HardwareProfile b650m-3090fe}},
 @{Name='USB and internal both selected';Expected='ExperimentalLocalInternal requires';Action={& $child -ExperimentalUnlock -StockProfile entryLatchedSs1Full -InternalBoot -AutomaticBoot -HeadlessResearch -UsbMedia -ExperimentalLocalInternal -HardwareProfile b650m-3090fe}}
)
foreach($case in $cases){$rejected=$false;try{& $case.Action}catch{if($_.Exception.Message -like ('*'+$case.Expected+'*')){$rejected=$true}else{throw}};if(!$rejected){throw ($case.Name+' unexpectedly accepted')};Write-Output ('PASS '+$case.Name)}
$after=if(Test-Path $config){(Get-FileHash $config).Hash}else{$null}
if($before -cne $after){throw 'Rejected attempts changed partition configuration'}
