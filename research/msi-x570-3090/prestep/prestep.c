/* TASK-22.6. USB pre-step: sync the RTX's I/O decode bit with the firmware's PciIo record,
 * log every step to \RTX3090-UNLOCK\PRESTnn.TXT (flushed before any change), then start
 * \EFI\BOOT\NEXT.EFI from the same stick (the project's probe or unlock wrapper, unmodified).
 *
 * PRESTEP_GATE=0 (probe chain): always start NEXT.EFI, so the probe records whatever happened.
 * PRESTEP_GATE=1 (unlock chain): start NEXT.EFI only if the log is written, the sync ended in a
 *   consistent state, nothing else in the GPU's or its bridge's config space changed, and
 *   re-reads at 1, 2, 4 and 8 s still agree. Otherwise return EFI_NOT_STARTED: nothing has been
 *   released, so the firmware continues its boot order (Windows Boot Manager) with the display on.
 *   This matters because the wrapper detaches the only display before its own decode check.
 * Every firmware write is preceded by a flushed log line saying what is about to happen; if that
 * line cannot be written, the write is not made. If this image was itself loaded as NEXT.EFI it
 * returns at once (a mis-staged stick would otherwise start itself over and over). */
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include "decode-sync.h"

#ifndef PRESTEP_GATE
#error "build with -DPRESTEP_GATE=0 (probe chain) or 1 (unlock chain)"
#endif
#define TARGET_BUS 0x2dU
#define TARGET_ID 0x220410deU
#define TARGET_CLASS_REV 0x030000a1U

_Static_assert((int)EfiPciIoAttributeOperationGet == (int)DS_GET && (int)EfiPciIoAttributeOperationEnable == (int)DS_ENABLE &&
               (int)EfiPciIoAttributeOperationDisable == (int)DS_DISABLE && (int)EfiPciIoAttributeOperationSupported == (int)DS_SUPPORTED,
               "DS_OP must match EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION");
_Static_assert(EFI_PCI_IO_ATTRIBUTE_IO == DS_ATTR_IO && EFI_PCI_IO_ATTRIBUTE_MEMORY == DS_ATTR_MEMORY &&
               EFI_PCI_IO_ATTRIBUTE_BUS_MASTER == DS_ATTR_BUS_MASTER, "attribute bits");

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID LoadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID PathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static EFI_SYSTEM_TABLE *St;
static EFI_BOOT_SERVICES *Bs;
static EFI_FILE_PROTOCOL *Root, *LogFile;
static CHAR16 LogName[] = L"\\RTX3090-UNLOCK\\PRESTnn.TXT";
static BOOLEAN LogOk;

