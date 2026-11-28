#include "panel_port_twai.h"

#include <string.h>

#include "driver/twai.h"

bool TwaiPanelPort::read(zmb::CanFrame& f) {
  twai_message_t m;
  if (twai_receive(&m, 0) != ESP_OK) return false;
  f.id = m.identifier;
  f.extended = m.extd;
  f.rtr = m.rtr;
  f.dlc = m.data_length_code;
  memset(f.data, 0, sizeof f.data);
  memcpy(f.data, m.data, m.data_length_code > 8 ? 8 : m.data_length_code);
  return true;
}

bool TwaiPanelPort::send(const zmb::CanFrame& f, bool singleShot) {
  twai_message_t m = {};
  m.identifier = f.id;
  m.extd = f.extended;
  m.rtr = f.rtr;
  m.ss = singleShot;
  m.data_length_code = f.dlc > 8 ? 8 : f.dlc;
  memcpy(m.data, f.data, m.data_length_code);
  if (twai_transmit(&m, 0) != ESP_OK) return false;
  if (singleShot) {
    singleInFlight_ = true;
    twai_status_info_t st;
    txFailedSeen_ = twai_get_status_info(&st) == ESP_OK ? st.tx_failed_count : 0;
  }
  return true;
}

bool TwaiPanelPort::rxQueueEmpty() {
  twai_status_info_t st;
  return twai_get_status_info(&st) == ESP_OK && st.msgs_to_rx == 0;
}

uint32_t TwaiPanelPort::overrunCount() {
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return 0;
  return st.rx_overrun_count + st.rx_missed_count;
}

bool TwaiPanelPort::busOff() {
  twai_status_info_t st;
  return twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_BUS_OFF;
}

void TwaiPanelPort::recover() {
  twai_initiate_recovery();  // the driver goes to STOPPED when recovered; started again below
  singleInFlight_ = false;
}

bool TwaiPanelPort::singleShotDone(panel_core::SingleShot& r) {
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return false;
  if (st.state == TWAI_STATE_STOPPED) twai_start();  // after a recovery
  if (!singleInFlight_) return false;
  if (st.tx_failed_count != txFailedSeen_) {
    singleInFlight_ = false;
    r = panel_core::SingleShot::Lost;
    return true;
  }
  if (st.msgs_to_tx == 0) {
    singleInFlight_ = false;
    r = panel_core::SingleShot::Sent;
    return true;
  }
  return false;
}
