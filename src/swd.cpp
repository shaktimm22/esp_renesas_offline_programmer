#include "swd.h"

#include <Arduino.h>

#include "pins.h"

namespace swd {

// Request bits (as in CMSIS-DAP): bit0 APnDP, bit1 RnW, bits2-3 A[3:2]
static constexpr uint8_t REQ_AP = 0x01;
static constexpr uint8_t REQ_RD = 0x02;

static constexpr uint8_t DP_IDCODE = 0x00;  // read
static constexpr uint8_t DP_ABORT = 0x00;   // write
static constexpr uint8_t DP_CTRLSTAT = 0x04;
static constexpr uint8_t DP_SELECT = 0x08;
static constexpr uint8_t DP_RDBUFF = 0x0C;

static constexpr uint8_t AP_CSW = 0x00;
static constexpr uint8_t AP_TAR = 0x04;
static constexpr uint8_t AP_DRW = 0x0C;

static constexpr uint32_t CSW_BASE = 0x23000000;  // MasterType=debug, HPROT privileged
static constexpr uint32_t CSW_SIZE8 = 0x00;
static constexpr uint32_t CSW_SIZE16 = 0x01;
static constexpr uint32_t CSW_SIZE32 = 0x02;
static constexpr uint32_t CSW_INC = 0x10;

static constexpr uint32_t WAIT_RETRIES = 20000;
static constexpr uint32_t INVALID = 0xFFFFFFFF;

static constexpr uint32_t CLK = 1UL << PIN_SWCLK;
static constexpr uint32_t DIO = 1UL << PIN_SWDIO;

static uint32_t s_delay = 0;
static uint8_t s_lastAck = 0;
static uint32_t s_select = INVALID;  // cached DP SELECT
static uint32_t s_csw = INVALID;     // cached AP CSW
static uint32_t s_tar = INVALID;     // cached TAR (only valid without auto-increment)

// ---------------------------------------------------------------- bit level

static inline void IRAM_ATTR dly() {
  for (volatile uint32_t i = s_delay; i; --i) {
  }
}
static inline void IRAM_ATTR clkLo() { GPOC = CLK; }
static inline void IRAM_ATTR clkHi() { GPOS = CLK; }
static inline void IRAM_ATTR dioSet(uint32_t b) {
  if (b & 1) GPOS = DIO;
  else GPOC = DIO;
}
static inline uint32_t IRAM_ATTR dioGet() { return (GPI & DIO) ? 1 : 0; }
static inline void IRAM_ATTR dioDrive() { GPES = DIO; }
static inline void IRAM_ATTR dioRelease() { GPEC = DIO; }

// Host changes SWDIO while SWCLK is low; target samples on the rising edge.
static inline void IRAM_ATTR writeBit(uint32_t b) {
  dioSet(b);
  clkLo();
  dly();
  clkHi();
  dly();
}
// Target drives SWDIO after the rising edge; host samples before the next one.
static inline uint32_t IRAM_ATTR readBit() {
  clkLo();
  dly();
  uint32_t b = dioGet();
  clkHi();
  dly();
  return b;
}
static inline void IRAM_ATTR cycle() {
  clkLo();
  dly();
  clkHi();
  dly();
}

// One SWD packet. Returns the 3-bit ACK (or ACK_PARITY on read parity error).
static uint8_t IRAM_ATTR xfer(uint8_t req, uint32_t *data) {
  uint32_t r = req & 0x0F;
  uint32_t par = (r ^ (r >> 1) ^ (r >> 2) ^ (r >> 3)) & 1;
  uint32_t hdr = 0x81 | (r << 1) | (par << 5);  // start, ..., parity, stop=0, park=1

  dioDrive();
  for (int i = 0; i < 8; i++) writeBit(hdr >> i);
  dioRelease();
  cycle();  // turnaround

  uint32_t ack = readBit();
  ack |= readBit() << 1;
  ack |= readBit() << 2;

  if (ack == ACK_OK) {
    if (req & REQ_RD) {
      uint32_t v = 0, p = 0;
      for (int i = 0; i < 32; i++) {
        uint32_t b = readBit();
        v |= b << i;
        p ^= b;
      }
      uint32_t pb = readBit();
      cycle();  // turnaround
      dioDrive();
      if (pb != p) ack = ACK_PARITY;
      else if (data) *data = v;
    } else {
      cycle();  // turnaround
      dioDrive();
      uint32_t v = *data, p = 0;
      for (int i = 0; i < 32; i++) {
        writeBit(v >> i);
        p ^= (v >> i) & 1;
      }
      writeBit(p);
    }
    dioSet(0);
    cycle();  // idle cycles let the write complete
    cycle();
    dioSet(1);
    return ack;
  }

  if (ack == ACK_WAIT || ack == ACK_FAULT) {
    cycle();  // turnaround
    dioDrive();
    dioSet(1);
    return ack;
  }

  // No/invalid response: back off a full data phase.
  for (int i = 0; i < 34; i++) cycle();
  dioDrive();
  dioSet(1);
  return ack;
}

static void invalidateCache() {
  s_select = INVALID;
  s_csw = INVALID;
  s_tar = INVALID;
}

static bool xferRetry(uint8_t req, uint32_t *data) {
  uint8_t a = 0;
  for (uint32_t i = 0; i < WAIT_RETRIES; i++) {
    a = xfer(req, data);
    if (a != ACK_WAIT) break;
  }
  s_lastAck = a;
  if (a == ACK_OK) return true;
  if (a == ACK_FAULT) clearErrors();
  invalidateCache();
  return false;
}

// ---------------------------------------------------------------- public

void begin() {
  pinMode(PIN_SWCLK, OUTPUT);
  digitalWrite(PIN_SWCLK, HIGH);
  pinMode(PIN_SWDIO, INPUT_PULLUP);  // keeps pull-up while toggling output enable
  digitalWrite(PIN_SWDIO, HIGH);
}

void setDelay(uint32_t loops) { s_delay = loops; }

uint32_t getDelay() { return s_delay; }

bool sampleSwdio() {
  dioRelease();
  delayMicroseconds(50);
  return dioGet();
}

void release() {
  dioRelease();
  clkHi();
}

uint8_t lastAck() { return s_lastAck; }

void clearErrors() {
  uint32_t v = 0x1E;  // STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR
  xfer(DP_ABORT, &v);
}

bool connect(uint32_t *dpidr, bool powerUp) {
  invalidateCache();
  dioDrive();
  dioSet(1);
  for (int i = 0; i < 56; i++) cycle();  // line reset
  const uint32_t jtag2swd = 0xE79E;
  for (int i = 0; i < 16; i++) writeBit(jtag2swd >> i);
  for (int i = 0; i < 56; i++) cycle();  // line reset
  dioSet(0);
  for (int i = 0; i < 4; i++) cycle();  // idle

  uint32_t id = 0;
  uint8_t a = xfer(REQ_RD | DP_IDCODE, &id);
  s_lastAck = a;
  if (a != ACK_OK || id == 0 || id == 0xFFFFFFFF) return false;
  if (dpidr) *dpidr = id;
  if (!powerUp) return true;

  clearErrors();
  if (!writeDP(DP_SELECT, 0)) return false;
  s_select = 0;
  if (!writeDP(DP_CTRLSTAT, 0x50000000)) return false;  // CSYSPWRUPREQ | CDBGPWRUPREQ
  for (int i = 0; i < 100; i++) {
    uint32_t cs;
    if (!readDP(DP_CTRLSTAT, &cs)) return false;
    if ((cs & 0xA0000000) == 0xA0000000) return true;
    delayMicroseconds(100);
  }
  return false;
}

void powerDown() {
  writeDP(DP_CTRLSTAT, 0);
  invalidateCache();
}

bool readDP(uint8_t addr, uint32_t *v) { return xferRetry(REQ_RD | (addr & 0x0C), v); }

bool writeDP(uint8_t addr, uint32_t v) { return xferRetry(addr & 0x0C, &v); }

static bool selectBank(uint8_t addr) {
  uint32_t sel = addr & 0xF0;  // APSEL = 0
  if (sel == s_select) return true;
  if (!writeDP(DP_SELECT, sel)) return false;
  s_select = sel;
  return true;
}

bool writeAP(uint8_t addr, uint32_t v) {
  return selectBank(addr) && xferRetry(REQ_AP | (addr & 0x0C), &v);
}

bool readAP(uint8_t addr, uint32_t *v) {
  uint32_t dummy;
  // AP reads are posted: the value arrives with the following RDBUFF read.
  return selectBank(addr) && xferRetry(REQ_AP | REQ_RD | (addr & 0x0C), &dummy) &&
         readDP(DP_RDBUFF, v);
}

static bool setCSW(uint32_t v) {
  if (v == s_csw) return true;
  if (!writeAP(AP_CSW, v)) return false;
  s_csw = v;
  return true;
}

static bool setTAR(uint32_t addr) {
  if (addr == s_tar) return true;
  if (!writeAP(AP_TAR, addr)) return false;
  s_tar = addr;
  return true;
}

bool read32(uint32_t addr, uint32_t *v) {
  return setCSW(CSW_BASE | CSW_SIZE32) && setTAR(addr) && readAP(AP_DRW, v);
}

// Narrow reads: the MEM-AP returns the data on the byte lanes selected by addr[1:0].
bool read16(uint32_t addr, uint16_t *v) {
  uint32_t w;
  if (!(setCSW(CSW_BASE | CSW_SIZE16) && setTAR(addr) && readAP(AP_DRW, &w))) return false;
  *v = (uint16_t)(w >> ((addr & 2) * 8));
  return true;
}

bool read8(uint32_t addr, uint8_t *v) {
  uint32_t w;
  if (!(setCSW(CSW_BASE | CSW_SIZE8) && setTAR(addr) && readAP(AP_DRW, &w))) return false;
  *v = (uint8_t)(w >> ((addr & 3) * 8));
  return true;
}

bool write32(uint32_t addr, uint32_t v) {
  return setCSW(CSW_BASE | CSW_SIZE32) && setTAR(addr) && writeAP(AP_DRW, v);
}

// Narrow writes: the value is replicated on every byte lane, so it lands on the lane addr[1:0]
// selects whatever the bus bridge expects.
bool write16(uint32_t addr, uint16_t v) {
  return setCSW(CSW_BASE | CSW_SIZE16) && setTAR(addr) &&
         writeAP(AP_DRW, (uint32_t)v | ((uint32_t)v << 16));
}

bool write8(uint32_t addr, uint8_t v) {
  return setCSW(CSW_BASE | CSW_SIZE8) && setTAR(addr) && writeAP(AP_DRW, (uint32_t)v * 0x01010101u);
}

bool readBlock32(uint32_t addr, uint32_t *buf, size_t words) {
  if (!setCSW(CSW_BASE | CSW_SIZE32 | CSW_INC)) return false;
  while (words) {
    size_t chunk = (0x400 - (addr & 0x3FF)) / 4;  // TAR auto-increment is only 10 bits
    if (chunk > words) chunk = words;
    s_tar = INVALID;
    if (!writeAP(AP_TAR, addr)) return false;
    uint32_t dummy;
    if (!xferRetry(REQ_AP | REQ_RD | AP_DRW, &dummy)) return false;
    for (size_t i = 0; i + 1 < chunk; i++) {
      if (!xferRetry(REQ_AP | REQ_RD | AP_DRW, &buf[i])) return false;
    }
    if (!readDP(DP_RDBUFF, &buf[chunk - 1])) return false;
    addr += chunk * 4;
    buf += chunk;
    words -= chunk;
  }
  return true;
}

}  // namespace swd
