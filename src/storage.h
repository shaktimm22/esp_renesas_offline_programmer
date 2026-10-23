#pragma once
// LittleFS-backed persistence: the firmware image (+ metadata), settings, counters.
#include <Arduino.h>

namespace storage {

static constexpr const char *IMG_PATH = "/fw.bin";
static constexpr const char *IMG_TMP = "/upload.tmp";

struct ImageMeta {
  bool valid = false;
  uint32_t size = 0;
  uint32_t crc = 0;  // CRC-32 (zlib) of the uploaded file
  char name[48] = "";
  uint32_t cfSize = 0;  // code flash bytes at the start of the file (from address 0)
  uint32_t dfSize = 0;  // data flash bytes after them (from 0x40100000)
};

enum Mode : uint8_t { MODE_BUTTON = 0, MODE_AUTO = 1 };
enum Erase : uint8_t { ERASE_CHIP = 0, ERASE_BLOCKS = 1 };

struct Config {
  uint8_t mode = MODE_BUTTON;
  bool verify = true;
  bool run = true;          // reset target and run after programming
  uint32_t settleMs = 500;  // auto mode: wait after target detected
  uint8_t erase = ERASE_CHIP;
  bool reset = true;        // use the RES line (connect under reset, reset to run)
  uint32_t swdDelay = 4;    // 0 is too fast for some boards; attach/probe slow down further if needed
};

struct Stats {
  uint32_t pass = 0;
  uint32_t fail = 0;
};

extern ImageMeta meta;
extern Config cfg;
extern Stats stats;

bool begin();
void saveMeta();
void saveConfig();
void saveStats();
uint32_t fsFree();

// key/value config setter used by the SET command; returns false on unknown key or value
bool setConfig(const char *key, const char *value);
String configString();

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len);

}  // namespace storage
