#include "ra.h"

#include <stdarg.h>

#include "pins.h"
#include "swd.h"

namespace ra {

// Cortex-M23 debug registers
static constexpr uint32_t DHCSR = 0xE000EDF0;
static constexpr uint32_t DEMCR = 0xE000EDFC;
static constexpr uint32_t AIRCR = 0xE000ED0C;
static constexpr uint32_t DBGKEY = 0xA05F0000;
static constexpr uint32_t C_DEBUGEN = 1u << 0;
static constexpr uint32_t C_HALT = 1u << 1;
static constexpr uint32_t S_HALT = 1u << 17;
static constexpr uint32_t VC_CORERESET = 1u << 0;
static constexpr uint32_t AIRCR_SYSRESET = 0x05FA0004;

// Clock generation (SYSC). After reset ICLK = MOCO (8 MHz) / 16 = 0.5 MHz, below the 1 MHz
// minimum of the flash sequencer, so the flasher runs the target from MOCO undivided.
static constexpr uint32_t SYSC = 0x4001E000;
static constexpr uint32_t SCKDIVCR = SYSC + 0x020;  // 32 bit
static constexpr uint32_t SCKSCR = SYSC + 0x026;    // 8 bit
static constexpr uint32_t MOCOCR = SYSC + 0x038;    // 8 bit
static constexpr uint32_t PRCR = SYSC + 0x3FE;      // 16 bit
static constexpr uint8_t ICLK_MHZ = 8;

// Flash controller (FLCN / R_FACI_LP), register widths from the RA2E1 SVD.
static constexpr uint32_t FLCN = 0x407EC000;
static constexpr uint32_t DFLCTL = FLCN + 0x0090;   // 8
static constexpr uint32_t FPMCR = FLCN + 0x0100;    // 8
static constexpr uint32_t FASR = FLCN + 0x0104;     // 8
static constexpr uint32_t FSARL = FLCN + 0x0108;    // 16
static constexpr uint32_t FSARH = FLCN + 0x0110;    // 16
static constexpr uint32_t FCR = FLCN + 0x0114;      // 8
static constexpr uint32_t FEARL = FLCN + 0x0118;    // 16
static constexpr uint32_t FEARH = FLCN + 0x0120;    // 16
static constexpr uint32_t FRESETR = FLCN + 0x0124;  // 8
static constexpr uint32_t FSTATR1 = FLCN + 0x012C;  // 8
static constexpr uint32_t FWBL0 = FLCN + 0x0130;    // 16
static constexpr uint32_t FWBH0 = FLCN + 0x0138;    // 16
static constexpr uint32_t FPR = FLCN + 0x0180;      // 8
static constexpr uint32_t FPSR = FLCN + 0x0184;     // 8
static constexpr uint32_t FAWSMR = FLCN + 0x01C8;   // 16
static constexpr uint32_t FAWEMR = FLCN + 0x01D0;   // 16
static constexpr uint32_t FISR = FLCN + 0x01D8;     // 8
static constexpr uint32_t FSTATR2 = FLCN + 0x01F0;  // 16
static constexpr uint32_t FENTRYR = FLCN + 0x3FB0;  // 16 (FENTRYR_MF4)
static constexpr uint32_t PFBER = FLCN + 0x3FC8;    // 8

static constexpr uint16_t FENTRY_CF = 0xAA01;  // code flash P/E mode
static constexpr uint16_t FENTRY_DF = 0xAA80;  // data flash P/E mode
static constexpr uint16_t FENTRY_READ = 0xAA00;
static constexpr uint8_t FPMCR_CF_PE = 0x02;   // MF4
static constexpr uint8_t FPMCR_DF_PE = 0x10;
static constexpr uint8_t FPMCR_READ = 0x08;
static constexpr uint8_t FPR_UNLOCK = 0xA5;
static constexpr uint8_t FCR_WRITE = 0x81;
static constexpr uint8_t FCR_ERASE = 0x84;
static constexpr uint8_t FCR_OPST = 0x80;      // "processing" bit, cleared first on MF4
static constexpr uint8_t FSTATR1_FRDY = 0x40;
static constexpr uint16_t ERR_ERASE = 0x11;    // ILGLERR | ERERR
static constexpr uint16_t ERR_WRITE = 0x16;    // ILGLERR | PRGERR01 | PRGERR1
static constexpr uint32_t DF_PE_BASE = 0xFE000000;  // data flash address as seen by the sequencer

// Worst-case times from r_flash_lp.c, with margin for the SWD polling.
static constexpr uint32_t TMO_WRITE_US = 20000;
static constexpr uint32_t TMO_ERASE_CF_US = 1000000;
static constexpr uint32_t TMO_ERASE_DF_US = 1500000;

// Device information (bsp_feature.h: BSP_FEATURE_BSP_UNIQUE_ID_POINTER / PART_NUMBER_POINTER)
static constexpr uint32_t UIDR = 0x01001C00;
static constexpr uint32_t PNR = 0x01001C10;

static char s_err[160];

static const char *fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_err, sizeof(s_err), fmt, ap);
  va_end(ap);
  return s_err;
}

