/*
 * This file contains source code derived from Adafruit_HTU31D which is under
 * the BSD license:
 *   Written by Limor Fried/Ladyada for Adafruit Industries.
 *   BSD license, all text above must be included in any redistribution.
 *
 * Modifications made by Mark Spicer.
 */

#include "htu31d.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace htu31d {

static const char *const TAG = "htu31d";

/**
 * Resets the sensor and ensures that the devices serial number can be read over
 * I2C.
 */
void HTU31DComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up esphome/components/htu31d HTU31D...");

  if (!reset_()) {
    this->mark_failed();
    return;
  }

  if (read_serial_num_() == 0) {
    this->mark_failed();
    return;
  }
}

/**
 * Called once every update interval (user configured, defaults to 60s) and sets
 * the current temperature and humidity.
 */
void HTU31DComponent::update() {
  ESP_LOGD(TAG, "Checking temperature and humidty values");

  float temperature = 0.0f;
  float humidity = 0.0f;

  // Trigger a conversion. From the spec sheet: The conversion command triggers
  // a single temperature and humidity conversion.
  if (this->write_register(HTU31D_CONVERSION, nullptr, 0) != i2c::ERROR_OK) {
    this->status_set_warning();
    ESP_LOGE(TAG, "Received errror writing conversion register");
    return;
  }

  // Wait conversion time.
  delay(20);

  uint8_t thdata[6];
  if (this->read_register(HTU31D_READTEMPHUM, thdata, 6) != i2c::ERROR_OK) {
    this->status_set_warning();
    ESP_LOGE(TAG, "Error reading temperature/humidty register");
    return;
  }

  // Calculate temperature value.
  uint16_t raw_temp = thdata[0];
  raw_temp <<= 8;
  raw_temp |= thdata[1];

  uint8_t crc = compute_crc_((uint32_t) raw_temp << 8);
  if (crc != thdata[2]) {
    this->status_set_warning();
    ESP_LOGE(TAG, "Error validating temperature CRC");
    return;
  }

  temperature = raw_temp;
  temperature /= 65535.0;
  temperature *= 165;
  temperature -= 40;

  if (this->temperature_ != nullptr) {
    this->temperature_->publish_state(temperature);
  }

  // Calculate humidty value.
  uint16_t raw_hum = thdata[3];
  raw_hum <<= 8;
  raw_hum |= thdata[4];

  crc = compute_crc_((uint32_t) raw_hum << 8);
  if (crc != thdata[5]) {
    this->status_set_warning();
    ESP_LOGE(TAG, "Error validating humidty CRC");
    return;
  }

  humidity = raw_hum;
  humidity /= 65535.0;
  humidity *= 100;

  if (this->humidity_ != nullptr) {
    this->humidity_->publish_state(humidity);
  }

  ESP_LOGD(TAG, "Got Temperature=%.1f°C Humidity=%.1f%%", temperature, humidity);
  this->status_clear_warning();
}

/**
 * Logs the current compoenent config.
 */
void HTU31DComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "HTU31D:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Communication with HTU31D failed!");
  }
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Temperature", this->temperature_);
  LOG_SENSOR("  ", "Humidity", this->humidity_);
}

/**
 * Computes a CRC result for the provided input.
 *
 * @returns the computed CRC result for the provided input
 */
uint8_t HTU31DComponent::compute_crc_(uint32_t value) {
  uint32_t polynom = 0x98800000;  // x^8 + x^5 + x^4 + 1
  uint32_t msb = 0x80000000;
  uint32_t mask = 0xFF800000;
  uint32_t threshold = 0x00000080;
  uint32_t result = value;

  while (msb != threshold) {
    // Check if msb of current value is 1 and apply XOR mask
    if (result & msb)
      result = ((result ^ polynom) & mask) | (result & ~mask);

    // Shift by one
    msb >>= 1;
    mask >>= 1;
    polynom >>= 1;
  }

  return result;
}

/**
 * Sends a 'reset' request to the HTU31D, followed by a 15ms delay.
 *
 * @returns True if was able to write the command successfully
 */
bool HTU31DComponent::reset_() {
  if (this->write_register(HTU31D_RESET, nullptr, 0) != i2c::ERROR_OK) {
    return false;
  }

  delay(15);
  return true;
}

/**
 * Reads the serial number from the device and checks the CRC.
 *
 * @returns the 24bit serial number from the device
 */
uint32_t HTU31DComponent::read_serial_num_() {
  uint8_t reply[4];
  uint32_t serial = 0;

  // Verify we can read the device serial.
  if (this->read_register(HTU31D_READSERIAL, reply, 4) != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Error reading device serial");
    return 0;
  }

  serial = reply[0];
  serial <<= 8;
  serial |= reply[1];
  serial <<= 8;
  serial |= reply[2];
  serial <<= 8;

  uint8_t crc = compute_crc_(serial);
  if (crc != reply[3]) {
    ESP_LOGE(TAG, "Error validating serial CRC");
    return 0;
  }

  ESP_LOGD(TAG, "Found serial: 0x%X", serial);

  return serial;
}

/**
 * Checks the diagnostics register to determine if the heater is currently
 * enabled.
 *
 * @returns True if the heater is currently enabled, False otherwise
 */
bool HTU31DComponent::is_heater_enabled() {
  uint8_t reply[1];
  uint8_t heater_enabled_position = 0;
  uint8_t mask = 1 << heater_enabled_position;
  uint8_t diagnostics = 0;

  if (this->read_register(HTU31D_DIAGNOSTICS, reply, 1) != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Error reading device serial");
    return false;
  }

  diagnostics = reply[0];
  return (diagnostics & mask) != 0;
}

/**
 * Sets the heater state on or off.
 *
 * @param desired True for on, and False for off.
 */
void HTU31DComponent::set_heater_state(bool desired) {
  bool current = is_heater_enabled();

  // If the current state matches the desired state, there is nothing to do.
  if (current == desired) {
    return;
  }

  // Update heater state.
  esphome::i2c::ErrorCode err;
  if (desired) {
    err = this->write_register(HTU31D_HEATERON, nullptr, 0);
  } else {
    err = this->write_register(HTU31D_HEATEROFF, nullptr, 0);
  }

  // Record any error.
  if (err != i2c::ERROR_OK) {
    this->status_set_warning();
    ESP_LOGE(TAG, "Received error updating heater state");
    return;
  }
}

/**
 * Sets the startup priority for this component.
 *
 * @returns The startup priority
 */
float HTU31DComponent::get_setup_priority() const { return setup_priority::DATA; }
}  // namespace htu31d
}  // namespace esphome
