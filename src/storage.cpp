#include "storage.h"

#include <LittleFS.h>

namespace storage {

ImageMeta meta;
Config cfg;
Stats stats;

static constexpr const char *META_PATH = "/fw.meta";
static constexpr const char *CFG_PATH = "/config.txt";
static constexpr const char *STATS_PATH = "/stats.txt";

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;  // zlib-compatible (matches Python zlib.crc32)
  while (len--) {
    crc ^= *data++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
  }
  return ~crc;
}

// Calls fn(key, value) for every "key=value" line of a file.
template <typename F>
static void readKv(const char *path, F fn) {
  File f = LittleFS.open(path, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    fn(line.substring(0, eq), line.substring(eq + 1));
  }
}

static uint32_t parseNum(const String &s) { return strtoul(s.c_str(), nullptr, 0); }

static void loadMeta() {
  meta = ImageMeta();
  readKv(META_PATH, [](const String &k, const String &v) {
    if (k == "size") meta.size = parseNum(v);
    else if (k == "crc") meta.crc = parseNum(v);
    else if (k == "name") strlcpy(meta.name, v.c_str(), sizeof(meta.name));
    else if (k == "cf") meta.cfSize = parseNum(v);
    else if (k == "df") meta.dfSize = parseNum(v);
  });
  File f = LittleFS.open(IMG_PATH, "r");
  meta.valid = f && meta.size > 0 && f.size() == meta.size && meta.cfSize + meta.dfSize == meta.size;
}

bool setConfig(const char *key, const char *value) {
  String k(key);
  uint32_t n = strtoul(value, nullptr, 0);
  if (k == "mode") cfg.mode = (strcmp(value, "auto") == 0 || n == 1) ? MODE_AUTO : MODE_BUTTON;
  else if (k == "verify") cfg.verify = n != 0;
  else if (k == "run") cfg.run = n != 0;
  else if (k == "settle") cfg.settleMs = n;
  else if (k == "erase") cfg.erase = (strcmp(value, "blocks") == 0 || n == 1) ? ERASE_BLOCKS : ERASE_CHIP;
  else if (k == "reset") cfg.reset = n != 0;
  else if (k == "swdelay") cfg.swdDelay = n;
  else return false;
  return true;
}

String configString() {
  String s;
  s += "mode=";
  s += cfg.mode == MODE_AUTO ? "auto" : "button";
  s += " verify=" + String(cfg.verify);
  s += " run=" + String(cfg.run);
  s += " settle=" + String(cfg.settleMs);
  s += " erase=";
  s += cfg.erase == ERASE_BLOCKS ? "blocks" : "chip";
  s += " reset=" + String(cfg.reset);
  s += " swdelay=" + String(cfg.swdDelay);
  return s;
}

static void loadConfig() {
  cfg = Config();
  readKv(CFG_PATH, [](const String &k, const String &v) { setConfig(k.c_str(), v.c_str()); });
}

static void loadStats() {
  stats = Stats();
  readKv(STATS_PATH, [](const String &k, const String &v) {
    if (k == "pass") stats.pass = parseNum(v);
    else if (k == "fail") stats.fail = parseNum(v);
  });
}

bool begin() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    if (!LittleFS.begin()) return false;
  }
  loadConfig();
  loadStats();
  loadMeta();
  return true;
}

void saveMeta() {
  File f = LittleFS.open(META_PATH, "w");
  f.printf("size=%u\ncrc=0x%08X\nname=%s\ncf=%u\ndf=%u\n", meta.size, meta.crc, meta.name, meta.cfSize,
           meta.dfSize);
}

void saveConfig() {
  File f = LittleFS.open(CFG_PATH, "w");
  String s = configString();
  s.replace(' ', '\n');
  f.println(s);
}

void saveStats() {
  File f = LittleFS.open(STATS_PATH, "w");
  f.printf("pass=%u\nfail=%u\n", stats.pass, stats.fail);
}

uint32_t fsFree() {
  FSInfo info;
  LittleFS.info(info);
  return info.totalBytes - info.usedBytes;
}

}  // namespace storage
