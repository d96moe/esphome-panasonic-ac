#include "esphome/components/climate/climate.h"
#include "esphome/components/climate/climate_mode.h"
#include "esppac.h"

namespace esphome {
namespace panasonic_ac {
namespace CNT {

static const uint8_t CTRL_HEADER = 0xF0;  // The header for control frames
static const uint8_t POLL_HEADER = 0x70;  // The header for the poll command

static const int POLL_INTERVAL = 5000;  // The interval at which to poll the AC
static const int CMD_INTERVAL = 250;  // The interval at which to send commands

enum class ACState {
  Initializing,  // Before first query response is receive
  Ready,         // All done, ready to receive regular packets
};

class PanasonicACCNT : public PanasonicAC {
 public:
  void control(const climate::ClimateCall &call) override;

  void on_horizontal_swing_change(const StringRef &swing) override;
  void on_vertical_swing_change(const StringRef &swing) override;
  void on_nanoex_change(bool nanoex) override;
  void on_eco_change(bool eco) override;
  void on_econavi_change(bool eco) override;
  void on_mild_dry_change(bool mild_dry) override;

  void setup() override;
  void loop() override;

  void set_anomaly_sensor(text_sensor::TextSensor *anomaly_sensor) { this->anomaly_sensor_ = anomaly_sensor; }
  void set_humidity_sensor(sensor::Sensor *humidity_sensor) { this->humidity_sensor_ = humidity_sensor; }
  void set_coil_temperature_sensor(sensor::Sensor *coil_temperature_sensor) {
    this->coil_temperature_sensor_ = coil_temperature_sensor;
  }

 protected:
  // Byte 20: relative humidity, 0-100, 0x80 = unavailable.
  sensor::Sensor *humidity_sensor_ = nullptr;
  // Byte 21: unclear - DomiStyle's own protocol notes label it a duplicate of
  // byte 18 (current/room temperature), but a live divergence from byte 18
  // has been observed on real hardware, which a plain duplicate shouldn't
  // show. Named "coil" per the alternate theory (indoor coil/piping temp,
  // https://github.com/ssjoholm/panasonic-cn-cnt) until confirmed either way.
  sensor::Sensor *coil_temperature_sensor_ = nullptr;

  // Bytes documented as static/reserved/unknown in the CN-CNT protocol
  // (https://github.com/ssjoholm/panasonic-cn-cnt) - byte 8 "Reserved (always
  // 0x00)", byte 9 "unknown flag, static", bytes 16-17 "model-specific" but
  // constant per unit, bytes 31-33 "multiplexed status/identifiers" of
  // unclear meaning. None of these are known to ever change during normal
  // operation, which makes them plausible places for a fault/error flag we
  // haven't identified yet - so instead of decoding them, we just remember
  // whatever value each one first shows up as and flag it the moment any of
  // them (or an unrecognized byte 12/14 value) deviates from that baseline.
  text_sensor::TextSensor *anomaly_sensor_ = nullptr;
  bool anomaly_baseline_set_ = false;
  uint8_t baseline_byte8_ = 0, baseline_byte9_ = 0, baseline_byte16_ = 0, baseline_byte17_ = 0;
  uint8_t baseline_byte31_ = 0, baseline_byte32_ = 0, baseline_byte33_ = 0;

  void check_for_anomaly();

  ACState state_ = ACState::Initializing;  // Stores the internal state of the AC, used during initialization

  // uint8_t data[10];
  std::vector<uint8_t> data = std::vector<uint8_t>(10);  // Stores the data received from the AC
  std::vector<uint8_t> cmd;  // Used to build next command

  void handle_poll();
  void handle_cmd();

  void set_data(bool set);

  void send_command(std::vector<uint8_t> command, CommandType type, uint8_t header);
  void send_packet(const std::vector<uint8_t> &command, CommandType type);

  bool verify_packet();
  void handle_packet();

  climate::ClimateMode determine_mode(uint8_t mode);
  climate::ClimateFanMode determine_fan_speed(uint8_t speed);
  uint8_t mode_to_cmd_byte_(climate::ClimateMode mode);

  std::string determine_vertical_swing(uint8_t swing);
  std::string determine_horizontal_swing(uint8_t swing);

  climate::ClimatePreset determine_preset(uint8_t preset);
  bool determine_preset_nanoex(uint8_t preset);
  bool determine_eco(uint8_t value);
  bool determine_econavi(uint8_t value);
  bool determine_mild_dry(uint8_t value);
  uint16_t determine_power_consumption(uint8_t byte_28, uint8_t multiplier, uint8_t offset);
};

}  // namespace CNT
}  // namespace panasonic_ac
}  // namespace esphome
