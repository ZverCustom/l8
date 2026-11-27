// Interop peer: the module side of lib/zmb driven over stdin/stdout, so another
// implementation (the Rust core in d7-d10) can exchange real frames with it.
// argv: local peer panelGen(hex) ownBootTag(hex)
// stdin: "T <ms>" time | "F <id> <data16>" frame from the bus | "SEND <tid> <hex>" start a
//        message | "POLL" offer+confirm every frame due now | "TICK" timers. Each command
//        is answered by zero or more lines and a final ".".
// stdout: "F <id> <data16>" frame to the bus | "O <result> <code>" sender outcome |
//         "M <hex>" delivered message | "E <kind> <tid> <code>" receiver event.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "zmb.h"

using namespace zmb;

static std::vector<uint8_t> hexBytes(const char* s) {
  std::vector<uint8_t> o;
  size_t n = strlen(s);
  for (size_t i = 0; i + 1 < n; i += 2) {
    char b[3] = {s[i], s[i + 1], 0};
    o.push_back(uint8_t(strtoul(b, nullptr, 16)));
  }
  return o;
}
static void printFrame(const CanFrame& f) {
  printf("F %08X ", f.id);
  for (int i = 0; i < 8; i++) printf("%02X", f.data[i]);
  printf("\n");
}
static const char* resultName(TxResult r) {
  switch (r) {
    case TxResult::NotDelivered: return "not_delivered";
    case TxResult::Rejected: return "rejected";
    case TxResult::Delivered: return "delivered";
    case TxResult::Unknown: return "unknown";
    default: return "none";
  }
}
static const char* kindName(RxKind k) {
  switch (k) {
    case RxKind::Message: return "message";
    case RxKind::Rejected: return "rejected";
    case RxKind::Aborted: return "aborted";
    case RxKind::TimedOut: return "timed_out";
    case RxKind::Duplicate: return "duplicate";
    default: return "none";
  }
}

static uint8_t rxbuf[kMaxMessage];
static std::vector<uint8_t> txmsg;

int main(int argc, char** argv) {
  if (argc < 5) return 2;
  uint8_t local = uint8_t(strtoul(argv[1], nullptr, 16)), peer = uint8_t(strtoul(argv[2], nullptr, 16));
  RxContext ctx;
  ctx.local = local;
  ctx.side = Side::Module;
  ctx.panelGen = uint32_t(strtoul(argv[3], nullptr, 16));
  ctx.ownBootTag = uint32_t(strtoul(argv[4], nullptr, 16));
  Sender tx(local, peer);
  Receiver rx(peer, rxbuf, sizeof rxbuf);
  uint32_t now = 0;
  static char line[60000];
  while (fgets(line, sizeof line, stdin)) {
    char* cmd = strtok(line, " \r\n");
    if (!cmd) continue;
    if (!strcmp(cmd, "T")) {
      now = uint32_t(strtoul(strtok(nullptr, " \r\n"), nullptr, 10));
    } else if (!strcmp(cmd, "F")) {
      CanFrame f;
      f.id = uint32_t(strtoul(strtok(nullptr, " \r\n"), nullptr, 16));
      auto d = hexBytes(strtok(nullptr, " \r\n"));
      memcpy(f.data, d.data(), d.size() < 8 ? d.size() : 8);
      TxOutcome o = tx.onFrame(f, now);
      if (o.result != TxResult::None) printf("O %s %u\n", resultName(o.result), o.code);
      RxOutput r = rx.onFrame(f, ctx, now);
      if (r.hasReply) {
        printFrame(r.reply);  // written to the bus: the ACK before the message is handed out
        if (r.flow) rx.confirmFlow(r.flow, now);  // a CTS grants credit only now that it is transmitted
      }
      if (r.event.kind == RxKind::Message) {
        printf("M ");
        for (size_t i = 0; i < r.event.len; i++) printf("%02X", rx.message()[i]);
        printf("\n");
      } else if (r.event.kind != RxKind::None) {
        printf("E %s %u %u\n", kindName(r.event.kind), r.event.tid, r.event.code);
      }
    } else if (!strcmp(cmd, "SEND")) {
      uint16_t tid = uint16_t(strtoul(strtok(nullptr, " \r\n"), nullptr, 16));
      txmsg = hexBytes(strtok(nullptr, " \r\n"));
      if (!tx.start(txmsg.data(), txmsg.size(), tid)) printf("O refused 0\n");
    } else if (!strcmp(cmd, "POLL")) {
      CanFrame f;
      while (tx.poll(now, f)) {
        tx.confirm(now);
        printFrame(f);
      }
    } else if (!strcmp(cmd, "TICK")) {
      TxOutcome o = tx.tick(now);
      if (o.result != TxResult::None) printf("O %s %u\n", resultName(o.result), o.code);
      RxEvent e = rx.tick(now);
      if (e.kind != RxKind::None) printf("E %s %u %u\n", kindName(e.kind), e.tid, e.code);
    }
    printf(".\n");
    fflush(stdout);
  }
  return 0;
}
