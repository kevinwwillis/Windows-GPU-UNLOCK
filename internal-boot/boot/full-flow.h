/* GPL-2.0-only. All execution gates shared by real EFI and host tests. */
#pragma once
#include "prepare-console.h"
typedef struct {
  VOID *Context;
  EFI_STATUS (*Prepare)(VOID *Context);
  EFI_STATUS (*Choose)(VOID *Context,BOOLEAN *Skip);
  EFI_STATUS (*Capture)(VOID *Context);
  EFI_STATUS (*Release)(VOID *Context);
  EFI_STATUS (*VerifyCold)(VOID *Context);
  EFI_STATUS (*Unlock)(VOID *Context);
  EFI_STATUS (*WindowsSkip)(VOID *Context);
  EFI_STATUS (*WindowsAfter)(VOID *Context);
  GU_PREPARE_LOG Record;
} GU_FULL_FLOW_OPS;
EFI_STATUS GuFullFlow(const GU_FULL_FLOW_OPS *Ops);
/* Firmware fallback is permissible only before any display-release attempt. */
static inline BOOLEAN GuMayReturnToFirmware(BOOLEAN ReleaseStarted) { return !ReleaseStarted; }
