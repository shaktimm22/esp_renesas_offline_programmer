#include "target.h"

#include "ra.h"
#include "storage.h"
#include "swd.h"

using storage::cfg;
using storage::meta;

namespace target {

void begin() {
  swd::begin();
  swd::setDelay(cfg.swdDelay);
}

static void finishSession(bool run) {
  if (run) ra::resetAndRun(cfg.reset);
  else ra::detach();
  swd::setDelay(cfg.swdDelay);  // attach may have slowed down for this board
}

static ra::Layout layout() {
  ra::Layout l;
  l.cfSize = meta.cfSize;
  l.dfSize = meta.dfSize;
  return l;
}

static const char *checkTarget(const ra::Target &t, char *buf, size_t len) {
  if (t.cfSize && meta.cfSize > t.cfSize) {
    snprintf(buf, len, "Image needs %u KB of code flash, %s has %u KB", (meta.cfSize + 1023) / 1024, t.part,
             t.cfSize / 1024);
    return buf;
  }
  return nullptr;
}

static void fillIds(const ra::Target &t, Result &r) {
  strlcpy(r.chip, t.part[0] ? t.part : "RA-unknown", sizeof(r.chip));
  strlcpy(r.id, t.uid[0] ? t.uid : "-", sizeof(r.id));
}

void flash(File &img, ProgressFn cb, Result &r) {
  static char msg[96];
  ra::Target t;
  evt("STATE connect");
  r.err = ra::attach(t, cfg.reset);
  if (r.err) r.counted = t.dpidr != 0;  // nothing answered on SWD: no board, not a failed one
  if (!r.err) r.err = checkTarget(t, msg, sizeof(msg));
  if (t.dpidr) {
    fillIds(t, r);
    evt("CHIP %s %s", r.chip, r.id);
  }
  if (!r.err) {
    evt("STATE erase");
    r.err = ra::erase(t, layout(), cfg.erase == storage::ERASE_CHIP, cb);
  }
  if (!r.err) {
    evt("STATE program");
    r.err = ra::program(img, layout(), cb);
  }
  if (!r.err && cfg.verify) {
    evt("STATE verify");
    r.err = ra::verify(img, layout(), cb);
  }
  finishSession(!r.err && cfg.run);
}

bool probe() { return ra::probe(); }

// Attach, run op on success, reply, end the session. ok is the reply text on success.
template <typename Op>
static void maintenance(const char *c, const char *ok, bool run, Op op) {
  ra::Target t;
  const char *err = ra::attach(t, cfg.reset);
  if (!err) err = op(t);
  if (err) replyErr(c, err);
  else reply(c, ok ? String(ok) : String());
  finishSession(run && !err);
}

bool command(const char *c, char *args) {
  String cmd(c);
  if (cmd == "DETECT") {
    ra::Target t;
    const char *err = ra::attach(t, cfg.reset);
    if (!err) {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "dpidr=0x%08X part=%s flash=%u uid=%s aws=0x%04X awe=0x%04X swdelay=%u reset=%d", t.dpidr,
               t.part[0] ? t.part : "UNKNOWN", t.cfSize / 1024, t.uid[0] ? t.uid : "-", t.fawsmr, t.fawemr,
               t.swdDelay, t.viaReset);
      reply(c, buf);
    } else {
      replyErr(c, err);
    }
    finishSession(!err);
  } else if (cmd == "ERASE") {
    // Whole code flash and data flash.
    maintenance(c, nullptr, false, [](const ra::Target &t) -> const char * {
      if (!t.cfSize) return "Unknown part number - cannot size the code flash";
      return ra::erase(t, ra::Layout(), true, nullptr);
    });
  } else if (cmd == "RESET") {
    maintenance(c, nullptr, true, [](const ra::Target &) -> const char * { return nullptr; });
  } else if (cmd == "READ") {
    // READ <addr> [words<=32]: dump target memory (debug aid)
    char *end;
    uint32_t addr = strtoul(args, &end, 0) & ~3u;
    uint32_t n = strtoul(end, nullptr, 0);
    n = n ? min<uint32_t>(n, 32) : 8;
    static uint32_t words[32];
    ra::Target t;
    const char *err = ra::attach(t, cfg.reset);
    if (!err && !swd::readBlock32(addr, words, n)) err = "SWD read failed";
    if (err) {
      replyErr(c, err);
    } else {
      String s;
      char buf[16];
      snprintf(buf, sizeof(buf), "0x%08X:", addr);
      s += buf;
      for (uint32_t i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), " %08X", words[i]);
        s += buf;
      }
      reply(c, s);
    }
    finishSession(false);
  } else {
    return false;
  }
  return true;
}

}  // namespace target
