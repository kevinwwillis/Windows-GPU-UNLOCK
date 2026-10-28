/* TASK-22.6 host harness: runs prestep.c's efi_main against a mock UEFI (gcc, WSL).
 * Based on the Claude Sonnet reviewer's harness (2026-10-27). Build twice: -DPRESTEP_GATE=0 and 1.
 * Needs -I <project clone> for the project's own device-path inspector. */
#define _GNU_SOURCE /* memmem */
#include "prestep.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal-boot/boot/media.h" /* project's path inspector */

static int checks, failures;
#define CHECK(c, m) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, m); } } while (0)

/* ---------- mock PCI devices ---------- */
typedef struct MDEV {
  EFI_PCI_IO_PROTOCOL io;       /* must be first */
  UINTN bus, dev, fn;
  UINT32 cfg[64];
  UINT64 rec;
  int model;                    /* 0 writes hw+rec, 1 inert, 2 reverts I/O at command read revert_at after enabling,
                                   3 Enable also changes another GPU register, 4 Enable also changes the bridge,
                                   6 record drops I/O from Get call get_drift_at on */
  int revert_at, get_drift_at;
  int opens, attr_calls, reads_after_fix, gets, reads, writes;
  int fail_read_at, fail_attr_at, fail_loc, fail_open;  /* fault injection, 1-based call index */
} MDEV;
#define MAXDEV 8
static MDEV Devs[MAXDEV]; static int NDev;
static int WriteFailedYet, AttrWriteAfterFail; static UINTN FlushedLen; static long FirstWriteFlushed;
static char Written[65536]; static UINTN WrittenLen;
static MDEV *Dev(EFI_PCI_IO_PROTOCOL *io) { return (MDEV *)io; }
static EFI_STATUS EFIAPI MGetLoc(EFI_PCI_IO_PROTOCOL *io, UINTN *s, UINTN *b, UINTN *d, UINTN *f) {
  MDEV *m = Dev(io); if (m->fail_loc) return EFI_DEVICE_ERROR; *s = 0; *b = m->bus; *d = m->dev; *f = m->fn; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MPciRead(EFI_PCI_IO_PROTOCOL *io, EFI_PCI_IO_PROTOCOL_WIDTH w, UINT32 off, UINTN cnt, VOID *buf) {
  MDEV *m = Dev(io); UINT8 *cfg = (UINT8 *)m->cfg; UINTN sz = (UINTN)1 << w;
  if (off + sz * cnt > 256) return EFI_UNSUPPORTED;
  if (m->model == 2 && off == 4 && w == EfiPciIoWidthUint16 && (m->cfg[1] & 1) && ++m->reads_after_fix >= m->revert_at) m->cfg[1] &= ~1u;
  memcpy(buf, cfg + off, sz * cnt);
  return ++m->reads == m->fail_read_at ? EFI_DEVICE_ERROR : EFI_SUCCESS;  /* a failed read may still fill the buffer */
}
static EFI_STATUS EFIAPI MAttr(EFI_PCI_IO_PROTOCOL *io, EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op, UINT64 bits, UINT64 *res) {
  MDEV *m = Dev(io); if (++m->attr_calls == m->fail_attr_at) return EFI_DEVICE_ERROR;
  UINT16 hw = (bits & 0x100 ? 1 : 0) | (bits & 0x200 ? 2 : 0) | (bits & 0x400 ? 4 : 0);
  if (op == EfiPciIoAttributeOperationEnable || op == EfiPciIoAttributeOperationDisable) {
    m->writes++; if (WriteFailedYet) AttrWriteAfterFail++;
    if (FirstWriteFlushed < 0) FirstWriteFlushed = (long)FlushedLen;
  }
  switch (op) {
  case EfiPciIoAttributeOperationGet:
    if (m->model == 6 && ++m->gets >= m->get_drift_at) m->rec &= ~0x100ULL;
    *res = m->rec; return EFI_SUCCESS;
  case EfiPciIoAttributeOperationSupported: *res = 0x7ff; return EFI_SUCCESS;
  case EfiPciIoAttributeOperationEnable:
    if (m->model == 1) return EFI_SUCCESS;
    if (m->model == 3) m->cfg[15] ^= 0x100;
    if (m->model == 4) Devs[0].cfg[15] ^= 0x80000;  /* bridge control word */
    m->rec |= bits; m->cfg[1] |= hw; return EFI_SUCCESS;
  case EfiPciIoAttributeOperationDisable: if (m->model == 1) return EFI_SUCCESS; m->rec &= ~bits; m->cfg[1] &= ~(UINT32)hw; return EFI_SUCCESS;
  default: return EFI_UNSUPPORTED;
  }
}
static MDEV *AddDev(UINTN bus, UINTN dev, UINTN fn, UINT32 id, UINT32 cls, UINT16 cmd, UINT64 rec, int model) {
  MDEV *m = &Devs[NDev++]; memset(m, 0, sizeof *m);
  m->io.GetLocation = MGetLoc; m->io.Pci.Read = MPciRead; m->io.Attributes = MAttr;
  m->bus = bus; m->dev = dev; m->fn = fn; m->cfg[0] = id; m->cfg[1] = cmd | (0x0010u << 16); m->cfg[2] = cls; m->rec = rec; m->model = model;
  m->revert_at = 2; m->get_drift_at = 3;
  for (int i = 4; i < 64; i++) m->cfg[i] = 0x01010101u * (UINT32)i;
  m->cfg[3] = 0x00800010; return m;
}
static MDEV *AddBridge(UINTN secondary) {
  MDEV *b = AddDev(0, 3, 1, 0x14831022, 0x06040000, 0x0007, 0x700, 0);
  b->cfg[3] = 0x00010010; b->cfg[6] = (UINT32)((secondary << 16) | (secondary << 8)); return b;
}

/* ---------- mock file system ---------- */
typedef struct { EFI_FILE_PROTOCOL f; } MFILE;
static MFILE Files[160]; static int NFiles, OpenFiles;
static CHAR16 Existing[100][64]; static int NExisting, DirExists = 1, WriteFails, FlushFails, OpenDenied;
static int WritesDone, WriteFailAt, WriteFailSticky, DecisionAt;
static CHAR16 CreatedName[64]; static int Reopens, Positioned;
static EFI_STATUS EFIAPI FClose(EFI_FILE_PROTOCOL *f) { (void)f; OpenFiles--; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI FWrite(EFI_FILE_PROTOCOL *f, UINTN *n, VOID *b) {
  (void)f; WritesDone++;
  if (!strncmp(b, "decision", 8)) DecisionAt = WritesDone;
  if (WriteFails || (WriteFailAt && (WriteFailSticky ? WritesDone >= WriteFailAt : WritesDone == WriteFailAt))) { WriteFailedYet = 1; return EFI_DEVICE_ERROR; }
  memcpy(Written + WrittenLen, b, *n); WrittenLen += *n; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FFlush(EFI_FILE_PROTOCOL *f) { (void)f; if (FlushFails) return EFI_DEVICE_ERROR; FlushedLen = WrittenLen; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI FSetPos(EFI_FILE_PROTOCOL *f, UINT64 p) { (void)f; if (p == ~0ULL) Positioned++; return EFI_SUCCESS; }
static int Eq(const CHAR16 *a, const CHAR16 *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static MFILE *NewFile(void) { MFILE *m = &Files[NFiles++]; memset(m, 0, sizeof *m); m->f.Close = FClose; m->f.Write = FWrite; m->f.Flush = FFlush; m->f.SetPosition = FSetPos; OpenFiles++; return m; }
static EFI_STATUS EFIAPI FOpen(EFI_FILE_PROTOCOL *f, EFI_FILE_PROTOCOL **nf, CHAR16 *name, UINT64 mode, UINT64 attr) {
  (void)f; (void)attr; *nf = NULL;
  if (OpenDenied) return EFI_ACCESS_DENIED;
  for (int i = 0; i < NExisting; i++) if (Eq(Existing[i], name)) { *nf = &NewFile()->f; return EFI_SUCCESS; }
  if (CreatedName[0] && Eq(CreatedName, name)) { Reopens++; *nf = &NewFile()->f; return EFI_SUCCESS; }
  if (!(mode & EFI_FILE_MODE_CREATE) || !DirExists) return EFI_NOT_FOUND;
  *nf = &NewFile()->f; for (int i = 0; name[i]; i++) CreatedName[i] = name[i];
  return EFI_SUCCESS;
}
static MFILE RootF;
static EFI_STATUS EFIAPI FOpenVol(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *s, EFI_FILE_PROTOCOL **root) {
  (void)s; memset(&RootF, 0, sizeof RootF); RootF.f.Open = FOpen; RootF.f.Close = FClose; *root = &RootF.f; OpenFiles++; return EFI_SUCCESS;
}
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Sfs = { 0, FOpenVol };

/* ---------- mock boot and runtime services ---------- */
static EFI_HANDLE HImage = (EFI_HANDLE)0x1000, HDev = (EFI_HANDLE)0x2000, HChild = (EFI_HANDLE)0x3000;
static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static UINT8 DevPath[128]; static UINTN DevPathLen;
static UINT8 SelfPath[128];
static UINT8 LoadedPath[512]; static UINTN LoadedPathLen;
static int LoadCalls, StartCalls, PoolLive, Stalls; static UINTN StalledUs; static EFI_STATUS LoadStatus, StartStatus;
static EFI_HANDLE PciHandles[MAXDEV];
static EFI_GUID gPci = EFI_PCI_IO_PROTOCOL_GUID, gLoaded = EFI_LOADED_IMAGE_PROTOCOL_GUID, gPath = EFI_DEVICE_PATH_PROTOCOL_GUID, gFs = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static int Eg(const EFI_GUID *a, const EFI_GUID *b) { return memcmp(a, b, 16) == 0; }
static int DevIndex(EFI_HANDLE h) { for (int i = 0; i < NDev; i++) if (PciHandles[i] == h) return i; return -1; }
static EFI_STATUS EFIAPI MHandleProtocol(EFI_HANDLE h, EFI_GUID *g, VOID **i) {
  if (h == HImage && Eg(g, &gLoaded)) { *i = &Loaded; return EFI_SUCCESS; }
  if (h == HDev && Eg(g, &gPath)) { *i = DevPath; return EFI_SUCCESS; }
  if (h == HDev && Eg(g, &gFs)) { *i = &Sfs; return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI MLocate(EFI_LOCATE_SEARCH_TYPE t, EFI_GUID *g, VOID *k, UINTN *n, EFI_HANDLE **b) {
  (void)t; (void)k; if (!Eg(g, &gPci)) return EFI_NOT_FOUND;
  *n = NDev; EFI_HANDLE *a = malloc(sizeof(EFI_HANDLE) * (NDev ? NDev : 1)); PoolLive++; for (int i = 0; i < NDev; i++) a[i] = PciHandles[i]; *b = a; return EFI_SUCCESS;
}
static int BadOpenAttr;
static EFI_STATUS EFIAPI MOpen(EFI_HANDLE h, EFI_GUID *g, VOID **i, EFI_HANDLE agent, EFI_HANDLE ctl, UINT32 attr) {
  (void)agent; (void)ctl; int d = DevIndex(h); if (d < 0 || !Eg(g, &gPci)) return EFI_UNSUPPORTED;
  if (Devs[d].fail_open) return EFI_ACCESS_DENIED;
  if (attr != EFI_OPEN_PROTOCOL_GET_PROTOCOL) BadOpenAttr++;
  Devs[d].opens++; *i = &Devs[d].io; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MClose(EFI_HANDLE h, EFI_GUID *g, EFI_HANDLE agent, EFI_HANDLE ctl) {
  (void)agent; (void)ctl; int d = DevIndex(h); if (d < 0 || !Eg(g, &gPci)) return EFI_INVALID_PARAMETER;
  if (Devs[d].opens <= 0) return EFI_NOT_FOUND; Devs[d].opens--; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MAlloc(EFI_MEMORY_TYPE t, UINTN n, VOID **b) { (void)t; *b = malloc(n); PoolLive++; return *b ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES; }
static EFI_STATUS EFIAPI MFree(VOID *b) { free(b); PoolLive--; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MStall(UINTN us) { Stalls++; StalledUs += us; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MLoad(BOOLEAN bp, EFI_HANDLE parent, EFI_DEVICE_PATH_PROTOCOL *p, VOID *src, UINTN sz, EFI_HANDLE *out) {
  (void)bp; (void)src; (void)sz; (void)parent; LoadCalls++;
  UINT8 *b = (UINT8 *)p; UINTN n = 0;
  for (;;) { UINTN l = b[n + 2] | (b[n + 3] << 8); n += l; if (b[n - l] == 0x7f && b[n - l + 1] == 0xff) break; if (n > 400) break; }
  memcpy(LoadedPath, p, n); LoadedPathLen = n; *out = HChild; return LoadStatus;
}
static EFI_STATUS EFIAPI MStart(EFI_HANDLE h, UINTN *sz, CHAR16 **d) { (void)h; (void)sz; (void)d; StartCalls++; return StartStatus; }
static EFI_STATUS EFIAPI MOut(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o, CHAR16 *s) { (void)o; (void)s; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MGetTime(EFI_TIME *t, EFI_TIME_CAPABILITIES *c) {
  (void)c; memset(t, 0, sizeof *t); t->Year = 2026; t->Month = 10; t->Day = 27; t->Hour = 16; t->Minute = 53; t->Second = 5; return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Bsv; static EFI_RUNTIME_SERVICES Rtv; static EFI_SYSTEM_TABLE Sys; static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL ConOut;

/* Device path: one file node holding name, then the end node. */
static void FilePathNode(UINT8 *p, const char *name) {
  UINTN n = strlen(name), len = 4 + 2 * (n + 1);
  p[0] = 4; p[1] = 4; p[2] = (UINT8)len; p[3] = (UINT8)(len >> 8);
  for (UINTN i = 0; i <= n; i++) { p[4 + 2 * i] = (UINT8)name[i]; p[5 + 2 * i] = 0; }
  p[len] = 0x7f; p[len + 1] = 0xff; p[len + 2] = 4; p[len + 3] = 0;
}
static void Setup(void) {
  NDev = 0; memset(Devs, 0, sizeof Devs); NFiles = 0; OpenFiles = 0; NExisting = 0; DirExists = 1; WriteFails = FlushFails = OpenDenied = 0;
  WrittenLen = 0; memset(Written, 0, sizeof Written); memset(CreatedName, 0, sizeof CreatedName); Reopens = Positioned = 0;
  WritesDone = WriteFailAt = WriteFailSticky = DecisionAt = 0; WriteFailedYet = AttrWriteAfterFail = 0; FlushedLen = 0; FirstWriteFlushed = -1;
  LoadCalls = StartCalls = PoolLive = Stalls = BadOpenAttr = 0; StalledUs = 0; LoadStatus = StartStatus = EFI_SUCCESS; LoadedPathLen = 0;
  memset(&Bsv, 0, sizeof Bsv); Bsv.HandleProtocol = MHandleProtocol; Bsv.LocateHandleBuffer = MLocate; Bsv.OpenProtocol = MOpen;
  Bsv.CloseProtocol = MClose; Bsv.AllocatePool = MAlloc; Bsv.FreePool = MFree; Bsv.LoadImage = MLoad; Bsv.StartImage = MStart; Bsv.Stall = MStall;
  ConOut.OutputString = MOut; Sys.BootServices = &Bsv; Sys.ConOut = &ConOut; Sys.RuntimeServices = NULL;
  memset(&Loaded, 0, sizeof Loaded); Loaded.DeviceHandle = HDev;
  FilePathNode(SelfPath, "\\EFI\\BOOT\\BOOTX64.EFI"); Loaded.FilePath = (EFI_DEVICE_PATH_PROTOCOL *)SelfPath;
  UINT8 *p = DevPath; UINT8 acpi[12] = {2, 1, 12, 0, 0xd0, 0x41, 3, 0x0a, 0, 0, 0, 0}; memcpy(p, acpi, 12); p += 12;
  UINT8 pci[6] = {1, 1, 6, 0, 1, 8}; memcpy(p, pci, 6); p += 6;
  UINT8 usb[6] = {3, 5, 6, 0, 2, 0}; memcpy(p, usb, 6); p += 6;
  UINT8 hd[42] = {4, 1, 42, 0}; hd[4] = 1; hd[24] = 1; hd[41] = 1; memcpy(p, hd, 42); p += 42;
  UINT8 end[4] = {0x7f, 0xff, 4, 0}; memcpy(p, end, 4); p += 4; DevPathLen = (UINTN)(p - DevPath);
  LogFile = NULL; Root = NULL; LogOk = FALSE;
  for (int i = 0; i < MAXDEV; i++) PciHandles[i] = (EFI_HANDLE)(UINTN)(0x5000 + i * 16);
}
static MDEV *Gpu;
static void AddStandard(UINT16 cmd, UINT64 rec, int model) {
  AddBridge(0x2d);
  AddDev(0x2d, 0, 1, 0x228b10de, 0x040300a1, 0x0002, 0x200, 0);              /* GPU audio fn1 */
  Gpu = AddDev(0x2d, 0, 0, 0x220410de, 0x030000a1, cmd, rec, model);         /* target */
  AddDev(0x2c, 0, 0, 0x220410de, 0x030000a1, 0x0002, 0x300, 0);              /* same id, other bus */
}
static EFI_STATUS Run(void) { return efi_main(HImage, &Sys); }
static int TotalOpens(void) { int t = 0; for (int i = 0; i < NDev; i++) t += Devs[i].opens; return t; }
static int OthersTouched(void) { int t = 0; for (int i = 0; i < NDev; i++) if (&Devs[i] != Gpu) t += Devs[i].attr_calls; return t; }
static int NameIs(const char *a) { for (int i = 0;; i++) { if ((CHAR16)(unsigned char)a[i] != CreatedName[i]) return 0; if (!a[i]) return 1; } }
static int Has(const char *s) { return strstr(Written, s) != NULL; }
static void Balanced(const char *what) {
  char m[160];
  snprintf(m, sizeof m, "%s: opens balanced", what); CHECK(TotalOpens() == 0 && BadOpenAttr == 0, m);
  snprintf(m, sizeof m, "%s: no pool leak", what); CHECK(PoolLive == 0, m);
  snprintf(m, sizeof m, "%s: files balanced", what); CHECK(OpenFiles == 0, m);
  snprintf(m, sizeof m, "%s: no other device touched", what); CHECK(OthersTouched() == 0, m);
}
/* Gate 1 must not start the child; gate 0 must still start the probe. */
#define REFUSED_OR_PROBE() (PRESTEP_GATE ? StartCalls == 0 && LoadCalls == 0 : StartCalls == 1)

int main(void) {
  EFI_STATUS s;
  /* 1: observed state, firmware writes hardware: fixed, verified, chained (both gate modes). */
  Setup(); AddStandard(0x0002, 0x300, 0); Sys.RuntimeServices = &Rtv; Rtv.GetTime = MGetTime; s = Run();
  CHECK(s == EFI_SUCCESS && StartCalls == 1, "fixed: child started");
  CHECK((Gpu->cfg[1] & 0xffff) == 0x0003 && Gpu->rec == 0x300, "fixed to 0x0003/0x300");
  CHECK(Has("result a=0x0000000000000003") && Has("config-delta-ok a=0x0000000000000001") && Has("decision ok/gate a=0x0000000000000001"), "log: result, delta, decision");
  CHECK(Has("gpu-before dwords=220410de 00100002") && Has("gpu-after dwords=220410de 00100003") && Has("bridge-before"), "log: full config before/after");
  CHECK(Stalls == 4 && StalledUs == 8000000 && Has("recheck-1s") && Has("recheck-2s") && Has("recheck-4s") && Has("recheck-8s"), "rechecks at 1, 2, 4, 8 s");
  CHECK(Has("intent sync-decode") && strstr(Written, "intent sync-decode") < strstr(Written, "about-to-enable-io") &&
        strstr(Written, "about-to-enable-io") < strstr(Written, "enable-io a="), "intent and about-to logged before the change");
  CHECK(FirstWriteFlushed > 0 && memmem(Written, (size_t)FirstWriteFlushed, "about-to-enable-io", 18) != NULL, "about-to line flushed before the write");
  CHECK(Has("load-next a=0x0000000000000000") && Has("next-returned") && Reopens == 1 && Positioned == 1, "chain status logged, log reopened and appended");
  CHECK(Has("prestep TASK-22.6 decode-sync v3 gate=") && Has("rtc=2026-10-27 16:53:05"), "header with decimal clock");
  CHECK(NameIs("\\RTX3090-UNLOCK\\PREST00.TXT"), "log name PREST00");
  Balanced("fixed");
  {
    UINTN bytes = 0; BOOLEAN usb = FALSE, part = FALSE;
    BOOLEAN ok = GuInspectPath((EFI_DEVICE_PATH_PROTOCOL *)LoadedPath, NULL, &bytes, &usb, &part);
    CHECK(ok && usb && bytes == LoadedPathLen, "project's GuInspectPath accepts the child path and sees USB");
    UINTN devBytes = DevPathLen - 4; UINT8 *fp = LoadedPath + devBytes;
    CHECK(memcmp(LoadedPath, DevPath, devBytes) == 0 && fp[0] == 4 && fp[1] == 4 && (fp[2] | (fp[3] << 8)) == 42 &&
          memcmp(fp + 4, L"\\EFI\\BOOT\\NEXT.EFI", 38) == 0 && LoadedPathLen == devBytes + 42 + 4, "child path exact");
  }
  int normalWrites = WritesDone, normalDecision = DecisionAt, gpuReads = Gpu->reads, gpuAttrs = Gpu->attr_calls, bridgeReads = Devs[0].reads;
  CHECK(normalDecision > 0 && normalWrites > normalDecision, "write counts recorded");
  /* 2: inert firmware: not fixed. Gate 1 refuses (no child), gate 0 still starts the probe. */
  Setup(); AddStandard(0x0002, 0x300, 1); s = Run();
  CHECK(Has("result a=0x0000000000000005"), "inert: STILL_INCONSISTENT logged");
  if (PRESTEP_GATE) CHECK(s == EFI_NOT_STARTED && LoadCalls == 0 && StartCalls == 0 && Has("refusing-unlock-chain"), "gate: inert refuses unlock chain");
  else CHECK(s == EFI_SUCCESS && StartCalls == 1, "probe chain: inert still starts the probe");
  Balanced("inert");
  /* 3: fix takes, then reverts before the 1 s recheck, or only before the 8 s recheck. */
  for (int at = 2; at <= 5; at += 3) {
    Setup(); AddStandard(0x0002, 0x300, 2); Gpu->revert_at = at; s = Run();
    CHECK(Has("result a=0x0000000000000003"), "revert: sync itself reported fixed");
    CHECK(REFUSED_OR_PROBE(), at == 2 ? "late revert at 1 s: gate refuses / probe runs" : "late revert at 8 s: gate refuses / probe runs");
    Balanced("revert");
  }
  /* 3a: record drops I/O after the sync while the command keeps it: the recheck sees the mismatch. */
  Setup(); AddStandard(0x0002, 0x300, 6); s = Run();
  CHECK(Has("result a=0x0000000000000003") && REFUSED_OR_PROBE(), "record drift: gate refuses / probe runs");
  Balanced("record drift");
  /* 3b/3c: Enable also changes another GPU register / the bridge: gate refuses. */
  for (int model = 3; model <= 4; model++) {
    Setup(); AddStandard(0x0002, 0x300, model); s = Run();
    CHECK(Has("result a=0x0000000000000003") && Has("config-delta-ok a=0x0000000000000000"), "side change: delta flagged");
    if (PRESTEP_GATE) CHECK(s == EFI_NOT_STARTED && StartCalls == 0, "gate: side change refuses unlock chain");
    else CHECK(StartCalls == 1, "probe chain: side change still starts the probe");
    Balanced(model == 3 ? "gpu side change" : "bridge side change");
  }
  /* 4: already consistent (proven machines' state): only reads, chained in both modes. */
  Setup(); AddStandard(0x0003, 0x300, 0); s = Run();
  CHECK(s == EFI_SUCCESS && StartCalls == 1 && Has("result a=0x0000000000000001") && Gpu->writes == 0, "consistent: chained, no write");
  Balanced("consistent");
  /* 5: unexpected state: untouched; gate refuses. */
  Setup(); AddStandard(0x0006, 0x300, 0); s = Run();
  CHECK(Has("result a=0x0000000000000002") && (Gpu->cfg[1] & 0xffff) == 0x0006 && Gpu->writes == 0, "unexpected: untouched");
  CHECK(REFUSED_OR_PROBE(), "unexpected: gate refuses / probe runs");
  Balanced("unexpected");
  /* 6: no GPU, no bridge, two GPUs, two bridges, wrong revision: nothing touched; gate refuses. */
  Setup(); AddBridge(0x2d); s = Run();
  CHECK(REFUSED_OR_PROBE(), "no gpu: gate refuses / probe runs");
  Balanced("no gpu");
  Setup(); AddDev(0x2d, 0, 0, 0x220410de, 0x030000a1, 0x0002, 0x300, 0); Gpu = NULL; s = Run();
  CHECK(Devs[0].attr_calls == 0 && REFUSED_OR_PROBE(), "no bridge: untouched");
  CHECK(TotalOpens() == 0 && PoolLive == 0 && OpenFiles == 0, "no bridge: balanced");
  Setup(); AddStandard(0x0002, 0x300, 0); AddDev(0x2d, 0, 0, 0x220410de, 0x030000a1, 0x0002, 0x300, 0); s = Run();
  CHECK(Gpu->attr_calls == 0 && Devs[4].attr_calls == 0 && TotalOpens() == 0, "two gpus: nothing touched");
  CHECK(REFUSED_OR_PROBE(), "two gpus: gate refuses / probe runs");
  Setup(); AddStandard(0x0002, 0x300, 0); AddBridge(0x2d); s = Run();
  CHECK(Gpu->attr_calls == 0 && REFUSED_OR_PROBE(), "two bridges: untouched, gate refuses / probe runs");
  Balanced("two bridges");
  Setup(); AddBridge(0x2d); Gpu = AddDev(0x2d, 0, 0, 0x220410de, 0x030000a2, 0x0002, 0x300, 0); s = Run();
  CHECK(Gpu->attr_calls == 0 && REFUSED_OR_PROBE(), "other revision at 2d:00.0: untouched");
  Gpu = NULL; Balanced("other revision");
  /* 6b: decoy type-0 device whose byte 0x19 is 0x2d is not a bridge: the normal fix proceeds. */
  Setup(); AddStandard(0x0002, 0x300, 0); { MDEV *d = AddDev(0x05, 0, 0, 0x12341022, 0x01080000, 0x0006, 0x600, 0); d->cfg[6] = 0x002d2d00; }
  s = Run(); CHECK(s == EFI_SUCCESS && StartCalls == 1 && (Gpu->cfg[1] & 0xffff) == 0x0003, "type-0 decoy ignored");
  Balanced("decoy");
  /* 7: log cannot be created (no directory, access denied, 100 logs, flush fails): no change; gate refuses. */
  for (int k = 0; k < 4; k++) {
    Setup(); AddStandard(0x0002, 0x300, 0);
    if (k == 0) DirExists = 0;
    if (k == 1) OpenDenied = 1;
    if (k == 2) for (int n = 0; n < 100; n++) {
      char name[40]; snprintf(name, sizeof name, "\\RTX3090-UNLOCK\\PREST%02d.TXT", n);
      for (int c = 0; name[c]; c++) Existing[n][c] = (CHAR16)name[c];
      NExisting++;
    }
    if (k == 3) FlushFails = 1;
    s = Run();
    CHECK(Gpu->attr_calls == 0 && (Gpu->cfg[1] & 0xffff) == 0x0002, "no log: nothing changed");
    CHECK(PRESTEP_GATE ? (s == EFI_NOT_STARTED && StartCalls == 0) : StartCalls == 1, "no log: gate refuses / probe runs");
    Balanced("no log");
  }
  /* 8: every log write fails: nothing changed. */
  Setup(); AddStandard(0x0002, 0x300, 0); WriteFails = 1; s = Run();
  CHECK(Gpu->attr_calls == 0 && (Gpu->cfg[1] & 0xffff) == 0x0002, "write failure: nothing changed");
  CHECK(REFUSED_OR_PROBE(), "write failure: gate refuses / probe runs");
  Balanced("write failure");
  /* 8b: one log write fails (once, or from then on) at each position: no firmware write after it,
   * and in gate mode no child once any line up to the decision failed. */
  for (int sticky = 0; sticky <= 1; sticky++) for (int k = 1; k <= normalWrites; k++) {
    Setup(); AddStandard(0x0002, 0x300, 0); WriteFailAt = k; WriteFailSticky = sticky; s = Run();
    char m[120];
    snprintf(m, sizeof m, "log write %d failed (%s): no firmware write after it", k, sticky ? "sticky" : "once"); CHECK(AttrWriteAfterFail == 0, m);
    if (k <= normalDecision) { snprintf(m, sizeof m, "log write %d failed: gate refuses / probe runs", k); CHECK(REFUSED_OR_PROBE(), m); }
    Balanced("log write fault");
  }
  /* 9: child missing; child returns NOT_STARTED. */
  Setup(); AddStandard(0x0002, 0x300, 0); LoadStatus = EFI_NOT_FOUND; s = Run();
  CHECK(s == EFI_NOT_STARTED && StartCalls == 0 && Has("load-next a=0x800000000000000e"), "LoadImage failure logged, NOT_STARTED");
  Balanced("load failure");
  Setup(); AddStandard(0x0002, 0x300, 0); StartStatus = EFI_NOT_STARTED; s = Run();
  CHECK(s == EFI_NOT_STARTED && Has("next-returned a=0x8000000000000013"), "child NOT_STARTED passed through and logged");
  /* 10: existing PREST00..02 -> next is 03. */
  Setup(); AddStandard(0x0002, 0x300, 0);
  { const char *n[] = {"\\RTX3090-UNLOCK\\PREST00.TXT", "\\RTX3090-UNLOCK\\PREST01.TXT", "\\RTX3090-UNLOCK\\PREST02.TXT"};
    for (int i = 0; i < 3; i++) { for (int k = 0; n[i][k]; k++) Existing[i][k] = (CHAR16)n[i][k]; NExisting++; } }
  Run(); CHECK(NameIs("\\RTX3090-UNLOCK\\PREST03.TXT"), "log numbering skips existing");
  /* 11: firmware call failures on the GPU or bridge at every position: gate never starts the child. */
  for (int i = 1; i <= gpuReads; i++) {
    Setup(); AddStandard(0x0002, 0x300, 0); Gpu->fail_read_at = i; s = Run();
    char m[80]; snprintf(m, sizeof m, "gpu read %d fails: gate refuses / probe runs", i); CHECK(REFUSED_OR_PROBE(), m); Balanced("gpu read fault");
  }
  for (int i = 1; i <= gpuAttrs; i++) {
    Setup(); AddStandard(0x0002, 0x300, 0); Gpu->fail_attr_at = i; s = Run();
    char m[80]; snprintf(m, sizeof m, "gpu attributes call %d fails: gate refuses / probe runs", i); CHECK(REFUSED_OR_PROBE(), m); Balanced("gpu attr fault");
  }
  for (int i = 1; i <= bridgeReads; i++) {
    Setup(); AddStandard(0x0002, 0x300, 0); Devs[0].fail_read_at = i; s = Run();
    char m[80]; snprintf(m, sizeof m, "bridge read %d fails: gate refuses / probe runs", i); CHECK(REFUSED_OR_PROBE(), m); Balanced("bridge read fault");
  }
  for (int k = 0; k < 4; k++) {
    Setup(); AddStandard(0x0002, 0x300, 0);
    if (k == 0) Gpu->fail_loc = 1; if (k == 1) Gpu->fail_open = 1; if (k == 2) Devs[0].fail_loc = 1; if (k == 3) Devs[0].fail_open = 1;
    s = Run(); CHECK(Gpu->writes == 0 && REFUSED_OR_PROBE(), "location/open failure: untouched, gate refuses / probe runs"); Balanced("location/open fault");
  }
  /* 12: this image loaded as NEXT.EFI (mis-staged stick): returns at once, touches nothing. */
  for (int k = 0; k < 2; k++) {
    Setup(); AddStandard(0x0002, 0x300, 0); FilePathNode(SelfPath, k ? "\\efi\\boot\\next.efi" : "\\EFI\\BOOT\\NEXT.EFI"); s = Run();
    CHECK(s == EFI_NOT_STARTED && Gpu->attr_calls == 0 && LoadCalls == 0 && NFiles == 0 && WritesDone == 0, "loaded as NEXT.EFI: nothing done");
    Balanced("self as next");
  }
  Setup(); AddStandard(0x0002, 0x300, 0); FilePathNode(SelfPath, "\\EFI\\BOOT\\NEXTX.EFI"); s = Run();
  CHECK(s == EFI_SUCCESS && StartCalls == 1, "other name ending differently still runs");
  printf("gate=%d: %d checks, %d failures\n", PRESTEP_GATE, checks, failures);
  return failures != 0;
}
