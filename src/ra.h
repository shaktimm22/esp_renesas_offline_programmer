#pragma once
// Renesas RA2E1 (Cortex-M23, MF4 low-power flash "FLASH_LP") programming over SWD.
// The flash sequencer is driven register by register from the debugger while the core is
// halted, following the FSP r_flash_lp driver (FSP 6.3.1, BSP_FEATURE_FLASH_LP_VERSION 4).
#include <Arduino.h>
#include <FS.h>

namespace ra {

static constexpr uint32_t CF_BASE = 0x00000000;   // code flash
static constexpr uint32_t CF_BLOCK = 2048;        // erase block
static constexpr uint32_t CF_MAX = 128 * 1024;    // largest RA2E1 (R7FA2E1A9)
static constexpr uint32_t DF_BASE = 0x40100000;   // data flash (read address)
static constexpr uint32_t DF_BLOCK = 1024;
static constexpr uint32_t DF_SIZE = 4096;

struct Target {
  uint32_t dpidr = 0;
  char part[20] = "";     // part number from the PNR, e.g. "R7FA2E1A72DFL"; "" = unreadable
  char uid[33] = "";      // 128-bit unique ID as hex
  uint32_t cfSize = 0;    // code flash size from the part number; 0 = unknown
  uint32_t swdDelay = 0;  // SWD delay that worked for this connection
  bool viaReset = false;  // the RES line was used to connect
  uint16_t fawsmr = 0;    // access window start / end monitors (FAWSMR, FAWEMR)
  uint16_t fawemr = 0;
};

// Stored image: code flash bytes from address 0 (cfSize, multiple of 4), then data flash
// bytes from DF_BASE (dfSize). Gaps are 0xFF and are skipped while programming.
struct Layout {
  uint32_t cfSize = 0;
  uint32_t dfSize = 0;
};

typedef void (*ProgressFn)(const char *stage, uint8_t pct);

// All operations return nullptr on success or an error message.
const char *attach(Target &t, bool useReset);  // connect, reset-halt, identify, set up flash clock
// Erase: all = every code + data flash block of the chip; otherwise only the blocks the image covers.
const char *erase(const Target &t, const Layout &l, bool all, ProgressFn cb);
const char *program(File &img, const Layout &l, ProgressFn cb);
const char *verify(File &img, const Layout &l, ProgressFn cb);
void resetAndRun(bool useReset);  // leave debug mode and start the target firmware
void detach();                    // leave debug mode, target stays as it is
bool probe();                     // quick check: does anything answer on SWD?

}  // namespace ra
