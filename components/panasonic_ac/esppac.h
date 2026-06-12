#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

namespace esphome {

namespace panasonic_ac {

static const char *const VERSION = "2.6.0";

static const uint8_t BUFFER_SIZE = 128;  // The maximum size of a single packet (both receive and transmit)
static const uint8_t READ_TIMEOUT = 20;  // The maximum time to wait before considering a packet complete

static const uint8_t MIN_TEMPERATURE = 16;     // Minimum temperature as reported by Panasonic app
static const uint8_t MAX_TEMPERATURE = 30;     // Maximum temperature as supported by Panasonic app
static const float TEMPERATURE_STEP = 0.5;     // Steps the temperature can be set in
static const float TEMPERATURE_TOLERANCE = 2;  // The tolerance to allow when checking the climate state
static const uint8_t TEMPERATURE_THRESHOLD =
    100;  // Maximum temperature the AC can report before considering the temperature as invalid

enum class CommandType { Normal, Response, Resend };

enum class ACType {
  DNSKP11,  // New module (via CN-WLAN)
  CZTACG1   // Old module (via CN-CNT)
};

class PanasonicAC : public Component, public uart::UARTDevice, public climate::Climate {
 public:
  void set_outside_temperature_sensor(sensor::Sensor *outside_temperature_sensor);
  void set_outside_temperature_offset(int8_t outside_temperature_offset);
  void set_vertical_swing_select(select::Select *vertical_swing_select);
  void set_horizontal_swing_select(select::Select *horizontal_swing_select);
  void set_nanoex_switch(switch_::Switch *nanoex_switch);
  void set_eco_switch(switch_::Switch *eco_switch);
  void set_econavi_switch(switch_::Switch *econavi_switch);
  void set_mild_dry_switch(switch_::Switch *mild_dry_switch);
  void set_current_power_consumption_sensor(sensor::Sensor *current_power_consumption_sensor);
  void set_error_code_text_sensor(text_sensor::TextSensor *error_code_text_sensor);
  void set_raw_packet_text_sensor(text_sensor::TextSensor *raw_packet_text_sensor);
  void set_raw_packet_2_text_sensor(text_sensor::TextSensor *raw_packet_2_text_sensor);
  void set_defrost_sensor(binary_sensor::BinarySensor *defrost_sensor);
  void set_serial_fault_sensor(binary_sensor::BinarySensor *serial_fault_sensor);

  // Protocol-investigation debug text sensors (raw hex dumps for offline analysis).
  // Telemetry is split across two sensors because HA caps a state string at 255 chars
  // (telemetry packets run ~160 bytes = ~320 hex chars).
  void set_debug_telemetry_1_text_sensor(text_sensor::TextSensor *sensor);
  void set_debug_telemetry_2_text_sensor(text_sensor::TextSensor *sensor);
  void set_debug_report_text_sensor(text_sensor::TextSensor *sensor);
  void set_debug_unknown_text_sensor(text_sensor::TextSensor *sensor);
  void set_probe_value_text_sensor(text_sensor::TextSensor *sensor);

  void set_current_temperature_sensor(sensor::Sensor *current_temperature_sensor);
  void set_current_temperature_offset(int8_t current_temperature_offset);

  void setup() override;
  void loop() override;

 protected:
  sensor::Sensor *outside_temperature_sensor_ = nullptr;        // Sensor to store outside temperature from queries
  select::Select *vertical_swing_select_ = nullptr;             // Select to store manual position of vertical swing
  select::Select *horizontal_swing_select_ = nullptr;           // Select to store manual position of horizontal swing
  switch_::Switch *nanoex_switch_ = nullptr;                    // Switch to toggle nanoeX on/off
  switch_::Switch *eco_switch_ = nullptr;                       // Switch to toggle eco mode on/off
  switch_::Switch *econavi_switch_ = nullptr;                   // Switch to toggle econavi mode on/off
  switch_::Switch *mild_dry_switch_ = nullptr;                  // Switch to toggle mild dry mode on/off
  sensor::Sensor *current_temperature_sensor_ = nullptr;        // Sensor to use for current temperature where AC does not report
  sensor::Sensor *current_power_consumption_sensor_ = nullptr;  // Sensor to store current power consumption from queries
  text_sensor::TextSensor *error_code_text_sensor_ = nullptr;   // Text sensor for the AC error/status code (e.g. "H000" = OK)
  text_sensor::TextSensor *raw_packet_text_sensor_ = nullptr;   // Raw 0x89 poll response bytes 0-126 as hex
  text_sensor::TextSensor *raw_packet_2_text_sensor_ = nullptr; // Raw 0x89 poll response bytes 127+ as hex (overflow sensor)
  binary_sensor::BinarySensor *defrost_sensor_ = nullptr;       // Sensor to store defrost status
  binary_sensor::BinarySensor *serial_fault_sensor_ = nullptr;  // True when no packet received for >60s (UART freeze)
  std::string error_code_state_;                                // Last published error code, to avoid duplicate publishes
  std::string raw_packet_state_;                                // Last published raw_packet (bytes 0-126), dedup
  std::string raw_packet_2_state_;                              // Last published raw_packet_2 (bytes 127+), dedup
  bool serial_fault_state_ = false;
  bool serial_fault_published_ = false;