// ---------------------------------------------------------------- register access

// A flash command is a sequence of register accesses; any failed SWD access clears s_ok and
// the sequence is checked once at the end.
static bool s_ok = true;

static void w8(uint32_t a, uint8_t v) { s_ok &= swd::write8(a, v); }
static void w16(uint32_t a, uint16_t v) { s_ok &= swd::write16(a, v); }
static void w32(uint32_t a, uint32_t v) { s_ok &= swd::write32(a, v); }
static uint8_t r8(uint32_t a) {
  uint8_t v = 0;
  s_ok &= swd::read8(a, &v);
  return v;
}
static uint16_t r16(uint32_t a) {
  uint16_t v = 0;
  s_ok &= swd::read16(a, &v);
  return v;
}

static void nrst(bool assert) {
#if PIN_NRST >= 0
  // Open drain: the target pulls RES up to its own VCC.
  if (assert) {
    digitalWrite(PIN_NRST, LOW);
    pinMode(PIN_NRST, OUTPUT);
  } else {
    pinMode(PIN_NRST, INPUT);
  }
#else
  (void)assert;
#endif
}

// ---------------------------------------------------------------- flash sequencer

// FPMCR is protected: FPR unlock, then the value, its complement and the value again.
static const char *writeFpmcr(uint8_t v) {
  w8(FPR, FPR_UNLOCK);
  w8(FPMCR, v);
  w8(FPMCR, (uint8_t)~v);
  w8(FPMCR, v);
  uint8_t ps = r8(FPSR);
  if (!s_ok) return "Lost target while setting the flash mode";
  if (ps & 1) return fail("Flash mode change rejected (FPSR=0x%02X)", ps);
  return nullptr;
}

static uint32_t s_fsarh = 0xFFFFFFFF;  // FSARH last written in this P/E session

static const char *exitPe() {
  s_ok = true;
  const char *e = writeFpmcr(FPMCR_READ);
  delayMicroseconds(16);  // tMS
  w16(FENTRYR, FENTRY_READ);
  uint32_t t0 = millis();
  while (r16(FENTRYR) != 0) {
    if (!s_ok) return "Lost target while leaving P/E mode";
    if (millis() - t0 > 50) return "Flash did not return to read mode";
  }
  return e;
}

// Reset the sequencer after an error and go back to read mode.
static void flashReset() {
  s_ok = true;
  w8(FRESETR, 1);
  w8(FRESETR, 0);
  exitPe();
}

