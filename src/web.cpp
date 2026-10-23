#include "web.h"

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include "web_page.h"  // generated from web/index.html by tools/embed_web.py

namespace web {

static const IPAddress AP_IP(192, 168, 4, 1);

static Hooks s_hooks;
static ESP8266WebServer s_server(80);
static DNSServer s_dns;
static bool s_started = false;

// ---------------------------------------------------------------- event queue

static constexpr uint32_t EVT_COUNT = 24;
static char s_events[EVT_COUNT][EVT_LEN];
static uint32_t s_seq = 0;  // number of events pushed since boot

void pushEvent(const char *line) {
  strlcpy(s_events[s_seq % EVT_COUNT], line, EVT_LEN);
  s_seq++;
}

static void announce(const char *fmt, ...) {
  char buf[EVT_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  pushEvent(buf);
}

// ---------------------------------------------------------------- captive portal

// Phones and laptops probe a known URL (generate_204, hotspot-detect.html, connecttest.txt, ...)
// right after joining. The DNS server answers every name with our IP, and any request that
// is not addressed to us is redirected to the UI, so the OS opens its "sign in" sheet on it.
static bool isOurHost() {
  String host = s_server.hostHeader();
  int colon = host.indexOf(':');
  if (colon >= 0) host.remove(colon);
  return host == AP_IP.toString() || host.equalsIgnoreCase(MDNS_HOSTNAME) ||
         host.equalsIgnoreCase(MDNS_HOSTNAME ".local");
}

static bool captiveRedirect() {
  if (isOurHost()) return false;
  s_server.sendHeader("Location", "http://" + AP_IP.toString() + "/", true);
  s_server.send(302, "text/plain", "");
  return true;
}

// ---------------------------------------------------------------- routes

static void handleRoot() {
  if (captiveRedirect()) return;
  s_server.sendHeader("Content-Encoding", "gzip");
  s_server.sendHeader("Cache-Control", "no-cache");
  s_server.send_P(200, "text/html", (PGM_P)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
}

static void handleNotFound() {
  if (captiveRedirect()) return;
  s_server.send(404, "text/plain", "not found");
}

static void sendText(const String &s) {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(200, "text/plain", s);
}

// Commands that only read programmer state are allowed while a flash is running.
static bool readOnly(const String &line) {
  String cmd = line.substring(0, line.indexOf(' ') < 0 ? line.length() : line.indexOf(' '));
  cmd.toUpperCase();
  return cmd == "PING" || cmd == "INFO";
}

static void handleCmd() {
  String line = s_server.arg("c");
  line.trim();
  if (!line.length()) return s_server.send(400, "text/plain", "missing c");
  if (s_hooks.busy() && !readOnly(line)) {
    String cmd = line.substring(0, line.indexOf(' ') < 0 ? line.length() : line.indexOf(' '));
    cmd.toUpperCase();
    return sendText("@ERR " + cmd + " programmer is busy flashing");
  }
  sendText(s_hooks.command(line.c_str()));
}

static void handleEvents() {
  uint32_t since = strtoul(s_server.arg("since").c_str(), nullptr, 10);
  uint32_t oldest = s_seq > EVT_COUNT ? s_seq - EVT_COUNT : 0;
  if (since > s_seq || since < oldest) since = oldest;  // rebooted, or the browser fell behind
  String out;
  out.reserve(32 + (s_seq - since) * 48);
  out += String(s_seq) + " " + (s_hooks.busy() ? "1" : "0") + "\n";
  for (uint32_t i = since; i < s_seq; i++) {
    out += s_events[i % EVT_COUNT];
    out += '\n';
  }
  sendText(out);
}

// Upload: the browser parses the .hex itself and posts the binary image as a multipart file;
// it goes through the same BEGIN / write / END path as a UART upload (size + CRC checked).
static String s_upReply;
static bool s_upFailed = false;

static String token(String s, const char *fallback) {
  s.trim();
  s.replace(' ', '_');
  return s.length() ? s : String(fallback);
}

static void handleUploadData() {
  HTTPUpload &up = s_server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      s_upFailed = true;
      if (s_hooks.busy()) {
        s_upReply = "@ERR BEGIN programmer is busy flashing";
        break;
      }
      String fields = s_server.arg("fields");
      fields.trim();
      String line = "BEGIN " + token(s_server.arg("size"), "0") + " " + token(s_server.arg("crc"), "0") + " " +
                    fields + " " + token(s_server.arg("name"), "web_upload");
      s_upReply = s_hooks.command(line.c_str());
      s_upFailed = !s_upReply.startsWith("@OK");
      break;
    }
    case UPLOAD_FILE_WRITE:
      if (s_upFailed) break;
      if (const char *err = s_hooks.uploadWrite(up.buf, up.currentSize)) {
        s_upReply = String("@ERR DATA ") + err;
        s_upFailed = true;
      }
      break;
    case UPLOAD_FILE_END:
      if (s_upFailed) break;
      s_upReply = s_hooks.command("END");
      s_upFailed = !s_upReply.startsWith("@OK");
      break;
    case UPLOAD_FILE_ABORTED:
      s_upReply = "@ERR UPLOAD connection lost";
      s_upFailed = true;
      s_hooks.command("ABORT");
      break;
  }
}

static void handleUploadDone() {
  if (!s_upReply.length()) s_upReply = "@ERR UPLOAD no file received";
  if (s_upFailed) s_hooks.command("ABORT");
  sendText(s_upReply);
  s_upReply = String();
}

// ---------------------------------------------------------------- setup / loop

void begin(const Hooks &hooks) {
  s_hooks = hooks;

  WiFi.persistent(false);  // don't write Wi-Fi settings to flash on every boot
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);
  char ssid[33];
  snprintf(ssid, sizeof(ssid), "%s-%02X%02X", WIFI_AP_SSID, mac[4], mac[5]);
  const char *pass = WIFI_AP_PASS;
  if (*pass && strlen(pass) < 8) {
    announce("@EVT ERROR WIFI_AP_PASS must be 8..63 characters - starting an open network");
    pass = "";
  }
  if (!WiFi.softAP(ssid, *pass ? pass : nullptr, WIFI_AP_CHANNEL)) {
    announce("@EVT ERROR Wi-Fi access point failed to start");
    return;
  }

  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", AP_IP);

  if (MDNS.begin(MDNS_HOSTNAME)) MDNS.addService("http", "tcp", 80);

  s_server.on("/", HTTP_GET, handleRoot);
  s_server.on("/api/cmd", HTTP_ANY, handleCmd);
  s_server.on("/api/events", HTTP_GET, handleEvents);
  s_server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadData);
  s_server.onNotFound(handleNotFound);
  s_server.begin();
  s_started = true;

  announce("@EVT WIFI ssid=%s ip=%s url=http://" MDNS_HOSTNAME ".local %s", ssid, AP_IP.toString().c_str(),
           *pass ? "secured" : "open");
}

void loop() {
  static bool inLoop = false;
  if (!s_started || inLoop) return;
  inLoop = true;
  s_dns.processNextRequest();
  s_server.handleClient();
  MDNS.update();
  inLoop = false;
}

}  // namespace web
