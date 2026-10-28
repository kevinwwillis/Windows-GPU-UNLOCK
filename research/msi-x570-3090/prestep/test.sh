# TASK-22.6 host tests (run in WSL): unit tests for decode-sync.c and the mock-UEFI harness for
# prestep.c in both gate modes, under ASan/UBSan. Needs the project clone for EDK2 headers.
set -e
R=/mnt/c/temp/gpu-unlock-3090
E=$R/deps/edk2/MdePkg/Include
[ "$(git -C $R rev-parse HEAD)" = 20c44fecac13cdf601b6a9357a46d8fea29f2678 ] || { echo "project clone is not at 20c44fe"; exit 1; }
P=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d /tmp/prestep-test.XXXXXX)
F="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -fshort-wchar -fsanitize=address,undefined -fno-sanitize-recover=all -I $E -I $E/X64 -I $P -I $R"
rc=0
gcc $F $P/decode-sync.c $P/test_decode_sync.c -o $O/unit && $O/unit || rc=1
for g in 0 1; do gcc $F -Wno-misleading-indentation -DPRESTEP_GATE=$g $P/decode-sync.c $P/test_prestep.c -o $O/h$g && $O/h$g || rc=1; done
rm -rf -- "${O:?}"
exit $rc
