/* TASK-22.6. Make the RTX's PCI I/O decode bit and the firmware's PciIo attribute record agree,
 * through the firmware's own PciIo->Attributes, before the probe or unlock wrapper runs.
 * MSI X570 / BIOS 1.Q2 leaves command 0x0002 (I/O off) while the record says 0x300 (I/O on).
 * Acts only on exactly that state; anything else is left untouched. */
#pragma once
#include <Uefi.h>

#define DS_ATTR_IO 0x100ULL
#define DS_ATTR_MEMORY 0x200ULL
#define DS_ATTR_BUS_MASTER 0x400ULL
#define DS_DECODE (DS_ATTR_IO | DS_ATTR_MEMORY | DS_ATTR_BUS_MASTER)
#define DS_OBSERVED_COMMAND 0x0002U
#define DS_OBSERVED_RECORD 0x300ULL

/* Values of EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION (checked in prestep.c). */
typedef enum { DS_GET = 0, DS_ENABLE = 2, DS_DISABLE = 3, DS_SUPPORTED = 4 } DS_OP;

typedef struct {
  VOID *Ctx;
  EFI_STATUS (*ReadCommand)(VOID *Ctx, UINT16 *Command);
  EFI_STATUS (*Attributes)(VOID *Ctx, DS_OP Op, UINT64 Bits, UINT64 *Result);
  /* Returns FALSE once any log write has failed; DsSync then makes no further change. */
  BOOLEAN (*Log)(VOID *Ctx, const char *Event, UINT64 A, UINT64 B);
} DS_OPS;

typedef enum {
  DS_ERROR = 0,             /* a firmware call failed; stopped making changes */
  DS_ALREADY_CONSISTENT,    /* nothing to do */
  DS_UNEXPECTED_STATE,      /* not the observed mismatch; left untouched */
  DS_FIXED_IO_ON,           /* command 0x0003, record 0x300 */
  DS_FIXED_RECORD_IO_OFF,   /* command 0x0002, record 0x200 */
  DS_STILL_INCONSISTENT,    /* calls succeeded but the state is not one of the above */
  DS_LOG_FAILED             /* the log could not record the next change; it was not made */
} DS_RESULT;

DS_RESULT DsSync(const DS_OPS *Ops);
/* Record's IO/MEM/BME bits equal the command register's. */
BOOLEAN DsConsistent(UINT16 Command, UINT64 Record);
/* 64-dword config snapshots are identical except that the command word may differ by exactly
 * AllowedCommandBits (0 = no change allowed). The status word must be identical too. */
BOOLEAN DsConfigDeltaOk(const UINT32 *Before, const UINT32 *After, UINT16 AllowedCommandBits);
