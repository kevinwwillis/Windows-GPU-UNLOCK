/* TASK-22.6 host tests for decode-sync.c: a mock PciIo with separate hardware command and
 * firmware record, models of how firmware might react, and fault injection on every call. */
#include "decode-sync.h"
#include <stdio.h>

typedef enum { FW_WRITES, FW_RECORD_ONLY, FW_IGNORES_ENABLE_WHEN_CACHED, FW_INERT, FW_ENABLE_SETS_BME,
               FW_HW_ONLY, FW_PARTIAL_THEN_ERROR, FW_HW_ON_RECORD_DROPS,
               FW_CACHED_DISABLE_DROPS_MEM, FW_CACHED_DISABLE_PARTIAL_ERROR } FW_MODEL;
typedef struct {
  UINT16 cmd; UINT64 rec, sup; FW_MODEL model;
  int calls, fail_at;   /* fail_at: 1-based call index that returns an error, 0 = none */
  int writes;           /* Enable/Disable calls made */
  int reads;            /* command reads */
  UINT16 cmd_trace[8]; int ntrace;
  int logs, log_fail_at;  /* log_fail_at: 1-based log call from which every log call fails, 0 = none */
  int write_after_log_fail;
} MOCK;

static int Fail(MOCK *m) { return ++m->calls == m->fail_at; }
static EFI_STATUS Rd(VOID *c, UINT16 *cmd) { MOCK *m = c; if (Fail(m)) return EFI_DEVICE_ERROR; m->reads++; *cmd = m->cmd; return EFI_SUCCESS; }
static EFI_STATUS At(VOID *c, DS_OP op, UINT64 bits, UINT64 *res) {
  MOCK *m = c; if (Fail(m)) return EFI_DEVICE_ERROR;
  UINT16 hw = (bits & DS_ATTR_IO ? 1 : 0) | (bits & DS_ATTR_MEMORY ? 2 : 0) | (bits & DS_ATTR_BUS_MASTER ? 4 : 0);
  switch (op) {
  case DS_GET: *res = m->rec; return EFI_SUCCESS;
  case DS_SUPPORTED: *res = m->sup; return EFI_SUCCESS;
  case DS_ENABLE:
    m->writes++; if (m->log_fail_at && m->logs >= m->log_fail_at) m->write_after_log_fail++;
    if (m->model == FW_INERT) return EFI_SUCCESS;
    if (m->model == FW_HW_ON_RECORD_DROPS) { m->cmd |= hw; m->rec = DS_ATTR_MEMORY; m->cmd_trace[m->ntrace++] = m->cmd; return EFI_SUCCESS; }
    if (m->model == FW_HW_ONLY) { m->cmd |= hw; m->cmd_trace[m->ntrace++] = m->cmd; return EFI_SUCCESS; }
    if (m->model == FW_PARTIAL_THEN_ERROR) { m->cmd |= hw; m->rec |= bits; m->cmd_trace[m->ntrace++] = m->cmd; return EFI_DEVICE_ERROR; }
    if (m->model == FW_ENABLE_SETS_BME) { m->cmd |= 4; m->rec |= DS_ATTR_BUS_MASTER; m->cmd_trace[m->ntrace++] = m->cmd; return EFI_SUCCESS; }
    if ((m->model == FW_IGNORES_ENABLE_WHEN_CACHED || m->model == FW_CACHED_DISABLE_DROPS_MEM ||
         m->model == FW_CACHED_DISABLE_PARTIAL_ERROR) && (m->rec & bits) == bits) return EFI_SUCCESS;
    m->rec |= bits; if (m->model != FW_RECORD_ONLY) m->cmd |= hw;
    m->cmd_trace[m->ntrace++] = m->cmd; return EFI_SUCCESS;
  case DS_DISABLE:
    m->writes++; if (m->log_fail_at && m->logs >= m->log_fail_at) m->write_after_log_fail++;
    if (m->model == FW_INERT) return EFI_SUCCESS;
    if (m->model == FW_CACHED_DISABLE_DROPS_MEM) { m->rec &= ~(bits | DS_ATTR_MEMORY); m->cmd &= (UINT16)~(hw | 2); m->cmd_trace[m->ntrace++] = m->cmd; return EFI_SUCCESS; }
    if (m->model == FW_CACHED_DISABLE_PARTIAL_ERROR) { m->rec &= ~bits; m->cmd_trace[m->ntrace++] = m->cmd; return EFI_DEVICE_ERROR; }
    m->rec &= ~bits; m->cmd &= (UINT16)~hw;
    m->cmd_trace[m->ntrace++] = m->cmd; return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
static BOOLEAN Lg(VOID *c, const char *e, UINT64 a, UINT64 b) {
  MOCK *m = c; (void)e; (void)a; (void)b; return !(m->log_fail_at && ++m->logs >= m->log_fail_at);
}

static int failures, checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static DS_RESULT Run(MOCK *m) { DS_OPS o = {m, Rd, At, Lg}; return DsSync(&o); }
static MOCK Observed(FW_MODEL model) { MOCK m = {0}; m.cmd = 0x0002; m.rec = 0x300; m.sup = 0x7ff; m.model = model; return m; }

int main(void) {
  /* Firmware writes hardware on Enable: fixed to the proven state in one call. */
  MOCK m = Observed(FW_WRITES);
  CHECK(Run(&m) == DS_FIXED_IO_ON && m.cmd == 3 && m.rec == 0x300 && m.writes == 1, "enable fixes");
  /* Firmware ignores Enable for a bit it already records: fallback corrects the record only. */
  m = Observed(FW_IGNORES_ENABLE_WHEN_CACHED);
  CHECK(Run(&m) == DS_FIXED_RECORD_IO_OFF && m.cmd == 2 && m.rec == 0x200 && m.writes == 2, "fallback when enable is a no-op");
  CHECK(m.ntrace == 1 && m.cmd_trace[0] == 2, "fallback never changes hardware command");
  /* Firmware updates its record but never the hardware bit (the suspected MSI behaviour). */
  m = Observed(FW_RECORD_ONLY);
  CHECK(Run(&m) == DS_FIXED_RECORD_IO_OFF && m.cmd == 2 && m.rec == 0x200, "record-only firmware");
  /* Firmware reports success but changes nothing: both attempts made, reported as still inconsistent. */
  m = Observed(FW_INERT);
  CHECK(Run(&m) == DS_STILL_INCONSISTENT && m.writes == 2 && m.cmd == 2 && m.rec == 0x300, "inert firmware");
  /* Enable moves something else (bus mastering): stop, no fallback write. */
  m = Observed(FW_ENABLE_SETS_BME);
  CHECK(Run(&m) == DS_STILL_INCONSISTENT && m.writes == 1, "unexpected change after enable stops");
  /* Firmware writes only the hardware bit (record already says I/O on): that is the fixed state. */
  m = Observed(FW_HW_ONLY);
  CHECK(Run(&m) == DS_FIXED_IO_ON && m.cmd == 3 && m.rec == 0x300 && m.writes == 1, "hw-only enable fixes");
  /* Hardware bit sets but the record drops I/O: not reported as fixed, no second write. */
  m = Observed(FW_HW_ON_RECORD_DROPS);
  CHECK(Run(&m) == DS_STILL_INCONSISTENT && m.writes == 1, "odd record after enable is not fixed");
  /* Enable fails after a partial effect: error, and the state is read back (logged) afterwards. */
  m = Observed(FW_PARTIAL_THEN_ERROR);
  CHECK(Run(&m) == DS_ERROR && m.writes == 1 && m.reads == 2, "partial effect then error is read back, no further write");
  /* Exact gate: extra command bits or record bits mean no action. */
  struct { UINT16 cmd; UINT64 rec; } near[] = {{0x0402, 0x300}, {0x0042, 0x300}, {0x0002, 0x301}, {0x0002, 0x380}};
  for (unsigned i = 0; i < sizeof near / sizeof near[0]; i++) {
    m = Observed(FW_WRITES); m.cmd = near[i].cmd; m.rec = near[i].rec;
    CHECK(Run(&m) == DS_UNEXPECTED_STATE && m.writes == 0, "near-miss state untouched");
  }
  /* Unreadable device (all ones) is an error, not merely unexpected. */
  m = Observed(FW_WRITES); m.cmd = 0xffff;
  CHECK(Run(&m) == DS_ERROR && m.writes == 0, "0xffff is an error");
  /* Bus master is part of consistency. */
  CHECK(DsConsistent(0x0007, 0x700) && !DsConsistent(0x0006, 0x200) && !DsConsistent(0x0002, 0x600) &&
        DsConsistent(0x0402, 0x200) && !DsConsistent(0x0002, 0x300), "DsConsistent");
  m = Observed(FW_WRITES); m.cmd = 0x0006; m.rec = 0x200;
  CHECK(Run(&m) == DS_UNEXPECTED_STATE && m.writes == 0, "bme mismatch untouched");
  /* Config delta: only the allowed command bit may differ; status must not. */
  { UINT32 a[64] = {0}, b[64]; for (int i = 0; i < 64; i++) a[i] = 0x11111111u * (UINT32)i;
    a[1] = 0x00100002;
    for (int i = 0; i < 64; i++) b[i] = a[i];
    CHECK(DsConfigDeltaOk(a, b, 0) && DsConfigDeltaOk(a, b, 0) , "identical ok");
    b[1] = 0x00100003; CHECK(DsConfigDeltaOk(a, b, 1) && !DsConfigDeltaOk(a, b, 0), "io bit allowed only when asked");
    b[1] = 0x00100007; CHECK(!DsConfigDeltaOk(a, b, 1), "extra command bit refused");
    b[1] = 0x02100003; CHECK(!DsConfigDeltaOk(a, b, 1), "status change refused");
    b[1] = 0x00100002; b[63] ^= 0x80; CHECK(!DsConfigDeltaOk(a, b, 0), "change elsewhere refused");
    b[63] = a[63]; b[4] ^= 1; CHECK(!DsConfigDeltaOk(a, b, 1), "bar change refused"); }
  /* Already consistent states: no calls that change anything. */
  UINT16 cmds[] = {0x0003, 0x0002, 0x0000}; UINT64 recs[] = {0x300, 0x200, 0x000};
  for (int i = 0; i < 3; i++) {
    m = Observed(FW_WRITES); m.cmd = cmds[i]; m.rec = recs[i];
    CHECK(Run(&m) == DS_ALREADY_CONSISTENT && m.writes == 0, "consistent untouched");
  }
  /* Any other mismatch is left alone. */
  struct { UINT16 cmd; UINT64 rec; } odd[] = {{0x0006, 0x300}, {0x0002, 0x700}, {0x0001, 0x300}, {0x0000, 0x300},
                                              {0x0002, 0x100}, {0x0003, 0x200}, {0x0002, 0x400}};
  for (unsigned i = 0; i < sizeof odd / sizeof odd[0]; i++) {
    m = Observed(FW_WRITES); m.cmd = odd[i].cmd; m.rec = odd[i].rec;
    DS_RESULT r = Run(&m);
    CHECK(r == DS_UNEXPECTED_STATE && m.writes == 0, "unexpected state untouched");
  }
  /* I/O not supported by the firmware: untouched. */
  m = Observed(FW_WRITES); m.sup = 0x6ff;
  CHECK(Run(&m) == DS_UNEXPECTED_STATE && m.writes == 0, "io unsupported");
  /* Fault on every call position in each model: never reports a fix, never writes after a fault. */
  FW_MODEL models[] = {FW_WRITES, FW_IGNORES_ENABLE_WHEN_CACHED, FW_RECORD_ONLY};
  for (int k = 0; k < 3; k++) {
    m = Observed(models[k]); Run(&m); int total = m.calls;
    for (int f = 1; f <= total; f++) {
      MOCK x = Observed(models[k]); x.fail_at = f;
      DS_RESULT r = Run(&x);
      CHECK(r == DS_ERROR, "fault reported as error");
      CHECK(x.writes <= m.writes && (x.ntrace == 0 || x.cmd_trace[x.ntrace - 1] == x.cmd), "no write after a fault");
    }
  }
  /* Disable also drops memory decode: reported, never as fixed, no third write. */
  m = Observed(FW_CACHED_DISABLE_DROPS_MEM);
  CHECK(Run(&m) == DS_STILL_INCONSISTENT && m.writes == 2 && m.cmd == 0, "disable dropping memory is not fixed");
  /* Disable fails after changing the record: error, state read back, no further write. */
  m = Observed(FW_CACHED_DISABLE_PARTIAL_ERROR);
  CHECK(Run(&m) == DS_ERROR && m.writes == 2 && m.reads == 3 && m.rec == 0x200, "disable partial then error is read back");
  /* A failed log line stops every later write: fail each log call position in each model. */
  FW_MODEL lmodels[] = {FW_WRITES, FW_IGNORES_ENABLE_WHEN_CACHED, FW_RECORD_ONLY, FW_INERT};
  for (int k = 0; k < 4; k++) {
    MOCK c = Observed(lmodels[k]); c.log_fail_at = 1000; Run(&c); int total = c.logs;
    CHECK(total >= 5, "log calls counted");
    for (int f = 1; f <= total; f++) {
      MOCK x = Observed(lmodels[k]); x.log_fail_at = f;
      DS_RESULT r = Run(&x);
      CHECK(x.write_after_log_fail == 0, "no write after a failed log line");
      if (x.writes == 0) CHECK(r == DS_LOG_FAILED, "log failure before the first write is reported");
    }
  }
  printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