static void Screen(const char *s) {
  CHAR16 w[200]; UINTN i = 0;
  for (; s[i] && i < 197; i++) w[i] = (CHAR16)s[i];
  w[i++] = L'\r'; w[i++] = L'\n'; w[i] = 0;
  if (St->ConOut) St->ConOut->OutputString(St->ConOut, w);
}
/* One line to screen and, when open, to the log file, flushed before returning. */
static void Emit(const char *s, UINTN n) {
  char line[640]; if (n > sizeof line - 3) n = sizeof line - 3;
  for (UINTN i = 0; i < n; i++) line[i] = s[i];
  line[n] = 0; Screen(line);
  if (!LogFile) return;
  line[n] = '\r'; line[n + 1] = '\n';
  UINTN size = n + 2;
  if (LogFile->Write(LogFile, &size, line) != EFI_SUCCESS || size != n + 2 || LogFile->Flush(LogFile) != EFI_SUCCESS) {
    LogOk = FALSE; Screen("prestep: LOG WRITE FAILED");
  }
}
typedef struct { char b[640]; UINTN n; } TXT;
static void Put(TXT *t, const char *s) { while (*s && t->n < sizeof t->b) t->b[t->n++] = *s++; }
static void Hex(TXT *t, UINT64 v, int digits) {
  for (int i = digits - 1; i >= 0; i--) if (t->n < sizeof t->b) t->b[t->n++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
}
static void Dec(TXT *t, UINT64 v, int digits) {
  char d[20]; for (int i = digits - 1; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; }
  for (int i = 0; i < digits; i++) if (t->n < sizeof t->b) t->b[t->n++] = d[i];
}
static void Line(const char *event, UINT64 a, UINT64 b) {
  TXT t; t.n = 0; Put(&t, event); Put(&t, " a=0x"); Hex(&t, a, 16); Put(&t, " b=0x"); Hex(&t, b, 16); Emit(t.b, t.n);
}
static void Config(const char *tag, const UINT32 *cfg) {
  TXT t; t.n = 0; Put(&t, tag); Put(&t, " dwords=");
  for (UINTN i = 0; i < 64; i++) { Hex(&t, cfg[i], 8); if (i < 63) Put(&t, " "); }
  Emit(t.b, t.n);
}

static void OpenLog(EFI_HANDLE Device) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  if (Bs->HandleProtocol(Device, &FsGuid, (VOID **)&fs) != EFI_SUCCESS || !fs || fs->OpenVolume(fs, &Root) != EFI_SUCCESS) { Root = NULL; return; }
  for (UINTN n = 0; n < 100; n++) {
    LogName[21] = (CHAR16)(L'0' + n / 10); LogName[22] = (CHAR16)(L'0' + n % 10);
    EFI_FILE_PROTOCOL *old = NULL;
    EFI_STATUS s = Root->Open(Root, &old, LogName, EFI_FILE_MODE_READ, 0);
    if (s == EFI_SUCCESS) { old->Close(old); continue; }
    if (s != EFI_NOT_FOUND) return;
    if (Root->Open(Root, &LogFile, LogName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0) == EFI_SUCCESS) LogOk = TRUE;
    else LogFile = NULL;
    return;
  }
}
static void CloseLog(void) {
  if (LogFile) { LogFile->Close(LogFile); LogFile = NULL; }
  if (Root) { Root->Close(Root); Root = NULL; }
}
/* After NEXT.EFI returns: reopen the same log and append its status. */
static void AppendAfter(EFI_HANDLE Device, const char *event, UINT64 status) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  if (!LogOk || Bs->HandleProtocol(Device, &FsGuid, (VOID **)&fs) != EFI_SUCCESS || !fs || fs->OpenVolume(fs, &Root) != EFI_SUCCESS) { Root = NULL; Line(event, status, 0); return; }
  if (Root->Open(Root, &LogFile, LogName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) != EFI_SUCCESS ||
      LogFile->SetPosition(LogFile, 0xFFFFFFFFFFFFFFFFULL) != EFI_SUCCESS) {
    if (LogFile) LogFile->Close(LogFile);
    LogFile = NULL;
  }
  Line(event, status, 0); CloseLog();
}

typedef struct { EFI_PCI_IO_PROTOCOL *Io; } CTX;
static EFI_STATUS OpRead(VOID *c, UINT16 *cmd) {
  EFI_PCI_IO_PROTOCOL *io = ((CTX *)c)->Io; return io->Pci.Read(io, EfiPciIoWidthUint16, 4, 1, cmd);
}
static EFI_STATUS OpAttr(VOID *c, DS_OP op, UINT64 bits, UINT64 *res) {
  EFI_PCI_IO_PROTOCOL *io = ((CTX *)c)->Io; return io->Attributes(io, (EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION)op, bits, res);
}
static BOOLEAN OpLog(VOID *c, const char *e, UINT64 a, UINT64 b) { (void)c; Line(e, a, b); return LogOk; }

static EFI_STATUS Snapshot(EFI_PCI_IO_PROTOCOL *io, UINT32 *cfg) {
  for (UINTN i = 0; i < 64; i++) cfg[i] = 0xffffffffU;
  return io->Pci.Read(io, EfiPciIoWidthUint32, 0, 64, cfg);
}

