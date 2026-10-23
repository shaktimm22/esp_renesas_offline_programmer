// Offline programmer for Renesas RA2E1 on an ESP8266: SWD, image stored in LittleFS.
// The PC UI talks to this over UART with a line protocol (see README.md):
// every reply starts with "@OK" / "@ERR", asynchronous events with "@EVT".
// The web UI (web.cpp) sends the same commands over HTTP and receives the same events.
#include <Arduino.h>
#include <LittleFS.h>
#include <StreamString.h>

#include "pins.h"
#include "ra.h"
#include "storage.h"
#include "target.h"
#include "web.h"

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 460800
#endif

using storage::cfg;
using storage::meta;
using storage::stats;

// ---------------------------------------------------------------- indicators

static void ledBusy(bool on) { digitalWrite(PIN_LED_BUSY, on ? LOW : HIGH); }

static void ledResult(int result) {  // 1 pass, 0 fail, -1 off
  digitalWrite(PIN_LED_PASS, result == 1);
  digitalWrite(PIN_LED_FAIL, result == 0);
}

static void beep(uint32_t ms, uint8_t times) {
#if PIN_BUZZER >= 0
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(ms);
    digitalWrite(PIN_BUZZER, LOW);
    if (i + 1 < times) delay(ms);
  }
#else
  (void)ms;
  (void)times;
#endif
}

// ---------------------------------------------------------------- events

