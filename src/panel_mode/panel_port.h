// The hardware boundary of the L8 panel mode (task 036): TWAI and NVS behind thin
// interfaces. The firmware implements them over ESP-IDF; the host over models.
#pragma once

#include <stdint.h>

#include "panel_core.h"

namespace panel_mode {

class PanelPort {
 public:
  virtual ~PanelPort() = default;
  // One received frame, non-blocking; false when the receive queue is empty.
  virtual bool read(zmb::CanFrame& f) = 0;
  // Queue a frame for transmission (singleShot: no retransmission). False: not queued.
  virtual bool send(const zmb::CanFrame& f, bool singleShot) = 0;
  // The driver's software receive queue is empty now (§14.3, msgs_to_rx == 0).
  virtual bool rxQueueEmpty() = 0;
  // RX overrun / missed counters added up (§14.3: growth voids the anchor).
  virtual uint32_t overrunCount() = 0;
  virtual bool busOff() = 0;
  // Listen-only (receive, never transmit — no ACK, no error flags) or normal; the driver
  // restarts in that mode at the panel bitrate, which also leaves bus-off (A5, R92).
  virtual void setListenOnly(bool listenOnly) = 0;
  // The outcome of the last single-shot frame, once known.
  virtual bool singleShotDone(panel_core::SingleShot& r) = 0;
};

class PanelStore {
 public:
  virtual ~PanelStore() = default;
  virtual bool load(uint32_t& panelId, uint8_t& addr) = 0;
  virtual void save(uint32_t panelId, uint8_t addr) = 0;
};

}  // namespace panel_mode