  // Protocol-investigation debug sensors + dedup state
  text_sensor::TextSensor *debug_telemetry_1_text_sensor_ = nullptr;  // Telemetry packet (0x11 0x03) bytes 0..124
  text_sensor::TextSensor *debug_telemetry_2_text_sensor_ = nullptr;  // Telemetry packet bytes 125..end
  text_sensor::TextSensor *debug_report_text_sensor_ = nullptr;       // Report packet (0x10 0x0A) raw dump
  text_sensor::TextSensor *debug_unknown_text_sensor_ = nullptr;      // Otherwise-dropped/unknown packets (incl. 0x3A header)
  text_sensor::TextSensor *probe_value_text_sensor_ = nullptr;        // Phase B: "KK:VV" hex for probed key's value in 0x89 response
  std::string debug_telemetry_1_state_;
  std::string debug_telemetry_2_state_;
  std::string debug_report_state_;
  std::string debug_unknown_state_;

  std::string vertical_swing_state_;
  std::string horizontal_swing_state_;

  int8_t current_temperature_offset_ = 0;  // current temperature offset to compensate internal sensor values
  int8_t outside_temperature_offset_ = 0;  // outside temperature offset to compensate internal sensor values
  bool nanoex_state_ = false;    // Stores the state of nanoex to prevent duplicate packets
  bool eco_state_ = false;       // Stores the state of eco to prevent duplicate packets
  bool econavi_state_ = false;       // Stores the state of econavi to prevent duplicate packets
  bool mild_dry_state_ = false;  // Stores the state of mild dry to prevent duplicate packets

  bool waiting_for_response_ = false;  // Set to true if we are waiting for a response

  // uint8_t receive_buffer_index = 0;     // Current position of the receive buffer
  // uint8_t receive_buffer[BUFFER_SIZE];  // Stores the packet currently being received

  std::vector<uint8_t> rx_buffer_;

  uint32_t init_time_;             // Stores the current time
  uint32_t last_read_;             // Stores the time at which the last read was done
  uint32_t last_packet_sent_;      // Stores the time at which the last packet was sent
  uint32_t last_packet_received_;  // Stores the time at which the last packet was received

  climate::ClimateTraits traits() override;

  void read_data();

  void update_outside_temperature(int8_t temperature);
  void update_current_temperature(int8_t temperature);
  void update_target_temperature(uint8_t raw_value);
  void update_swing_horizontal(const std::string &swing);
  void update_swing_vertical(const std::string &swing);
  void update_nanoex(bool nanoex);
  void update_eco(bool eco);
  void update_econavi(bool econavi);
  void update_mild_dry(bool mild_dry);
  void update_current_power_consumption(int16_t power);
  void update_error_code(const std::string &code);
  void update_raw_packet(const std::vector<uint8_t> &packet);
  void update_defrost(bool defrost);
  void update_serial_fault(bool fault);

  // Protocol-investigation debug publishers
  static std::string hex_encode(const std::vector<uint8_t> &packet, size_t begin, size_t end);
  void update_debug_telemetry(const std::vector<uint8_t> &packet);
  void update_debug_report(const std::vector<uint8_t> &packet);
  void update_debug_unknown(const std::vector<uint8_t> &packet);
  void update_probe_value(uint8_t key, const uint8_t *data, uint8_t len);

  virtual void on_horizontal_swing_change(const std::string &swing) = 0;
  virtual void on_vertical_swing_change(const std::string &swing) = 0;
  virtual void on_nanoex_change(bool nanoex) = 0;
  virtual void on_eco_change(bool eco) = 0;
  virtual void on_econavi_change(bool econavi) = 0;
  virtual void on_mild_dry_change(bool mild_dry) = 0;

  climate::ClimateAction determine_action();

  void log_packet(std::vector<uint8_t> data, bool outgoing = false);
};

}  // namespace panasonic_ac
}  // namespace esphome
