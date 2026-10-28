# TASK-22.6. Build the I/O-decode pre-step EFI with the Windows-GPU-UNLOCK toolchain and flags
# (clang/lld-link 20.1.8, the project's pinned EDK2 headers). Outputs, each with a .sha256:
#   build\PRESTEP-PROBE.EFI  (PRESTEP_GATE=0: always starts NEXT.EFI, for probe captures)
#   build\PRESTEP-UNLOCK.EFI (PRESTEP_GATE=1: starts NEXT.EFI only after a verified, stable sync)
param([string]$Repo='C:\temp\gpu-unlock-3090',[string]$Clang='C:\Program Files\LLVM\bin\clang.exe',
      [string]$LldLink='C:\Program Files\LLVM\bin\lld-link.exe')
$ErrorActionPreference='Stop'
$head=git -C $Repo rev-parse HEAD
if($head -ne '20c44fecac13cdf601b6a9357a46d8fea29f2678'){throw "$Repo is at $head, not the pinned 20c44fe"}
$inc=Join-Path $Repo 'deps\edk2\MdePkg\Include'
$out=Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force $out | Out-Null
foreach($v in @(@{Name='PROBE';Gate=0},@{Name='UNLOCK';Gate=1})) {
 $objs=@()
 foreach($src in 'prestep.c','decode-sync.c') {
  $obj=Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($src)+'-'+$v.Name+'.obj')
  & $Clang -target x86_64-pc-windows-msvc -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -fshort-wchar -Os -Wall -Wextra -Werror "-DPRESTEP_GATE=$($v.Gate)" -I $inc -I "$inc\X64" -I $PSScriptRoot -c (Join-Path $PSScriptRoot $src) -o $obj
  if($LASTEXITCODE -ne 0){throw "Compile failed: $src"}
  $objs+=$obj
 }
 $efi=Join-Path $out "PRESTEP-$($v.Name).EFI"
 & $LldLink /nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /machine:x64 /base:0 /dynamicbase /nxcompat /timestamp:0 /opt:ref /opt:icf "/map:$out\PRESTEP-$($v.Name).map" "/out:$efi" @objs
 if($LASTEXITCODE -ne 0){throw 'Link failed'}
 $h=(Get-FileHash -LiteralPath $efi).Hash.ToLowerInvariant()
 "$h  PRESTEP-$($v.Name).EFI" | Set-Content -LiteralPath "$efi.sha256" -Encoding ascii
 "Built $efi ($h)"
}
