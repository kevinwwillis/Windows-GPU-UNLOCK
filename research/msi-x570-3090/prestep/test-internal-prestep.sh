#!/usr/bin/env bash
# GPL-2.0-only. Mock UEFI only; does not access the GPU or firmware.
set -eu
pre=$(cd -- "$(dirname -- "$0")" && pwd -P)
repo=$(cd -- "$pre/../../.." && pwd -P)
inc="$repo/deps/edk2/MdePkg/Include"
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
flags=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -fshort-wchar -fsanitize=address,undefined -fno-sanitize-recover=all -I "$inc" -I "$inc/X64" -I "$pre" -I "$repo")
gcc "${flags[@]}" "$pre/decode-sync.c" "$pre/test_decode_sync.c" -o "$out/unit"
"$out/unit"
for gate in 0 1; do
  gcc "${flags[@]}" -Wno-misleading-indentation "-DPRESTEP_GATE=$gate" "$pre/decode-sync.c" "$pre/test_prestep.c" -o "$out/usb-$gate"
  "$out/usb-$gate"
done
gcc "${flags[@]}" -Wno-misleading-indentation -DPRESTEP_GATE=1 -DPRESTEP_INTERNAL=1 "$pre/decode-sync.c" "$pre/test_prestep.c" -o "$out/internal"
"$out/internal"