static const char *enterPe(bool code) {
  s_ok = true;
  s_fsarh = 0xFFFFFFFF;
  if (code) w8(PFBER, 0);  // no prefetching from the code flash while it changes
  w16(FENTRYR, code ? FENTRY_CF : FENTRY_DF);
  delayMicroseconds(6);  // tDSTOP
  const char *e = writeFpmcr(code ? FPMCR_CF_PE : FPMCR_DF_PE);
  delayMicroseconds(3);  // tDIS
  if (!e) {
    uint16_t fe = r16(FENTRYR);
    if (!s_ok) e = "Lost target while entering P/E mode";
    else if (fe != (code ? 0x0001 : 0x0080))
      e = fail("Flash did not enter %s P/E mode (FENTRYR=0x%04X)", code ? "code" : "data", fe);
  }
  if (!e) w8(FASR, 0);  // user area
  if (e) flashReset();
  return e;
}

static const char *describe(uint16_t st2) {
  if (st2 & 0x10) return " - illegal command: blocks protected by an access window?";
  if (st2 & 0x01) return " - erase error";
  if (st2 & 0x06) return " - program error: not erased?";
  return "";
}

// Wait for the command started with FCR=cmd, then hand FCR back as MF4 requires
// (clear the processing bit first, then the command), and check the error flags.
static const char *finish(uint8_t cmd, uint32_t timeoutUs, uint16_t errMask, const char *what, uint32_t addr) {
  uint32_t t0 = micros();
  while (!(r8(FSTATR1) & FSTATR1_FRDY)) {
    if (!s_ok) return fail("Lost target during %s at 0x%08X", what, addr);
    if (micros() - t0 > timeoutUs) {
      flashReset();
      return fail("%s timed out at 0x%08X", what, addr);
    }
    if (timeoutUs > 100000) yield();
  }
  w8(FCR, cmd & ~FCR_OPST);
  w8(FCR, 0);
  t0 = micros();
  while (r8(FSTATR1) & FSTATR1_FRDY) {
    if (!s_ok) return fail("Lost target during %s at 0x%08X", what, addr);
    if (micros() - t0 > 20000) {
      flashReset();
      return fail("%s did not complete at 0x%08X", what, addr);
    }
  }
  uint16_t st2 = r16(FSTATR2);
  if (!s_ok) return fail("Lost target during %s at 0x%08X", what, addr);
  if (st2 & errMask) {
    flashReset();
    return fail("%s failed at 0x%08X (FSTATR2=0x%04X)%s", what, addr, st2, describe(st2));
  }
  return nullptr;
}

static void setStart(uint32_t peAddr) {
  if ((peAddr >> 16) != s_fsarh) {
    s_fsarh = peAddr >> 16;
    w16(FSARH, (uint16_t)s_fsarh);
  }
  w16(FSARL, (uint16_t)peAddr);
}

static const char *eraseBlock(uint32_t peAddr, uint32_t size, uint32_t timeoutUs, uint32_t addr) {
  s_ok = true;
  uint32_t end = peAddr + size - 1;
  setStart(peAddr);
  w16(FEARH, (uint16_t)(end >> 16));
  w16(FEARL, (uint16_t)end);
  w8(FCR, FCR_ERASE);
  return finish(FCR_ERASE, timeoutUs, ERR_ERASE, "Erase", addr);
}

static const char *writeCodeWord(uint32_t addr, uint32_t w) {
  s_ok = true;
  setStart(addr);
  w16(FWBL0, (uint16_t)w);
  w16(FWBH0, (uint16_t)(w >> 16));
  w8(FCR, FCR_WRITE);
  return finish(FCR_WRITE, TMO_WRITE_US, ERR_WRITE, "Write", addr);
}

static const char *writeDataByte(uint32_t off, uint8_t b) {
  s_ok = true;
  setStart(DF_PE_BASE + off);
  w16(FWBL0, b);
  w8(FCR, FCR_WRITE);
  return finish(FCR_WRITE, TMO_WRITE_US, ERR_WRITE, "Write", DF_BASE + off);
}

