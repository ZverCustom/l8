// PanelPort over the ESP-IDF TWAI driver (task 036): the only panel mode code that touches
// the hardware. CANBus keeps owning install/start/restart.
#pragma once

#include "can_bus.h"
#include "panel_mode/panel_port.h"

class TwaiPanelPort : public panel_mode::PanelPort {
 public:
  bool read(zmb::CanFrame& f) override;
  bool send(const zmb::CanFrame& f, bool singleShot) override;
  bool rxQueueEmpty() override;
  uint32_t overrunCount() override;
  bool busOff() override;
  void setListenOnly(bool listenOnly) override;
  CANBus* bus = nullptr;  // the owner of the driver (main.cpp)
  bool singleShotDone(panel_core::SingleShot& r) override;

 private:
  bool singleInFlight_ = false;
  uint32_t txFailedSeen_ = 0;
};
