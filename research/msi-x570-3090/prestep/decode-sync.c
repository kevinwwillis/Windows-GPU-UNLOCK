/* TASK-22.6. Core of the I/O-decode pre-step; no UEFI services, host-testable. */
#include "decode-sync.h"

static BOOLEAN Read(const DS_OPS *o, UINT16 *Command, UINT64 *Record) {
  EFI_STATUS s = o->ReadCommand(o->Ctx, Command);
  o->Log(o->Ctx, "read-command", s, *Command);
  if (s != EFI_SUCCESS || *Command == 0xffff) return FALSE;
  s = o->Attributes(o->Ctx, DS_GET, 0, Record);
  o->Log(o->Ctx, "read-record", s, *Record);
  return s == EFI_SUCCESS;
}

BOOLEAN DsConsistent(UINT16 Command, UINT64 Record) {
  UINT64 raw = (Command & 1 ? DS_ATTR_IO : 0) | (Command & 2 ? DS_ATTR_MEMORY : 0) |
               (Command & 4 ? DS_ATTR_BUS_MASTER : 0);
  return (Record & DS_DECODE) == raw;
}

BOOLEAN DsConfigDeltaOk(const UINT32 *Before, const UINT32 *After, UINT16 AllowedCommandBits) {
  for (UINTN i = 0; i < 64; i++) {
    UINT32 diff = Before[i] ^ After[i];
    if (i == 1 ? diff != AllowedCommandBits : diff != 0) return FALSE;
  }
  return TRUE;
}

/* After a failed write call the hardware may still have changed: record what is there now. */
static DS_RESULT Failed(const DS_OPS *o) {
  UINT16 command = 0xffff; UINT64 record = 0;
  (void)Read(o, &command, &record);
  return DS_ERROR;
}

DS_RESULT DsSync(const DS_OPS *o) {
  UINT16 command = 0xffff; UINT64 record = 0, supported = 0;
  if (!Read(o, &command, &record)) return DS_ERROR;
  if (DsConsistent(command, record)) return DS_ALREADY_CONSISTENT;
  if (command != DS_OBSERVED_COMMAND || record != DS_OBSERVED_RECORD) return DS_UNEXPECTED_STATE;
  EFI_STATUS s = o->Attributes(o->Ctx, DS_SUPPORTED, 0, &supported);
  o->Log(o->Ctx, "supported", s, supported);
  if (s != EFI_SUCCESS) return DS_ERROR;
  if (!(supported & DS_ATTR_IO)) return DS_UNEXPECTED_STATE;

  /* Attempt 1: ask the firmware to switch I/O decode on (the proven machines' state). */
  if (!o->Log(o->Ctx, "about-to-enable-io", command, record)) return DS_LOG_FAILED;
  s = o->Attributes(o->Ctx, DS_ENABLE, DS_ATTR_IO, NULL);
  o->Log(o->Ctx, "enable-io", s, 0);
  if (s != EFI_SUCCESS) return Failed(o);
  if (!Read(o, &command, &record)) return DS_ERROR;
  if (command == (DS_OBSERVED_COMMAND | 1) && record == DS_OBSERVED_RECORD) return DS_FIXED_IO_ON;
  if (command != DS_OBSERVED_COMMAND || record != DS_OBSERVED_RECORD)
    return DS_STILL_INCONSISTENT;  /* something else moved; make no further change */

  /* Attempt 2: the bit did not take. Correct the record instead. Expected to rewrite the
   * command register with I/O cleared, the value it already holds; the readback checks that. */
  if (!o->Log(o->Ctx, "about-to-disable-io", command, record)) return DS_LOG_FAILED;
  s = o->Attributes(o->Ctx, DS_DISABLE, DS_ATTR_IO, NULL);
  o->Log(o->Ctx, "disable-io", s, 0);
  if (s != EFI_SUCCESS) return Failed(o);
  if (!Read(o, &command, &record)) return DS_ERROR;
  if (command == DS_OBSERVED_COMMAND && record == (DS_OBSERVED_RECORD & ~DS_ATTR_IO)) return DS_FIXED_RECORD_IO_OFF;
  return DS_STILL_INCONSISTENT;
}