// Clock and flash set-up once the core is halted.
static const char *setupFlash() {
  s_ok = true;
  w16(PRCR, 0xA501);  // unlock the clock registers
  w8(MOCOCR, 0);      // MOCO on (it is after reset; firmware may have stopped it)
  delayMicroseconds(50);
  w8(SCKSCR, 1);      // system clock = MOCO
  w32(SCKDIVCR, 0);   // ICLK = PCLKB = PCLKD = MOCO / 1 = 8 MHz
  w16(PRCR, 0xA500);
  uint8_t sck = r8(SCKSCR);
  uint32_t div = 0;
  s_ok &= swd::read32(SCKDIVCR, &div);
  if (!s_ok) return "Lost target while setting its clock";
  if (sck != 1 || (div & 0x07000000))
    return fail("Cannot set the target clock (SCKSCR=0x%02X SCKDIVCR=0x%08X)", sck, div);

  w8(DFLCTL, 1);  // data flash readable
  delayMicroseconds(10);

  // FISR.PCKA = flash clock in MHz - 1; FISR is writable in data flash P/E mode only.
  if (const char *e = enterPe(false)) return e;
  uint8_t fisr = r8(FISR);
  w8(FISR, (fisr & 0xC0) | (ICLK_MHZ - 1));
  const char *e = exitPe();
  if (!e && !s_ok) e = "Lost target while setting the flash clock";
  return e;
}

// ---------------------------------------------------------------- identification

static void toHex(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02X", b[i]);
}

// The PNR holds the part number in ASCII. Accept it in either byte order.
static void parsePart(const uint8_t raw[16], Target &t) {
  char fwd[17], rev[17];
  for (int i = 0; i < 16; i++) {
    fwd[i] = (char)raw[i];
    rev[i] = (char)raw[15 - i];
  }
  fwd[16] = rev[16] = 0;
  for (const char *s : {fwd, rev}) {
    const char *p = strstr(s, "R7F");
    if (!p) continue;
    size_t n = 0;
    while (n < sizeof(t.part) - 1 && isalnum((unsigned char)p[n])) n++;
    memcpy(t.part, p, n);
    t.part[n] = 0;
    // R7FA2E1A<n>...: 5 = 32 KB, 7 = 64 KB, 9 = 128 KB of code flash
    if (strncmp(t.part, "R7FA2E1A", 8) == 0) {
      char size = t.part[8];
      t.cfSize = size == '5' ? 32 * 1024 : size == '7' ? 64 * 1024 : size == '9' ? 128 * 1024 : 0;
    }
    return;
  }
}

static void identify(Target &t) {
  uint32_t w[4];
  if (swd::readBlock32(PNR, w, 4)) parsePart((const uint8_t *)w, t);
  if (swd::readBlock32(UIDR, w, 4)) toHex((const uint8_t *)w, 16, t.uid);
  swd::read16(FAWSMR, &t.fawsmr);
  swd::read16(FAWEMR, &t.fawemr);
}

// ---------------------------------------------------------------- operations

bool probe() {
  // Same speed fallback as attach(): some boards only answer with a slower SWD clock.
  // Start with the speed that worked last time so a present board is seen in one try.
  static uint32_t lastGood = 0;
  const uint32_t base = swd::getDelay();
  const uint32_t tries[] = {max(base, lastGood), base, 4, 16, 64};
  uint32_t id;
  bool ok = false;
  for (uint32_t d : tries) {
    if (d < base) continue;
    swd::setDelay(d);
    if (swd::connect(&id, false)) {
      ok = true;
      lastGood = d;
      break;
    }
  }
  swd::setDelay(base);
  swd::release();
  return ok;
}

static bool waitHalted() {
  for (int i = 0; i < 50; i++) {
    uint32_t v;
    if (swd::read32(DHCSR, &v) && (v & S_HALT)) return true;
    delay(2);
  }
  return false;
}