/* Exactly one PciIo at 0000:2d:00.0 with the RTX 3090 identity, and exactly one type-1 bridge
 * whose secondary bus is 0x2d; both kept open GET_PROTOCOL by this image (closed by caller). */
typedef struct { EFI_HANDLE Handle; EFI_PCI_IO_PROTOCOL *Io; UINTN Matches; } PICK;
static EFI_STATUS FindTargets(EFI_HANDLE Image, PICK *Gpu, PICK *Bridge) {
  EFI_HANDLE *handles = NULL; UINTN count = 0;
  EFI_STATUS s = Bs->LocateHandleBuffer(ByProtocol, &PciGuid, NULL, &count, &handles);
  Line("pci-handles", s, count);
  if (s != EFI_SUCCESS) return s;
  for (UINTN i = 0; i < count; i++) {
    EFI_PCI_IO_PROTOCOL *io = NULL;
    if (Bs->OpenProtocol(handles[i], &PciGuid, (VOID **)&io, Image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL) != EFI_SUCCESS || !io) continue;
    UINTN seg = 0, bus = 0, dev = 0, fn = 0; UINT32 id = 0, cls = 0, hdr = 0, buses = 0;
    BOOLEAN located = io->GetLocation(io, &seg, &bus, &dev, &fn) == EFI_SUCCESS && seg == 0 &&
                      io->Pci.Read(io, EfiPciIoWidthUint32, 0, 1, &id) == EFI_SUCCESS &&
                      io->Pci.Read(io, EfiPciIoWidthUint32, 8, 1, &cls) == EFI_SUCCESS &&
                      io->Pci.Read(io, EfiPciIoWidthUint32, 12, 1, &hdr) == EFI_SUCCESS;
    PICK *keep = NULL;
    if (located && bus == TARGET_BUS && dev == 0 && fn == 0 && id == TARGET_ID && cls == TARGET_CLASS_REV) keep = Gpu;
    else if (located && ((hdr >> 16) & 0x7f) == 1 && io->Pci.Read(io, EfiPciIoWidthUint32, 0x18, 1, &buses) == EFI_SUCCESS &&
             ((buses >> 8) & 0xff) == TARGET_BUS) keep = Bridge;
    if (keep && ++keep->Matches == 1) { keep->Handle = handles[i]; keep->Io = io; continue; }
    Bs->CloseProtocol(handles[i], &PciGuid, Image, NULL);
  }
  Bs->FreePool(handles);
  Line("target-matches gpu/bridge", Gpu->Matches, Bridge->Matches);
  return Gpu->Matches == 1 && Bridge->Matches == 1 ? EFI_SUCCESS : EFI_NOT_FOUND;
}
static void ClosePick(EFI_HANDLE Image, PICK *p) {
  if (p->Matches >= 1 && p->Handle) Bs->CloseProtocol(p->Handle, &PciGuid, Image, NULL);
  p->Handle = NULL; p->Io = NULL;
}

/* TRUE if the last file node of this image's own path ends in NEXT.EFI (any case). */
static BOOLEAN LoadedAsNext(EFI_DEVICE_PATH_PROTOCOL *path) {
  static const char tail[] = "NEXT.EFI"; const UINTN tl = sizeof tail - 1;
  const UINT8 *last = NULL; UINTN lastChars = 0, walked = 0;
  for (const UINT8 *p = (const UINT8 *)path; p && !(p[0] == END_DEVICE_PATH_TYPE && p[1] == END_ENTIRE_DEVICE_PATH_SUBTYPE);) {
    UINTN n = p[2] | ((UINTN)p[3] << 8);
    if (n < 4 || (walked += n) > 4096) return FALSE;
    if (p[0] == MEDIA_DEVICE_PATH && p[1] == MEDIA_FILEPATH_DP) { last = p + 4; lastChars = (n - 4) / 2; }
    p += n;
  }
  while (lastChars && !(last[2 * (lastChars - 1)] | last[2 * (lastChars - 1) + 1])) lastChars--;  /* drop NULs */
  if (!last || lastChars < tl) return FALSE;
  for (UINTN i = 0; i < tl; i++) {
    const UINT8 *c = last + 2 * (lastChars - tl + i);
    char ch = c[1] ? 0 : (char)c[0]; if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch != tail[i]) return FALSE;
  }
  return TRUE;
}

