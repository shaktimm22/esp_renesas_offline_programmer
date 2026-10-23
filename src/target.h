#pragma once
// One full programming cycle and the target maintenance commands (DETECT, ERASE, RESET, READ),
// on top of ra.cpp. main.cpp owns the protocol, the image upload, the button / auto-detect
// and the result reporting.
#include <Arduino.h>
#include <FS.h>

namespace target {

typedef void (*ProgressFn)(const char *stage, uint8_t pct);

struct Result {
  const char *err = nullptr;  // nullptr = PASS
  bool counted = true;        // false: no target answered, so it is not counted as a failed device
  char chip[20] = "";         // part number for the RESULT / CHIP events
  char id[33] = "";           // unique ID
};

void begin();                                   // target lines to their idle state, apply settings
void flash(File &img, ProgressFn cb, Result &r);  // one full programming cycle
bool probe();                                   // quick check for auto-detect: is a target there?
bool command(const char *cmd, char *args);      // target commands; false = not one of them

// Provided by main.cpp.
void reply(const char *cmd, const String &s = String());
void replyErr(const char *cmd, const char *msg);
void evt(const char *fmt, ...);

}  // namespace target