const char *attach(Target &t, bool useReset) {
  t = Target();
  const uint32_t base = swd::getDelay();
  const uint32_t fallback[] = {base, 4, 16, 64, 250};

#if PIN_NRST >= 0
  if (useReset) {
    // Connect under reset: hold RES low and, if the debug port answers, arm a halt on the
    // reset vector so the firmware never runs (it may use the SWD pins or enter standby).
    t.viaReset = true;
    nrst(true);
    delay(5);
    for (uint32_t d : fallback) {
      if (d < base || d > 64) continue;
      swd::setDelay(d);
      uint32_t id;
      if (swd::connect(&id)) {
        swd::write32(DHCSR, DBGKEY | C_HALT | C_DEBUGEN);
        swd::write32(DEMCR, VC_CORERESET);
        break;
      }
    }
    nrst(false);
    delay(3);
  }
#else
  (void)useReset;
#endif

  bool ok = false;
  uint8_t ack = 0;
  for (uint32_t d : fallback) {
    if (d < base) continue;
    swd::setDelay(d);
    for (int i = 0; i < 2 && !ok; i++) {
      ok = swd::connect(&t.dpidr);
      if (!ok) {
        ack = swd::lastAck();
        delay(5);
      }
    }
    if (ok) {
      t.swdDelay = d;
      break;
    }
  }
  if (!ok) {
    swd::setDelay(base);
    bool idleHigh = swd::sampleSwdio();
    if (ack == 7 && idleHigh)
      return fail("No SWD response: SWDIO idles high, no reply (target unpowered, no GND, "
                  "or SWDIO/SWCLK swapped)");
    if (ack == 0 && !idleHigh)
      return fail("No SWD response: SWDIO is held LOW (short to GND, or firmware uses P108 as an output - "
                  "wire RES)");
    return fail("Garbled SWD reply (ack=%u, SWDIO idle %c): check levels (5V target needs a level shifter), "
                "wires",
                ack, idleHigh ? (uint32_t)'H' : (uint32_t)'L');
  }

  // Halt on the reset vector after a system reset, so clocks and peripherals are at their
  // reset state and no firmware runs while the flash is rewritten.
  swd::write32(DHCSR, DBGKEY | C_HALT | C_DEBUGEN);
  swd::write32(DEMCR, VC_CORERESET);
  swd::write32(AIRCR, AIRCR_SYSRESET);
  delay(20);
  swd::clearErrors();

  if (!waitHalted()) {
    uint32_t id;
    swd::connect(&id);
    swd::write32(DHCSR, DBGKEY | C_HALT | C_DEBUGEN);
    if (!waitHalted()) return "Could not halt the core (ID code protected?)";
  }
  swd::write32(DEMCR, 0);

  identify(t);
  return setupFlash();
}

static uint32_t roundUp(uint32_t n, uint32_t unit) { return (n + unit - 1) / unit * unit; }

const char *erase(const Target &t, const Layout &l, bool all, ProgressFn cb) {
  const uint32_t cfEnd = all && t.cfSize ? t.cfSize : roundUp(l.cfSize, CF_BLOCK);
  const uint32_t dfEnd = all ? DF_SIZE : roundUp(l.dfSize, DF_BLOCK);
  const uint32_t total = cfEnd / CF_BLOCK + dfEnd / DF_BLOCK;
  uint32_t done = 0;

  if (cfEnd) {
    if (const char *e = enterPe(true)) return e;
    for (uint32_t a = 0; a < cfEnd; a += CF_BLOCK) {
      if (const char *e = eraseBlock(a, CF_BLOCK, TMO_ERASE_CF_US, a)) return e;
      if (cb) cb("erase", ++done * 100 / total);
    }
    if (const char *e = exitPe()) return e;
  }
  if (dfEnd) {
    if (const char *e = enterPe(false)) return e;
    for (uint32_t off = 0; off < dfEnd; off += DF_BLOCK) {
      if (const char *e = eraseBlock(DF_PE_BASE + off, DF_BLOCK, TMO_ERASE_DF_US, DF_BASE + off)) return e;
      if (cb) cb("erase", ++done * 100 / total);
    }
    if (const char *e = exitPe()) return e;
  }
  return nullptr;
}