// Sends "@EVT <...>" to the UART and to the web UI's event queue.
void target::evt(const char *fmt, ...) {
  char buf[web::EVT_LEN];
  int n = snprintf(buf, sizeof(buf), "@EVT ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  Serial.println(buf);
  web::pushEvent(buf);
}

using target::evt;

static void evtCount() { evt("COUNT %u %u", stats.pass, stats.fail); }

static void onProgress(const char *stage, uint8_t pct) {
  static uint32_t lastMs = 0;
  if (pct == 100 || millis() - lastMs > 150) {
    lastMs = millis();
    evt("PROGRESS %s %u", stage, pct);
  }
  ledBusy((millis() / 100) & 1);
  web::loop();  // keep the web UI responsive (status and progress only) while flashing
}

// ---------------------------------------------------------------- flashing

static bool s_busy = false;
static bool s_flashPending = false;  // FLASH received; runs from loop() once the reply is sent

static void runFlash() {
  if (s_busy) return;
  s_busy = true;
  uint32_t t0 = millis();
  ledResult(-1);
  ledBusy(true);

  target::Result r;
  File img;
  if (!meta.valid) {
    r.err = "No firmware image stored - upload one first";
    r.counted = false;
  } else if (!(img = LittleFS.open(storage::IMG_PATH, "r"))) {
    r.err = "Cannot open stored image";
  } else {
    target::flash(img, onProgress, r);
  }
  if (img) img.close();

  uint32_t ms = millis() - t0;
  ledBusy(false);
  if (!r.err) {
    stats.pass++;
    storage::saveStats();
    evt("RESULT PASS %u %s %s", ms, r.chip, r.id);
    ledResult(1);
    beep(80, 1);
  } else {
    if (r.counted) {
      stats.fail++;
      storage::saveStats();
    }
    evt("RESULT FAIL %d %s", r.counted ? 1 : 0, r.err);
    ledResult(0);
    beep(150, 3);
  }
  evtCount();
  evt("STATE idle");
  s_busy = false;
}

// ---------------------------------------------------------------- auto detect

enum AutoState { AUTO_ARMED, AUTO_SETTLING, AUTO_WAIT_REMOVAL };
static AutoState s_auto = AUTO_ARMED;
static uint32_t s_autoTimer = 0;
static uint8_t s_autoHits = 0;
static constexpr uint32_t PROBE_MS = 200;

static void pollAuto() {
  if (cfg.mode != storage::MODE_AUTO || s_busy) return;
  if (millis() - s_autoTimer < PROBE_MS) return;
  bool present = target::probe();

  switch (s_auto) {
    case AUTO_ARMED:
      s_autoHits = present ? s_autoHits + 1 : 0;
      if (s_autoHits >= 2) {
        evt("TARGET present");
        s_auto = AUTO_SETTLING;
        s_autoTimer = millis();
        return;
      }
      break;
    case AUTO_SETTLING:
      if (millis() - s_autoTimer < cfg.settleMs) return;
      runFlash();
      s_auto = AUTO_WAIT_REMOVAL;
      s_autoHits = 0;
      break;
    case AUTO_WAIT_REMOVAL:
      s_autoHits = present ? 0 : s_autoHits + 1;
      if (s_autoHits >= 3) {  // gone for 3 probes
        evt("TARGET absent");
        s_auto = AUTO_ARMED;
        s_autoHits = 0;
      }
      break;
  }
  s_autoTimer = millis();
}

// ---------------------------------------------------------------- button

static void pollButton() {
  static bool last = true;
  static uint32_t changed = 0;
  bool now = digitalRead(PIN_BUTTON);
  if (now != last && millis() - changed > 30) {
    changed = millis();
    last = now;
    if (!now) runFlash();  // pressed (active low)
  }
}

// ---------------------------------------------------------------- upload

static File s_up;
static uint32_t s_upSize = 0, s_upCrc = 0, s_upRecv = 0;
static storage::ImageMeta s_upMeta;

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Appends a chunk to the image being uploaded; nullptr on success.
static const char *uploadWrite(const uint8_t *data, size_t n) {
  if (!s_up) return "no upload in progress";
  if (s_upRecv + n > s_upSize) return "data beyond declared size";
  if (s_up.write(data, n) != n) return "filesystem write failed";
  s_upCrc = storage::crc32Update(s_upCrc, data, n);
  s_upRecv += n;
  return nullptr;
}

static void uploadAbort() {
  if (s_up) s_up.close();
  LittleFS.remove(storage::IMG_TMP);
}

// ---------------------------------------------------------------- commands

static Print *s_out = &Serial;  // where replies go: the UART, or a web request's buffer

void target::reply(const char *cmd, const String &s) {
  s_out->printf("@OK %s%s%s\n", cmd, s.length() ? " " : "", s.c_str());
}
void target::replyErr(const char *cmd, const char *msg) { s_out->printf("@ERR %s %s\n", cmd, msg); }

using target::reply;
using target::replyErr;

static String infoString() {
  String s;
  s += "fw=" FW_VERSION;
  s += " img=" + String(meta.valid);
  s += " size=" + String(meta.size);
  char buf[24];
  snprintf(buf, sizeof(buf), " crc=0x%08X", meta.crc);
  s += buf;
  s += " name=" + String(meta.name[0] ? meta.name : "-");
  s += " cf=" + String(meta.cfSize) + " df=" + String(meta.dfSize);
  s += " pass=" + String(stats.pass) + " fail=" + String(stats.fail);
  s += " free=" + String(storage::fsFree());
  s += " " + storage::configString();
  return s;
}

static void handleLine(char *line) {
  char *args = strchr(line, ' ');
  if (args) *args++ = 0;
  else args = line + strlen(line);
  String cmd(line);
  cmd.toUpperCase();
  const char *c = cmd.c_str();

  if (cmd == "PING") {
    reply(c, "RAFLASHER " FW_VERSION);
  } else if (cmd == "INFO") {
    reply(c, infoString());
  } else if (cmd == "BEGIN") {
    // BEGIN <size> <crc32> <code flash bytes> <data flash bytes> <name...>
    char *save;
    char *tSize = strtok_r(args, " ", &save);
    char *tCrc = strtok_r(nullptr, " ", &save);
    char *tCf = strtok_r(nullptr, " ", &save);
    char *tDf = strtok_r(nullptr, " ", &save);
    char *name = strtok_r(nullptr, "", &save);
    if (!tSize || !tCrc || !tCf || !tDf) return replyErr(c, "usage: BEGIN size crc cf df name");
    s_upMeta = storage::ImageMeta();
    s_upSize = strtoul(tSize, nullptr, 0);
    s_upMeta.size = s_upSize;
    s_upMeta.crc = strtoul(tCrc, nullptr, 0);
    s_upMeta.cfSize = strtoul(tCf, nullptr, 0);
    s_upMeta.dfSize = strtoul(tDf, nullptr, 0);
    strlcpy(s_upMeta.name, name ? name : "", sizeof(s_upMeta.name));
    for (char *p = s_upMeta.name; *p; p++)
      if (*p == ' ') *p = '_';
    if (s_upSize == 0 || s_upMeta.cfSize + s_upMeta.dfSize != s_upSize)
      return replyErr(c, "size must equal code flash + data flash bytes");
    if (s_upMeta.cfSize % 4 || s_upMeta.cfSize > ra::CF_MAX) return replyErr(c, "bad code flash size");
    if (s_upMeta.dfSize > ra::DF_SIZE) return replyErr(c, "bad data flash size");
    if (s_upSize + 8192 > storage::fsFree() + (meta.valid ? meta.size : 0))
      return replyErr(c, "image too large for programmer storage");
    if (s_up) s_up.close();
    s_up = LittleFS.open(storage::IMG_TMP, "w");
    if (!s_up) return replyErr(c, "cannot create file");
    s_upRecv = 0;
    s_upCrc = 0;
    reply(c);
  } else if (cmd == "DATA") {
    // DATA <offset> <hex bytes>
    if (!s_up) return replyErr(c, "no upload in progress");
    char *hex = strchr(args, ' ');
    if (!hex) return replyErr(c, "usage: DATA offset hex");
    *hex++ = 0;
    uint32_t off = strtoul(args, nullptr, 0);
    if (off != s_upRecv) {
      char m[48];
      snprintf(m, sizeof(m), "expected offset %u", s_upRecv);
      return replyErr(c, m);
    }
    static uint8_t buf[300];
    size_t n = 0;
    for (; hex[0] && hex[1] && n < sizeof(buf); hex += 2) {
      int hi = hexNibble(hex[0]), lo = hexNibble(hex[1]);
      if (hi < 0 || lo < 0) return replyErr(c, "bad hex");
      buf[n++] = (hi << 4) | lo;
    }
    if (const char *err = uploadWrite(buf, n)) return replyErr(c, err);
    reply(c, String(s_upRecv));
  } else if (cmd == "END") {
    if (!s_up) return replyErr(c, "no upload in progress");
    s_up.close();
    if (s_upRecv != s_upSize || s_upCrc != s_upMeta.crc) {
      LittleFS.remove(storage::IMG_TMP);
      return replyErr(c, "size/CRC mismatch, upload discarded");
    }
    LittleFS.remove(storage::IMG_PATH);
    LittleFS.rename(storage::IMG_TMP, storage::IMG_PATH);
    meta = s_upMeta;
    meta.valid = true;
    storage::saveMeta();
    reply(c, infoString());
  } else if (cmd == "ABORT") {
    uploadAbort();
    reply(c);
  } else if (cmd == "FLASH") {
    reply(c);
    s_flashPending = true;
  } else if (cmd == "SET") {
    char *val = strchr(args, ' ');
    if (!val) return replyErr(c, "usage: SET key value");
    *val++ = 0;
    if (!storage::setConfig(args, val)) return replyErr(c, "unknown key or value");
    storage::saveConfig();
    target::begin();
    s_auto = AUTO_ARMED;
    s_autoHits = 0;
    reply(c, storage::configString());
  } else if (cmd == "RESETCNT") {
    stats = storage::Stats();
    storage::saveStats();
    reply(c);
    evtCount();
  } else if (!target::command(c, args)) {
    replyErr(c, "unknown command");
  }
}

static void pollSerial() {
  static char line[700];
  static size_t len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (overflow) replyErr("LINE", "too long");
      else if (len) {
        line[len] = 0;
        handleLine(line);
      }
      len = 0;
      overflow = false;
    } else if (len < sizeof(line) - 1) {
      line[len++] = ch;
    } else {
      overflow = true;
    }
  }
}

