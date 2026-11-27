// Test process around lib/panel_rx (task 026): the D10 rig feeds it the frames of its model
// bus, with the L8 queue moments it models, one command per line on stdin.
//   POLL <t0> <empty 0|1> <overrun 0|1>         -> P <anchor|-> 
//   READ <tRead> <overrun 0|1> <id hex> <data hex 16> [std]
//                                               -> R <result> <usable 0|1> <updated 0|1>
//   Q <now>                                     -> V <rpm> <m> <ageMs> <fresh> <running> <vehicleDataFresh>
//   C                                           -> counters
// Times are L8 u32 ms.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_rx.h"

using namespace panel_rx;

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  QueueStamp q;
  Consumer c;
  Poll p = q.poll(Snapshot::at(0, false, false));
  char line[256];
  while (fgets(line, sizeof line, stdin)) {
    char cmd[16];
    if (sscanf(line, "%15s", cmd) != 1) continue;
    if (!strcmp(cmd, "POLL")) {
      unsigned long t0;
      int e, o;
      if (sscanf(line, "POLL %lu %d %d", &t0, &e, &o) != 3) return 2;
      p = q.poll(Snapshot::at(static_cast<uint32_t>(t0), e != 0, o != 0));
      if (p.anchor().has) printf("P %u\n", p.anchor().t);
      else printf("P -\n");
    } else if (!strcmp(cmd, "READ")) {
      unsigned long tr, id;
      int o;
      char hex[32], kind[8] = "";
      if (sscanf(line, "READ %lu %d %lx %31s %7s", &tr, &o, &id, hex, kind) < 4) return 2;
      Frame f;
      f.id = static_cast<uint32_t>(id);
      f.ext = strcmp(kind, "std") != 0;
      f.dlc = static_cast<uint8_t>(strlen(hex) / 2);
      for (int i = 0; i < f.dlc && i < 8; i++) {
        unsigned b;
        sscanf(hex + 2 * i, "%2x", &b);
        f.data[i] = static_cast<uint8_t>(b);
      }
      Stamp s = q.read(p, static_cast<uint32_t>(tr), o != 0);
      uint32_t before = c.counters().fields_updated;
      Result r = c.onFrame(f, s);
      printf("R %d %d %d\n", static_cast<int>(r), s.usable ? 1 : 0, c.counters().fields_updated != before ? 1 : 0);
    } else if (!strcmp(cmd, "Q")) {
      unsigned long now;
      if (sscanf(line, "Q %lu", &now) != 1) return 2;
      uint32_t n = static_cast<uint32_t>(now);
      const FieldVal& f = c.field(kRpm);
      printf("V %u %u %d %d %d %d\n", f.value, f.m, c.ageMs(kRpm, n), c.fresh(kRpm, n) ? 1 : 0, c.engineRunning(n) ? 1 : 0,
             c.vehicleDataFresh(n) ? 1 : 0);
    } else if (!strcmp(cmd, "C")) {
      const Counters& k = c.counters();
      printf("C fast=%u slow=%u hb=%u state=%u seq_dropped=%u unusable=%u updated=%u gen_reset=%u\n", k.veh_fast, k.veh_slow,
             k.panel_hb, k.panel_state, k.seq_dropped, k.stamp_unusable, k.fields_updated, k.seq_reset_gen);
    } else {
      return 2;
    }
  }
  return 0;
}
