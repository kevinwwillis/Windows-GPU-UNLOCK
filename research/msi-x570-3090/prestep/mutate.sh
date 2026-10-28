# TASK-22.6 mutation check (WSL): each mutant breaks one guard; the test suites must fail.
R=/mnt/c/temp/gpu-unlock-3090
E=$R/deps/edk2/MdePkg/Include
[ "$(git -C $R rev-parse HEAD)" = 20c44fecac13cdf601b6a9357a46d8fea29f2678 ] || { echo "project clone is not at 20c44fe"; exit 1; }
P=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d /tmp/prestep-mut.XXXXXX)
F="-std=gnu11 -w -fshort-wchar -I $E -I $E/X64 -I $O -I $R"
run() { # name file sed-expr
  cp $P/*.c $P/*.h $O/; sed -i "$3" $O/$2
  if cmp -s $P/$2 $O/$2; then echo "$1: DID NOT APPLY"; return; fi
  ok=0
  gcc $F $O/decode-sync.c $O/test_decode_sync.c -o $O/u 2>/dev/null && $O/u >/dev/null 2>&1 || ok=1
  for g in 0 1; do gcc $F -DPRESTEP_GATE=$g $O/decode-sync.c $O/test_prestep.c -o $O/h 2>/dev/null && $O/h >/dev/null 2>&1 || ok=1; done
  [ $ok = 1 ] && echo "$1: killed" || echo "$1: SURVIVED"
}
run D01-gate-not-exact-cmd   decode-sync.c 's/command != DS_OBSERVED_COMMAND || record != DS_OBSERVED_RECORD) return DS_UNEXPECTED_STATE/(command \& 7) != DS_OBSERVED_COMMAND || record != DS_OBSERVED_RECORD) return DS_UNEXPECTED_STATE/'
run D02-gate-not-exact-rec   decode-sync.c 's/command != DS_OBSERVED_COMMAND || record != DS_OBSERVED_RECORD) return DS_UNEXPECTED_STATE/command != DS_OBSERVED_COMMAND || (record \& DS_DECODE) != DS_OBSERVED_RECORD) return DS_UNEXPECTED_STATE/'
run D03-no-supported-check   decode-sync.c 's/if (!(supported \& DS_ATTR_IO)) return DS_UNEXPECTED_STATE;//'
run D04-fixed-ignores-record decode-sync.c 's/if (command == (DS_OBSERVED_COMMAND | 1) \&\& record == DS_OBSERVED_RECORD) return DS_FIXED_IO_ON;/if (command == (DS_OBSERVED_COMMAND | 1)) return DS_FIXED_IO_ON;/'
run D05-no-readback-on-error decode-sync.c 's/if (s != EFI_SUCCESS) return Failed(o);/if (s != EFI_SUCCESS) return DS_ERROR;/'
run D06-fallback-ignores-rec decode-sync.c 's/if (command == DS_OBSERVED_COMMAND \&\& record == (DS_OBSERVED_RECORD \& ~DS_ATTR_IO)) return/if (command == DS_OBSERVED_COMMAND) return/'
run D07-no-0xffff            decode-sync.c 's/ || \*Command == 0xffff//'
run D08-consistent-no-bme    decode-sync.c 's/(Command \& 4 ? DS_ATTR_BUS_MASTER : 0)/0/'
run D09-delta-ignores-status decode-sync.c 's/if (i == 1 ? diff != AllowedCommandBits : diff != 0) return FALSE;/if (i == 1 ? (diff \& 0xffff) != AllowedCommandBits : diff != 0) return FALSE;/'
run P01-gate-ignores-ok      prestep.c     's/if (PRESTEP_GATE \&\& (!ok || !LogOk))/if (PRESTEP_GATE \&\& !LogOk)/'
run P02-no-recheck-compare   prestep.c     's/cmd == (g1\[1\] \& 0xffff) \&\& DsConsistent(cmd, rec)/DsConsistent(g1[1] \& 0xffff, rec)/'
run P03-mutate-without-log   prestep.c     's/if (!LogOk) {/if (0) {/'
run P04-delta-not-required   prestep.c     's/ok = deltas \&\& stable/ok = stable/'
run P05-bridge-not-compared  prestep.c     's/ \&\& DsConfigDeltaOk(b0, b1, 0)//'
run P06-no-close-bridge      prestep.c     's/ClosePick(Image, \&gpu); ClosePick(Image, \&bridge);/ClosePick(Image, \&gpu);/'
run D10-enable-log-ignored    decode-sync.c 's/if (!o->Log(o->Ctx, "about-to-enable-io", command, record)) return DS_LOG_FAILED;/o->Log(o->Ctx, "about-to-enable-io", command, record);/'
run D11-disable-log-ignored   decode-sync.c 's/if (!o->Log(o->Ctx, "about-to-disable-io", command, record)) return DS_LOG_FAILED;/o->Log(o->Ctx, "about-to-disable-io", command, record);/'
run P07-sync-without-log      prestep.c     's/r = LogOk ? DsSync(\&ops) : DS_LOG_FAILED;/r = DsSync(\&ops);/'
run P08-no-flush              prestep.c     's/ || LogFile->Flush(LogFile) != EFI_SUCCESS//'
run P09-gate-ignores-logok    prestep.c     's/if (PRESTEP_GATE \&\& (!ok || !LogOk))/if (PRESTEP_GATE \&\& !ok)/'
run P10-any-result-accepted   prestep.c     's/ \&\& (r == DS_ALREADY_CONSISTENT || r == DS_FIXED_IO_ON || r == DS_FIXED_RECORD_IO_OFF)//'
run P11-after-snapshot-status prestep.c     's/BOOLEAN deltas = s0 == EFI_SUCCESS \&\& s1 == EFI_SUCCESS \&\&/BOOLEAN deltas =/'
run P12-bridge-any-header     prestep.c     's/((hdr >> 16) \& 0x7f) == 1 \&\& //'
run P13-gpu-any-class         prestep.c     's/ \&\& cls == TARGET_CLASS_REV//'
run P14-recheck-no-consistent prestep.c     's/ \&\& DsConsistent(cmd, rec))) stable = FALSE;/)) stable = FALSE;/'
run P15-one-recheck-only      prestep.c     's/for (UINTN i = 0; i < 4; i++) {/for (UINTN i = 0; i < 1; i++) {/'
run P16-no-self-check         prestep.c     's/if (LoadedAsNext(self->FilePath)) {/if (0) {/'
run P17-recheck-status        prestep.c     's/if (!(rs == EFI_SUCCESS \&\& as == EFI_SUCCESS \&\& /if (!(/'
run P18-recheck-not-sticky    prestep.c     's/))) stable = FALSE;/))) stable = FALSE; else stable = TRUE;/'
rm -rf -- "${O:?}"
