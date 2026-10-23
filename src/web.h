#pragma once
// Wi-Fi access point + captive portal + mDNS + HTTP server for the browser UI.
// The browser talks the same line protocol as the UART (see README.md):
//   POST /api/cmd     c=<command line>   -> "@OK <CMD> ..." / "@ERR <CMD> ..."
//   GET  /api/events  ?since=<seq>       -> "<seq> <busy>\n" then the "@EVT ..." lines after <seq>
//   POST /api/upload  ?size&crc&fields&name, multipart file -> the END reply
//                     (fields = "<code flash bytes> <data flash bytes>", as in BEGIN)
#include <Arduino.h>

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "RA-Flasher"  // the last 4 hex digits of the MAC are appended
#endif
#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS ""  // empty = open network; otherwise 8..63 characters
#endif
#ifndef WIFI_AP_CHANNEL
#define WIFI_AP_CHANNEL 6
#endif
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "ra-flasher"  // http://ra-flasher.local
#endif

namespace web {

static constexpr size_t EVT_LEN = 200;  // longest "@EVT ..." line kept for the browser

struct Hooks {
  String (*command)(const char *line);                 // run one protocol line, return its reply
  bool (*busy)();                                      // a flash is running or queued
  const char *(*uploadWrite)(const uint8_t *, size_t);  // append image data, nullptr on success
};

void begin(const Hooks &hooks);
void loop();  // safe to call from inside long operations; never re-enters a request handler
void pushEvent(const char *line);

}  // namespace web