// Runs one protocol line for the web UI and returns its reply ("@OK ..." / "@ERR ...").
static String webCommand(const char *line) {
  static char buf[256];
  strlcpy(buf, line, sizeof(buf));
  StreamString out;
  s_out = &out;
  handleLine(buf);
  s_out = &Serial;
  out.trim();
  return out;
}

static bool webBusy() { return s_busy || s_flashPending; }

// ---------------------------------------------------------------- main

void setup() {
  pinMode(PIN_LED_BUSY, OUTPUT);
  pinMode(PIN_LED_PASS, OUTPUT);
  pinMode(PIN_LED_FAIL, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#if PIN_NRST >= 0
  pinMode(PIN_NRST, INPUT);  // released; the target pulls RES up
#endif
#if PIN_BUZZER >= 0
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
#endif
  ledBusy(false);
  ledResult(-1);

  Serial.setRxBufferSize(1024);
  Serial.begin(SERIAL_BAUD);
  Serial.println();

  if (!storage::begin()) evt("ERROR filesystem mount failed");
  target::begin();

  web::begin({webCommand, webBusy, uploadWrite});

  evt("BOOT RAFLASHER " FW_VERSION);
  evtCount();
}

void loop() {
  pollSerial();
  web::loop();
  if (s_flashPending) {
    s_flashPending = false;
    runFlash();
  }
  pollButton();
  pollAuto();
}