static EFI_STATUS StartNext(EFI_HANDLE Image, EFI_HANDLE Device, EFI_HANDLE *Child) {
  static const CHAR16 next[] = L"\\EFI\\BOOT\\NEXT.EFI";
  EFI_DEVICE_PATH_PROTOCOL *dev = NULL, *p;
  EFI_STATUS s = Bs->HandleProtocol(Device, &PathGuid, (VOID **)&dev);
  if (s != EFI_SUCCESS || !dev) return EFI_NOT_STARTED;
  UINTN devBytes = 0;
  for (p = dev; !(p->Type == END_DEVICE_PATH_TYPE && p->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE);) {
    UINTN n = p->Length[0] | ((UINTN)p->Length[1] << 8);
    if (n < 4 || devBytes + n > 4096) return EFI_NOT_STARTED;
    devBytes += n; p = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)p + n);
  }
  UINTN fileBytes = 4 + sizeof next, total = devBytes + fileBytes + 4;
  UINT8 *path = NULL;
  if (Bs->AllocatePool(EfiLoaderData, total, (VOID **)&path) != EFI_SUCCESS) return EFI_NOT_STARTED;
  for (UINTN i = 0; i < devBytes; i++) path[i] = ((UINT8 *)dev)[i];
  EFI_DEVICE_PATH_PROTOCOL *fp = (EFI_DEVICE_PATH_PROTOCOL *)(path + devBytes);
  fp->Type = MEDIA_DEVICE_PATH; fp->SubType = MEDIA_FILEPATH_DP;
  fp->Length[0] = (UINT8)fileBytes; fp->Length[1] = (UINT8)(fileBytes >> 8);
  for (UINTN i = 0; i < sizeof next; i++) ((UINT8 *)fp)[4 + i] = ((const UINT8 *)next)[i];
  EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(path + devBytes + fileBytes);
  end->Type = END_DEVICE_PATH_TYPE; end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE; end->Length[0] = 4; end->Length[1] = 0;
  s = Bs->LoadImage(FALSE, Image, (EFI_DEVICE_PATH_PROTOCOL *)path, NULL, 0, Child);
  Bs->FreePool(path);
  return s;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE Image, EFI_SYSTEM_TABLE *Table) {
  St = Table; Bs = Table->BootServices;
  EFI_LOADED_IMAGE_PROTOCOL *self = NULL;
  if (Bs->HandleProtocol(Image, &LoadedGuid, (VOID **)&self) != EFI_SUCCESS || !self) return EFI_NOT_STARTED;
  if (LoadedAsNext(self->FilePath)) {
    Screen("prestep: started as NEXT.EFI (mis-staged stick); doing nothing");
    return EFI_NOT_STARTED;
  }
  OpenLog(self->DeviceHandle);
  TXT h; h.n = 0; Put(&h, "prestep TASK-22.6 decode-sync v3 gate="); Hex(&h, PRESTEP_GATE, 1);
  EFI_TIME now; if (St->RuntimeServices && St->RuntimeServices->GetTime(&now, NULL) == EFI_SUCCESS) {
    Put(&h, " rtc="); Dec(&h, now.Year, 4); Put(&h, "-"); Dec(&h, now.Month, 2); Put(&h, "-"); Dec(&h, now.Day, 2);
    Put(&h, " "); Dec(&h, now.Hour, 2); Put(&h, ":"); Dec(&h, now.Minute, 2); Put(&h, ":"); Dec(&h, now.Second, 2);
  }
  Emit(h.b, h.n);
  BOOLEAN ok = FALSE; DS_RESULT r = DS_ERROR;
  if (!LogOk) {
    Screen("prestep: cannot create the log on the stick; changing nothing");
  } else {
    PICK gpu, bridge; gpu.Handle = bridge.Handle = NULL; gpu.Io = bridge.Io = NULL; gpu.Matches = bridge.Matches = 0;
    if (FindTargets(Image, &gpu, &bridge) == EFI_SUCCESS) {
      static UINT32 g0[64], g1[64], b0[64], b1[64];
      EFI_STATUS s0 = Snapshot(gpu.Io, g0), s1 = Snapshot(bridge.Io, b0);
      Config("gpu-before", g0); Config("bridge-before", b0);
      if (s0 == EFI_SUCCESS && s1 == EFI_SUCCESS && LogOk) {
        Line("intent sync-decode", g0[1] & 0xffff, 0);
        CTX ctx = {gpu.Io};
        DS_OPS ops = {&ctx, OpRead, OpAttr, OpLog};
        r = LogOk ? DsSync(&ops) : DS_LOG_FAILED;
        Line("result", r, 0);
        s0 = Snapshot(gpu.Io, g1); s1 = Snapshot(bridge.Io, b1);
        Config("gpu-after", g1); Config("bridge-after", b1);
        BOOLEAN deltas = s0 == EFI_SUCCESS && s1 == EFI_SUCCESS &&
                         DsConfigDeltaOk(g0, g1, r == DS_FIXED_IO_ON ? 1 : 0) && DsConfigDeltaOk(b0, b1, 0);
        Line("config-delta-ok", deltas, 0);
        /* The wrapper checks again only after its 5 s Esc window and console set-up: sample past that. */
        static const char when[4][28] = {"recheck-1s command/record", "recheck-2s command/record",
                                         "recheck-4s command/record", "recheck-8s command/record"};
        static const UINTN wait[] = {1000000, 1000000, 2000000, 4000000};
        BOOLEAN stable = TRUE;
        for (UINTN i = 0; i < 4; i++) {
          Bs->Stall(wait[i]);
          UINT16 cmd = 0xffff; UINT64 rec = 0;
          EFI_STATUS rs = OpRead(&ctx, &cmd); EFI_STATUS as = OpAttr(&ctx, DS_GET, 0, &rec);
          Line(when[i], cmd, rec);
          if (!(rs == EFI_SUCCESS && as == EFI_SUCCESS && cmd == (g1[1] & 0xffff) && DsConsistent(cmd, rec))) stable = FALSE;
        }
        ok = deltas && stable && (r == DS_ALREADY_CONSISTENT || r == DS_FIXED_IO_ON || r == DS_FIXED_RECORD_IO_OFF);
      }
    }
    ClosePick(Image, &gpu); ClosePick(Image, &bridge);
  }
  Line("decision ok/gate", ok, PRESTEP_GATE);
  if (PRESTEP_GATE && (!ok || !LogOk)) {
    Line("refusing-unlock-chain returning-to-firmware-boot-order", EFI_NOT_STARTED, 0);
    Screen("prestep: state not verified; unlock NOT started. Firmware continues to Windows.");
    CloseLog();
    return EFI_NOT_STARTED;
  }
  EFI_HANDLE child = NULL;
  EFI_STATUS s = StartNext(Image, self->DeviceHandle, &child);
  Line("load-next", s, 0);
  CloseLog();
  if (s != EFI_SUCCESS) return EFI_NOT_STARTED;
  s = Bs->StartImage(child, NULL, NULL);
  AppendAfter(self->DeviceHandle, "next-returned", s);
  return s;
}
