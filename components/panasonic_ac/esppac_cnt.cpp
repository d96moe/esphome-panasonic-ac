#include "esppac_cnt.h"
#include "esppac_commands_cnt.h"

#include "esphome/core/log.h"
#include <algorithm>

namespace esphome {
namespace panasonic_ac {
namespace CNT {

static const char *const TAG = "panasonic_ac.cz_tacg1";

void PanasonicACCNT::setup() {
  PanasonicAC::setup();

  ESP_LOGD(TAG, "Using CZ-TACG1 protocol via CN-CNT");
}

void PanasonicACCNT::loop() {
  PanasonicAC::read_data();

  if (millis() - this->last_read_ > READ_TIMEOUT &&
      !this->rx_buffer_.empty())  // Check if our read timed out and we received something
  {
    log_packet(this->rx_buffer_);

    if (!verify_packet())  // Verify length, header, counter and checksum
      return;

    this->waiting_for_response_ = false;
    this->last_packet_received_ = millis();  // Set the time at which we received our last packet

    handle_packet();

    this->rx_buffer_.clear();  // Reset buffer
  }
  handle_cmd();
  handle_poll();  // Handle sending poll packets
}

/*
 * ESPHome control request
 */

void PanasonicACCNT::control(const climate::ClimateCall &call) {
  if (this->state_ != ACState::Ready)
    return;

  if (this->cmd.empty()) {
    ESP_LOGV(TAG, "Copying data to cmd");
    this->cmd = this->data;
  }

  if (call.get_mode().has_value()) {
    ESP_LOGV(TAG, "Requested mode change");
    if (this->heat_8_15_mode_) {
      this->heat_8_15_mode_ = false;
      this->clear_custom_preset_();
      this->cmd[1] = (uint8_t)(this->get_heat_8_15_exit_temperature_() / TEMPERATURE_STEP);
    }

    // Mode is set explicitly below from the requested value, so no need to restore
    // the saved pre-heat_8_15 mode here.
    switch (*call.get_mode()) {
      case climate::CLIMATE_MODE_COOL:
        this->cmd[0] = 0x34;
        break;
      case climate::CLIMATE_MODE_HEAT:
        this->cmd[0] = 0x44;
        break;
      case climate::CLIMATE_MODE_DRY:
        this->cmd[0] = 0x24;
        break;
      case climate::CLIMATE_MODE_HEAT_COOL:
        this->cmd[0] = 0x04;
        break;
      case climate::CLIMATE_MODE_FAN_ONLY:
        this->cmd[0] = 0x64;
        break;
      case climate::CLIMATE_MODE_OFF:
        this->cmd[0] = this->cmd[0] & 0xF0;  // Strip right nib to turn AC off
        break;
      default:
        ESP_LOGV(TAG, "Unsupported mode requested");
        break;
    }
  }

  if (call.get_target_temperature().has_value()) {
    float temp = *call.get_target_temperature();
    if (this->heat_8_15_mode_) {
      temp = std::max((float) MIN_TEMPERATURE_HEAT_8_15, std::min((float) MAX_TEMPERATURE_HEAT_8_15, temp));
    }
    this->cmd[1] = (uint8_t)(temp / TEMPERATURE_STEP);
  }

  if (call.get_fan_mode().has_value()) {
    if (this->heat_8_15_mode_) {
      ESP_LOGW(TAG, "Fan mode change rejected: heat_8_15 preset locks fan to max");
    } else {
      ESP_LOGV(TAG, "Requested fan mode change");

      if (this->preset != climate::CLIMATE_PRESET_COMFORT) {
        ESP_LOGV(TAG, "Resetting preset");
        this->cmd[5] = (this->cmd[5] & 0xF0);  // Clear right nib for normal mode
      }

      switch (*call.get_fan_mode()) {
        case climate::CLIMATE_FAN_LOW:
          this->cmd[3] = 0x30;
          break;
        case climate::CLIMATE_FAN_MEDIUM:
          this->cmd[3] = 0x50;
          break;
        case climate::CLIMATE_FAN_HIGH:
          this->cmd[3] = 0x70;
          break;
        case climate::CLIMATE_FAN_AUTO:
          this->cmd[3] = 0xA0;
          break;
        default:
          ESP_LOGV(TAG, "Unsupported mode requested");
          break;
      }
    }
  }

  if (call.get_swing_mode().has_value()) {
    ESP_LOGV(TAG, "Requested swing mode change");

    switch (*call.get_swing_mode()) {
      case climate::CLIMATE_SWING_BOTH:
        this->cmd[4] = 0xFD;
        break;
      case climate::CLIMATE_SWING_OFF:
        this->cmd[4] = 0x36;  // Reset both to center
        break;
      case climate::CLIMATE_SWING_VERTICAL:
        this->cmd[4] = 0xF6;  // Swing vertical, horizontal center
        break;
      case climate::CLIMATE_SWING_HORIZONTAL:
        this->cmd[4] = 0x3D;  // Swing horizontal, vertical center
        break;
      default:
        ESP_LOGV(TAG, "Unsupported swing mode requested");
        break;
    }
  }

  if (call.get_preset().has_value()) {
    ESP_LOGV(TAG, "Requested preset change");
    if (this->heat_8_15_mode_) {
      this->heat_8_15_mode_ = false;
      this->clear_custom_preset_();
      this->cmd[1] = (uint8_t)(this->get_heat_8_15_exit_temperature_() / TEMPERATURE_STEP);
      this->cmd[0] = this->mode_to_cmd_byte_(this->get_heat_8_15_exit_mode_());  // Restore mode active before heat_8_15
      this->publish_state();  // Immediately restore normal temp range in UI
    }

    switch (*call.get_preset()) {
      case climate::CLIMATE_PRESET_COMFORT:
        this->cmd[5] = (this->cmd[5] & 0xF0);  // Clear right nib for normal mode
        break;
      case climate::CLIMATE_PRESET_BOOST:
        this->cmd[5] = (this->cmd[5] & 0xF0) + 0x02;  // Clear right nib and set powerful mode
        break;
      case climate::CLIMATE_PRESET_ECO:
        this->cmd[5] = (this->cmd[5] & 0xF0) + 0x04;  // Clear right nib and set quiet mode
        break;
      default:
        ESP_LOGV(TAG, "Unsupported preset requested");
        break;
    }
  }

  if (this->heat_8_15_preset_enabled_ && call.has_custom_preset() && call.get_custom_preset() == PRESET_HEAT_8_15) {
    ESP_LOGD(TAG, "Setting heat_8_15 preset: HEAT + max fan + temp clamped to 8-15 C");
    this->cmd[0] = 0x44;                     // HEAT mode + ON
    this->cmd[3] = 0x70;                     // Max fan (level 5)
    this->cmd[5] = (this->cmd[5] & 0xF0);   // Clear preset bits (powerful/eco)
    float clamped_temp = std::max((float) MIN_TEMPERATURE_HEAT_8_15,
                                  std::min((float) MAX_TEMPERATURE_HEAT_8_15, this->target_temperature));
    this->cmd[1] = (uint8_t)(clamped_temp / TEMPERATURE_STEP);
    this->save_pre_heat_8_15_temperature_();
    this->save_pre_heat_8_15_mode_();
    this->heat_8_15_mode_ = true;
    this->set_custom_preset_(PRESET_HEAT_8_15);
    this->preset = {};
    this->publish_state();  // Immediately show 8-15 C range in UI
  }
}

/*
 * Set the data array to the fields
 */
void PanasonicACCNT::set_data(bool set) {
  // Capture the last known good mode while NOT in heat_8_15 mode, before this->mode is
  // overwritten below - covers the case where heat_8_15 is entered by some other
  // controller (app/remote) rather than through our own control().
  this->save_pre_heat_8_15_mode_();

  this->mode = determine_mode(this->data[0]);
  this->fan_mode = determine_fan_speed(this->data[3]);

  StringRef verticalSwing(determine_vertical_swing(this->data[4]));
  StringRef horizontalSwing(determine_horizontal_swing(this->data[4]));

  climate::ClimatePreset preset = determine_preset(this->data[5]);

  // Detect heat_8_15 (winter/summer house) mode from AC state:
  // HEAT mode + max fan (0x70) + target temp below normal minimum (raw < 32 = temp < 16 C)
  bool new_heat_8_15 = this->heat_8_15_preset_enabled_ &&
                       (this->mode == climate::CLIMATE_MODE_HEAT) &&
                       (this->data[3] == 0x70) &&
                       (this->data[1] < (uint8_t)(MIN_TEMPERATURE / TEMPERATURE_STEP));

  bool nanoex = determine_preset_nanoex(this->data[5]);
  bool eco = determine_eco(this->data[8]);
  bool econavi = determine_econavi(this->data[5]);
  bool mildDry = determine_mild_dry(this->data[2]);

  // Capture the last known good setpoint while NOT in heat_8_15 mode, before it's
  // overwritten below - covers the case where heat_8_15 is entered by some other
  // controller (app/remote) rather than through our own control().
  this->save_pre_heat_8_15_temperature_();

  this->update_target_temperature((int8_t) this->data[1]);

  if (set) {
    // Also set current and outside temperature
    // 128 means not supported
    if (this->current_temperature_sensor_ == nullptr) {
      if(this->rx_buffer_[18] != 0x80)
        this->update_current_temperature((int8_t)this->rx_buffer_[18]);
      else if(this->rx_buffer_[21] != 0x80)
        this->update_current_temperature((int8_t)this->rx_buffer_[21]);
      else {
        ESP_LOGV(TAG, "Current temperature is not supported");
      }
    }

    if (this->outside_temperature_sensor_ != nullptr)
    {
      if(this->rx_buffer_[19] != 0x80)
        this->update_outside_temperature((int8_t)this->rx_buffer_[19]);
      else if(this->rx_buffer_[22] != 0x80)
        this->update_outside_temperature((int8_t)this->rx_buffer_[22]);
      else {
        ESP_LOGV(TAG, "Outside temperature is not supported");
      }
    }

    if(this->current_power_consumption_sensor_ != nullptr) {
      uint16_t power_consumption = determine_power_consumption((int8_t)this->rx_buffer_[28], (int8_t)this->rx_buffer_[29], (int8_t)this->rx_buffer_[30]);
      this->update_current_power_consumption(power_consumption);
    }

    bool is_defrosting = this->rx_buffer_.size() >= 15 && this->rx_buffer_[14] == 0x02;

    if (this->defrost_sensor_ != nullptr) {
      if (this->rx_buffer_.size() >= 15) {
        update_defrost(is_defrosting);
      } else {
        ESP_LOGV(TAG, "Defrost status is not supported");
      }
    }

    // Determine the real hvac_action from the unit's own operational state byte,
    // instead of the generic temperature-delta heuristic (determine_action()) that
    // the WLAN protocol falls back to. Only 0x40 (heat idle) and 0x4C (heat running)
    // are empirically confirmed on real hardware; the 0x44/0x48 heat sub-states and
    // the whole 0x30-0x3C cool range are inferred by symmetry, not yet observed live.
    // Any non-idle value within a mode's range is treated as actively running.
    // Defrost takes priority over the state-byte mapping: the unit reports itself
    // as heating (0x4x) while defrosting, but ESPHome/HA has a dedicated action for
    // this, independent of whether defrost_sensor is configured.
    if (this->rx_buffer_.size() >= 13) {
      uint8_t state_byte = this->rx_buffer_[12];
      if (is_defrosting) {
        this->action = climate::CLIMATE_ACTION_DEFROSTING;
      } else if (state_byte == 0x00) {
        this->action = climate::CLIMATE_ACTION_OFF;
      } else if ((state_byte & 0xF0) == 0x40) {
        this->action = (state_byte == 0x40) ? climate::CLIMATE_ACTION_IDLE : climate::CLIMATE_ACTION_HEATING;
      } else if ((state_byte & 0xF0) == 0x30) {
        this->action = (state_byte == 0x30) ? climate::CLIMATE_ACTION_IDLE : climate::CLIMATE_ACTION_COOLING;
      } else {
        this->action = climate::CLIMATE_ACTION_IDLE;  // 0x04/0x08 transient, or unrecognized value
      }
    }
  }

  if (verticalSwing == "auto" && horizontalSwing == "auto")
    this->swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (verticalSwing == "auto")
    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (horizontalSwing == "auto")
    this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else
    this->swing_mode = climate::CLIMATE_SWING_OFF;

  this->update_swing_vertical(verticalSwing);
  this->update_swing_horizontal(horizontalSwing);

  if (new_heat_8_15) {
    this->heat_8_15_mode_ = true;
    this->set_custom_preset_(PRESET_HEAT_8_15);
    this->preset = {};
  } else {
    if (this->heat_8_15_mode_) {
      this->heat_8_15_mode_ = false;
      this->clear_custom_preset_();
    }
    this->preset = preset;
  }

  this->update_nanoex(nanoex);
  this->update_eco(eco);
  this->update_econavi(econavi);
  this->update_mild_dry(mildDry);
}

/*
 * Send a command, attaching header, packet length and checksum
 */
void PanasonicACCNT::send_command(std::vector<uint8_t> command, CommandType type, uint8_t header = CNT::CTRL_HEADER) {
  uint8_t length = command.size();
  command.insert(command.begin(), header);
  command.insert(command.begin() + 1, length);

  uint8_t checksum = 0;

  for (uint8_t i : command)
    checksum -= i;  // Add to checksum

  command.push_back(checksum);

  send_packet(command, type);  // Actually send the constructed packet
}

/*
 * Send a raw packet, as is
 */
void PanasonicACCNT::send_packet(const std::vector<uint8_t> &packet, CommandType type) {
  this->last_packet_sent_ = millis();  // Save the time when we sent the last packet

  if (type != CommandType::Response)     // Don't wait for a response for responses
    this->waiting_for_response_ = true;  // Mark that we are waiting for a response

  write_array(packet);       // Write to UART
  log_packet(packet, true);  // Write to log
}

/*
 * Loop handling
 */

void PanasonicACCNT::handle_poll() {
  if (millis() - this->last_packet_sent_ > POLL_INTERVAL) {
    ESP_LOGV(TAG, "Polling AC");
    send_command(CMD_POLL, CommandType::Normal, POLL_HEADER);
  }
}

void PanasonicACCNT::handle_cmd() {
  if (!this->cmd.empty() && millis() - this->last_packet_sent_ > CMD_INTERVAL) {
    ESP_LOGV(TAG, "Sending Command");
    send_command(this->cmd, CommandType::Normal, CTRL_HEADER);
    this->cmd.clear();
  }
}

/*
 * Packet handling
 */

bool PanasonicACCNT::verify_packet() {
  if (this->rx_buffer_.size() < 12) {
    ESP_LOGW(TAG, "Dropping invalid packet (length)");

    this->rx_buffer_.clear();  // Reset buffer
    return false;
  }

  // Check if header matches
  if (this->rx_buffer_[0] != CTRL_HEADER && this->rx_buffer_[0] != POLL_HEADER) {
    ESP_LOGW(TAG, "Dropping invalid packet (header)");

    this->rx_buffer_.clear();  // Reset buffer
    return false;
  }

  // Packet length minus header, packet length and checksum
  if (this->rx_buffer_[1] != this->rx_buffer_.size() - 3) {
    ESP_LOGD(TAG, "Dropping invalid packet (length mismatch)");

    this->rx_buffer_.clear();  // Reset buffer
    return false;
  }

  uint8_t checksum = 0;

  for (uint8_t b : this->rx_buffer_) {
    checksum += b;
  }

  if (checksum != 0) {
    ESP_LOGD(TAG, "Dropping invalid packet (checksum)");

    this->rx_buffer_.clear();  // Reset buffer
    return false;
  }

  return true;
}

void PanasonicACCNT::handle_packet() {
  if (this->rx_buffer_[0] == POLL_HEADER) {
    this->data = std::vector<uint8_t>(this->rx_buffer_.begin() + 2, this->rx_buffer_.begin() + 12);

    this->set_data(true);
    this->publish_state();

    if (this->state_ != ACState::Ready)
      this->state_ = ACState::Ready;  // Mark as ready after first poll
  } else {
    ESP_LOGD(TAG, "Received unknown packet");
  }
}

uint8_t PanasonicACCNT::mode_to_cmd_byte_(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_COOL:
      return 0x34;
    case climate::CLIMATE_MODE_HEAT:
      return 0x44;
    case climate::CLIMATE_MODE_DRY:
      return 0x24;
    case climate::CLIMATE_MODE_FAN_ONLY:
      return 0x64;
    case climate::CLIMATE_MODE_HEAT_COOL:
    default:
      return 0x04;
  }
}

climate::ClimateMode PanasonicACCNT::determine_mode(uint8_t mode) {
  uint8_t nib1 = (mode >> 4) & 0x0F;  // Left nib for mode
  uint8_t nib2 = (mode >> 0) & 0x0F;  // Right nib for power state

  if (nib2 == 0x00)
    return climate::CLIMATE_MODE_OFF;

  switch (nib1) {
    case 0x00:  // Auto
      return climate::CLIMATE_MODE_HEAT_COOL;
    case 0x03:  // Cool
      return climate::CLIMATE_MODE_COOL;
    case 0x04:  // Heat
      return climate::CLIMATE_MODE_HEAT;
    case 0x02:  // Dry
      return climate::CLIMATE_MODE_DRY;
    case 0x06:  // Fan only
      return climate::CLIMATE_MODE_FAN_ONLY;
    default:
      ESP_LOGW(TAG, "Received unknown climate mode");
      return climate::CLIMATE_MODE_OFF;
  }
}

climate::ClimateFanMode PanasonicACCNT::determine_fan_speed(uint8_t speed) {
  switch (speed) {
    case 0xA0:  // Auto
      return climate::CLIMATE_FAN_AUTO;
    case 0x30:  // 1
      return climate::CLIMATE_FAN_LOW;
    case 0x40:  // 2
      return climate::CLIMATE_FAN_LOW;
    case 0x50:  // 3
      return climate::CLIMATE_FAN_MEDIUM;
    case 0x60:  // 4
      return climate::CLIMATE_FAN_HIGH;
    case 0x70:  // 5
      return climate::CLIMATE_FAN_HIGH;
    default:
      ESP_LOGW(TAG, "Received unknown fan speed");
      return climate::CLIMATE_FAN_AUTO;
  }
}

std::string PanasonicACCNT::determine_vertical_swing(uint8_t swing) {
  uint8_t nib = (swing >> 4) & 0x0F;  // Left nib for vertical swing

  switch (nib) {
    case 0x0E:
      return "swing";
    case 0x0F:
      return "auto";
    case 0x01:
      return "up";
    case 0x02:
      return "up_center";
    case 0x03:
      return "center";
    case 0x04:
      return "down_center";
    case 0x05:
      return "down";
    case 0x00:
      return "unsupported";
    default:
      ESP_LOGW(TAG, "Received unknown vertical swing mode: 0x%02X", nib);
      return "Unknown";
  }
}

std::string PanasonicACCNT::determine_horizontal_swing(uint8_t swing) {
  uint8_t nib = (swing >> 0) & 0x0F;  // Right nib for horizontal swing

  switch (nib) {
    case 0x0D:
      return "auto";
    case 0x09:
      return "left";
    case 0x0A:
      return "left_center";
    case 0x06:
      return "center";
    case 0x0B:
      return "right_center";
    case 0x0C:
      return "right";
    case 0x00:
      return "unsupported";
    default:
      ESP_LOGW(TAG, "Received unknown horizontal swing mode");
      return "Unknown";
  }
}

climate::ClimatePreset PanasonicACCNT::determine_preset(uint8_t preset) {
  uint8_t nib = (preset >> 0) & 0x0F;  // Right nib for preset (powerful/quiet)

  switch (nib) {
    case 0x02:
      return climate::CLIMATE_PRESET_BOOST;
    case 0x04:
      return climate::CLIMATE_PRESET_ECO;
    case 0x00:
      return climate::CLIMATE_PRESET_COMFORT;
    default:
      ESP_LOGW(TAG, "Received unknown preset");
      return climate::CLIMATE_PRESET_COMFORT;
  }
}

bool PanasonicACCNT::determine_preset_nanoex(uint8_t preset) {
  uint8_t nib = (preset >> 4) & 0x04;  // Left nib for nanoex

  if (nib == 0x04)
    return true;
  else if (nib == 0x00)
    return false;
  else {
    ESP_LOGW(TAG, "Received unknown nanoex value");
    return false;
  }
}

bool PanasonicACCNT::determine_eco(uint8_t value) {
  if (value == 0x40)
    return true;
  else if (value == 0x00)
    return false;
  else {
    ESP_LOGW(TAG, "Received unknown eco value");
    return false;
  }
}

bool PanasonicACCNT::determine_econavi(uint8_t value) {
  uint8_t nib = value & 0x10;
  
  if (nib == 0x10)
    return true;
  else if (nib == 0x00)
    return false;
  else {
    ESP_LOGW(TAG, "Received unknown econavi value");
    return false;
  }
}

bool PanasonicACCNT::determine_mild_dry(uint8_t value) {
  if (value == 0x7F)
    return true;
  else if (value == 0x80)
    return false;
  else {
    ESP_LOGW(TAG, "Received unknown mild dry value");
    return false;
  }
}

uint16_t PanasonicACCNT::determine_power_consumption(uint8_t byte_28, uint8_t byte_29, uint8_t offset) {
  return (uint16_t)(byte_28 + (byte_29 * 256)) - offset;
}

/*
 * Sensor handling
 */

void PanasonicACCNT::on_vertical_swing_change(const StringRef &swing) {
  if (this->state_ != ACState::Ready)
    return;

  ESP_LOGD(TAG, "Setting vertical swing position");

  if (this->cmd.empty()) {
    ESP_LOGV(TAG, "Copying data to cmd");
    this->cmd = this->data;
  }

  if (swing == "down")
    this->cmd[4] = (this->cmd[4] & 0x0F) + 0x50;
  else if (swing == "down_center")
    this->cmd[4] = (this->cmd[4] & 0x0F) + 0x40;
  else if (swing == "center")
    this->cmd[4] = (this->cmd[4] & 0x0F) + 0x30;
  else if (swing == "up_center")
    this->cmd[4] = (this->cmd[4] & 0x0F) + 0x20;
  else if (swing == "up")
    this->cmd[4] = (this->cmd[4] & 0x0F) + 0x10;
  else if (swing == "swing")
    this->cmd[4] = (this->cmd[4] & 0x0F) + 0xE0;
  else if (swing == "auto")
    this->cmd[4] = (this->cmd[4] & 0x0F) + 0xF0;
  else {
    ESP_LOGW(TAG, "Unsupported vertical swing position received");
    return;
  }

}

void PanasonicACCNT::on_horizontal_swing_change(const StringRef &swing) {
  if (this->state_ != ACState::Ready)
    return;

  ESP_LOGD(TAG, "Setting horizontal swing position");

  if (this->cmd.empty()) {
    ESP_LOGV(TAG, "Copying data to cmd");
    this->cmd = this->data;
  }

  if (swing == "left")
    this->cmd[4] = (this->cmd[4] & 0xF0) + 0x09;
  else if (swing == "left_center")
    this->cmd[4] = (this->cmd[4] & 0xF0) + 0x0A;
  else if (swing == "center")
    this->cmd[4] = (this->cmd[4] & 0xF0) + 0x06;
  else if (swing == "right_center")
    this->cmd[4] = (this->cmd[4] & 0xF0) + 0x0B;
  else if (swing == "right")
    this->cmd[4] = (this->cmd[4] & 0xF0) + 0x0C;
  else if (swing == "auto")
    this->cmd[4] = (this->cmd[4] & 0xF0) + 0x0D;
  else {
    ESP_LOGW(TAG, "Unsupported horizontal swing position received");
    return;
  }

}

void PanasonicACCNT::on_nanoex_change(bool state) {
  if (this->state_ != ACState::Ready)
    return;

  if (this->cmd.empty()) {
    ESP_LOGV(TAG, "Copying data to cmd");
    this->cmd = this->data;
  }

  this->nanoex_state_ = state;

  if (state) {
    ESP_LOGV(TAG, "Turning nanoex on");
    this->cmd[5] = (this->cmd[5] & 0x0F) + 0x40;
  } else {
    ESP_LOGV(TAG, "Turning nanoex off");
    this->cmd[5] = (this->cmd[5] & 0x0F);
  }
}

void PanasonicACCNT::on_eco_change(bool state) {
  if (this->state_ != ACState::Ready)
    return;

  if (this->cmd.empty()) {
    ESP_LOGV(TAG, "Copying data to cmd");
    this->cmd = this->data;
  }

  this->eco_state_ = state;

  if (state) {
    ESP_LOGV(TAG, "Turning eco mode on");
    this->cmd[8] = 0x40;
  } else {
    ESP_LOGV(TAG, "Turning eco mode off");
    this->cmd[8] = 0x00;
  }
}

void PanasonicACCNT::on_econavi_change(bool state) {
  if (this->state_ != ACState::Ready)
    return;

  if (this->cmd.empty()) {
    ESP_LOGV(TAG, "Copying data to cmd");
    this->cmd = this->data;
  }

  this->econavi_state_ = state;

  if (state) {
    ESP_LOGV(TAG, "Turning econavi mode on");
    this->cmd[5] = 0x10;
  } else {
    ESP_LOGV(TAG, "Turning econavi mode off");
    this->cmd[5] = 0x00;
  }

}

void PanasonicACCNT::on_mild_dry_change(bool state) {
  if (this->state_ != ACState::Ready)
    return;

  if (this->cmd.empty()) {
    ESP_LOGV(TAG, "Copying data to cmd");
    this->cmd = this->data;
  }

  this->mild_dry_state_ = state;

  if (state) {
    ESP_LOGV(TAG, "Turning mild dry on");
    this->cmd[2] = 0x7F;
  } else {
    ESP_LOGV(TAG, "Turning mild dry off");
    this->cmd[2] = 0x80;
  }

}

}  // namespace CNT
}  // namespace panasonic_ac
}  // namespace esphome