const char *program(File &img, const Layout &l, ProgressFn cb) {
  static uint8_t buf[1024];
  const uint32_t total = l.cfSize + l.dfSize;
  uint8_t lastPct = 255;
  auto progress = [&](uint32_t done) {
    uint8_t pct = (uint8_t)((uint64_t)done * 100 / total);
    if (cb && pct != lastPct) cb("program", lastPct = pct);
  };

  if (l.cfSize) {
    if (const char *e = enterPe(true)) return e;
    img.seek(0);
    for (uint32_t off = 0; off < l.cfSize; off += sizeof(buf)) {
      uint32_t n = min<uint32_t>(sizeof(buf), l.cfSize - off);
      if ((uint32_t)img.read(buf, n) != n) {
        exitPe();
        return "Image read error";
      }
      for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w;
        memcpy(&w, buf + i, 4);
        if (w == 0xFFFFFFFF) continue;  // erased state
        if (const char *e = writeCodeWord(off + i, w)) return e;
      }
      progress(off + n);
      yield();
    }
    if (const char *e = exitPe()) return e;
  }

  if (l.dfSize) {
    if (const char *e = enterPe(false)) return e;
    img.seek(l.cfSize);
    for (uint32_t off = 0; off < l.dfSize; off += sizeof(buf)) {
      uint32_t n = min<uint32_t>(sizeof(buf), l.dfSize - off);
      if ((uint32_t)img.read(buf, n) != n) {
        exitPe();
        return "Image read error";
      }
      for (uint32_t i = 0; i < n; i++) {
        if (buf[i] == 0xFF) continue;
        if (const char *e = writeDataByte(off + i, buf[i])) return e;
      }
      progress(l.cfSize + off + n);
      yield();
    }
    if (const char *e = exitPe()) return e;
  }
  return nullptr;
}

const char *verify(File &img, const Layout &l, ProgressFn cb) {
  static uint32_t words[256];
  static uint8_t ref[1024];
  const uint32_t total = l.cfSize + l.dfSize;
  uint32_t done = 0;
  uint8_t lastPct = 255;
  const struct {
    uint32_t fileOff, addr, size;
  } regions[] = {{0, CF_BASE, l.cfSize}, {l.cfSize, DF_BASE, l.dfSize}};

  for (const auto &r : regions) {
    img.seek(r.fileOff);
    for (uint32_t off = 0; off < r.size; off += sizeof(ref)) {
      uint32_t n = min<uint32_t>(sizeof(ref), r.size - off);
      if ((uint32_t)img.read(ref, n) != n) return "Image read error";
      uint32_t nw = (n + 3) / 4;
      for (uint32_t i = n; i < nw * 4; i++) ref[i] = 0xFF;  // past the image end: erased
      if (!swd::readBlock32(r.addr + off, words, nw)) return fail("SWD read failed at 0x%08X", r.addr + off);
      if (memcmp(words, ref, nw * 4) != 0) {
        const uint8_t *got = (const uint8_t *)words;
        uint32_t i = 0;
        while (got[i] == ref[i]) i++;
        i &= ~3u;
        uint32_t exp, rd;
        memcpy(&exp, ref + i, 4);
        memcpy(&rd, got + i, 4);
        return fail("Verify mismatch at 0x%08X: read 0x%08X, expected 0x%08X", r.addr + off + i, rd, exp);
      }
      done += n;
      uint8_t pct = (uint8_t)((uint64_t)done * 100 / total);
      if (cb && pct != lastPct) cb("verify", lastPct = pct);
      yield();
    }
  }
  return nullptr;
}

void detach() {
  swd::powerDown();
  swd::release();
}

void resetAndRun(bool useReset) {
  swd::write32(DEMCR, 0);
  swd::write32(DHCSR, DBGKEY);  // debug off: the core runs after the reset
#if PIN_NRST >= 0
  if (useReset) {
    detach();
    nrst(true);
    delay(2);
    nrst(false);
    return;
  }
#else
  (void)useReset;
#endif
  swd::write32(AIRCR, AIRCR_SYSRESET);
  delay(2);
  detach();
}

}  // namespace ra
