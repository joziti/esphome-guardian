// Guardian UART: an ESPHome component for the Bluelab Guardian's sensor board.
//
// The Guardian is built from two boards: a sensor/display board (a PIC18
// microcontroller) and a Wi-Fi module (an ESP32-S3). They talk over a 3-wire
// serial link. This component lets an ESP32-S3 running ESPHome take the Wi-Fi
// module's place on that link. It polls the sensor board about once a second
// and exposes the readings, the alarm status and thresholds, the pH
// calibration state and some diagnostics to Home Assistant.
//
// The serial protocol is described in docs/PROTOCOL.md. Comments below refer to
// its sections as "PROTOCOL.md §n".
//
// How the pieces fit together, once per ESPHome loop() call:
//
//   loop()
//     read_incoming_bytes_()       UART bytes -> rx_buffer_
//     process_complete_frames_()   complete frames -> handle_frame_()
//                                    -> handle_measurement_() etc.
//     check_link_timeout_()        declare the link lost if the board is silent
//     publish_link_state_()        keep the "link connected" entity current
//     send_poll_if_due_()          send one request shortly after each board
//                                  frame -> send_next_request_()
//
// A threshold change from Home Assistant arrives through GuardianNumber::control(),
// is stored as a PendingWrite, and goes out with the next poll. It counts as
// done only when the board reports the new range back (PROTOCOL.md §12).

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace guardian_uart {

// The three sensors, numbered as the sensor board numbers them (PROTOCOL.md
// §10). Used as an array index throughout the component.
enum SensorIndex : uint8_t {
  SENSOR_EC = 0,
  SENSOR_TEMPERATURE = 1,
  SENSOR_PH = 2,
};
static constexpr uint8_t SENSOR_COUNT = 3;

// The three pH calibration buffers, numbered as the sensor board numbers its
// calibration records (PROTOCOL.md §12). Used as an array index.
enum PhBuffer : uint8_t {
  PH_BUFFER_4 = 0,
  PH_BUFFER_7 = 1,
  PH_BUFFER_10 = 2,
};
static constexpr uint8_t PH_BUFFER_COUNT = 3;

// The operating mode the sensor board reports (PROTOCOL.md §10). While the mode
// is SETTINGS, CALIBRATION or UPDATE someone is using the Guardian's front
// panel, and the board stops answering requests.
enum class DeviceMode : uint8_t {
  MONITOR = 0,
  CONTROL = 1,
  SETTINGS = 2,
  CALIBRATION = 3,
  UPDATE = 4,
  UNKNOWN = 0xFF,  // not reported yet, or the link is down
};

class GuardianUART;

// One writable alarm threshold: the low or the high bound of one sensor's
// alarm range. Home Assistant calls control() to change it; the number hands
// the new value to the hub component, which does the actual write.
class GuardianNumber : public number::Number {
 public:
  void set_parent(GuardianUART *parent) { this->parent_ = parent; }
  void set_kind(uint8_t sensor, bool is_high_bound) {
    this->sensor_ = sensor;
    this->is_high_bound_ = is_high_bound;
  }

 protected:
  void control(float value) override;

  GuardianUART *parent_{nullptr};
  uint8_t sensor_{SENSOR_EC};
  bool is_high_bound_{false};
};

// The hub component: owns the UART, talks to the sensor board, and updates
// every entity.
class GuardianUART : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Entity registration. Called from the code that ESPHome generates from the
  // YAML configuration (see __init__.py). Every entity is optional.
  void set_value_sensor(uint8_t sensor, sensor::Sensor *entity) { this->value_sensors_[sensor] = entity; }
  void set_alarm_status_sensor(uint8_t sensor, text_sensor::TextSensor *entity) {
    this->alarm_status_sensors_[sensor] = entity;
  }
  void set_reading_status_sensor(uint8_t sensor, text_sensor::TextSensor *entity) {
    this->reading_status_sensors_[sensor] = entity;
  }
  void set_low_alarm_number(uint8_t sensor, GuardianNumber *entity) { this->low_alarm_numbers_[sensor] = entity; }
  void set_high_alarm_number(uint8_t sensor, GuardianNumber *entity) { this->high_alarm_numbers_[sensor] = entity; }
  void set_alarms_set_sensor(binary_sensor::BinarySensor *entity) { this->alarms_set_sensor_ = entity; }
  void set_ph_calibration_due_sensor(binary_sensor::BinarySensor *entity) {
    this->ph_calibration_due_sensor_ = entity;
  }
  void set_link_connected_sensor(binary_sensor::BinarySensor *entity) { this->link_connected_sensor_ = entity; }
  void set_device_mode_sensor(text_sensor::TextSensor *entity) { this->device_mode_sensor_ = entity; }
  // Per-buffer pH calibration: state ("calibrated" / "due" / "none", what the
  // Guardian's lamps show) and the electrode voltage at calibration.
  void set_calibration_state_sensor(uint8_t buffer, text_sensor::TextSensor *entity) {
    this->calibration_state_sensors_[buffer] = entity;
  }
  void set_calibration_voltage_sensor(uint8_t buffer, sensor::Sensor *entity) {
    this->calibration_voltage_sensors_[buffer] = entity;
  }
  // Diagnostics: the board's raw state and calibration messages, as text.
  void set_raw_status_sensor(text_sensor::TextSensor *entity) { this->raw_status_sensor_ = entity; }
  void set_raw_calibration_sensor(text_sensor::TextSensor *entity) { this->raw_calibration_sensor_ = entity; }

  // Called by GuardianNumber::control() when Home Assistant changes a
  // threshold. Validates the new range and queues it for the next poll.
  void request_alarm_write(uint8_t sensor, bool is_high_bound, float value);

 protected:
  // A message inside a frame: the message id and its body (PROTOCOL.md §5).
  using Message = std::pair<uint8_t, std::vector<uint8_t>>;

  // A threshold change waiting to be written and confirmed. There is one slot
  // per sensor, so a second change to the same sensor (the other bound, or the
  // same bound again) merges into the slot instead of replacing it.
  struct PendingWrite {
    bool active{false};
    float low{0};
    float high{0};
    uint32_t deadline{0};  // millis() after which the write is abandoned
  };

  // --- Receiving: bytes -> frames -> messages (PROTOCOL.md §2-§5, §8) ---
  void read_incoming_bytes_();
  void process_complete_frames_();
  size_t find_valid_frame_after_(size_t start) const;
  static bool frame_checksum_ok_(const uint8_t *frame, size_t length);
  static int message_body_length_(uint8_t id);
  void handle_frame_(const uint8_t *payload, size_t length);
  void handle_measurement_(const uint8_t *body);
  void handle_alarm_range_(const uint8_t *body);
  void handle_alarms_set_(const uint8_t *body);
  void handle_status_word_(uint8_t id, const uint8_t *body);
  void handle_lockout_state_(const uint8_t *body);
  void handle_control_event_(const uint8_t *body);
  void handle_calibration_record_(const uint8_t *body);
  void handle_calibration_state_(const uint8_t *body);
  void handle_device_mode_(const uint8_t *body);
  void publish_raw_status_();
  void publish_raw_calibration_();
  void publish_calibration_entities_(uint8_t buffer);
  bool wants_calibration_records_() const;

  // --- Sending: one request per board frame (PROTOCOL.md §9, §12) ---
  void send_poll_if_due_(uint32_t now);
  void send_next_request_();
  bool send_pending_write_();
  void send_frame_(const std::vector<Message> &messages);

  // --- Link state ---
  void note_board_is_alive_();
  void check_link_timeout_(uint32_t now);
  void publish_link_state_();
  bool front_panel_is_busy_() const;

  // --- Alarm ranges and the values derived from them ---
  void store_alarm_range_(uint8_t sensor, float low, float high);
  void publish_alarm_status_(uint8_t sensor);
  void publish_reading_status_(uint8_t sensor);
  void publish_numbers_from_device_(uint8_t sensor);

  // --- Float24, the board's number format (PROTOCOL.md §6) ---
  static float decode_float24_(const uint8_t *bytes);
  static bool encode_float24_(float value, uint8_t out[3]);

  // Receive buffer. Bytes accumulate here until a complete frame can be taken
  // out of the front.
  std::vector<uint8_t> rx_buffer_;

  // Poll timing. After each frame from the board a reply is due at reply_at_.
  // If the board is silent, a poll goes out every fallback_interval_ms_ instead.
  bool reply_due_{false};
  uint32_t reply_at_{0};
  uint32_t last_poll_at_{0};
  uint32_t fallback_interval_ms_{1500};

  // Link state.
  uint32_t last_data_frame_at_{0};  // last frame other than the idle beacon
  bool link_up_{false};
  bool link_state_ever_published_{false};
  bool last_published_link_state_{false};
  DeviceMode device_mode_{DeviceMode::UNKNOWN};

  // Which request to send next. Measurements are the default; the settings,
  // the state and the calibration records are requested when their flag is
  // set, by a timer from the last request for that set or by an event. A flag
  // is cleared when the request is sent.
  bool settings_refresh_wanted_{false};
  bool state_refresh_wanted_{false};
  bool calibration_refresh_wanted_{false};
  uint32_t last_settings_requested_at_{0};
  uint32_t last_state_requested_at_{0};
  uint32_t last_calibration_requested_at_{0};

  // The board's status words from the state stream (PROTOCOL.md §12): low
  // alarm state (0x04), high alarm state (0x05) and calibration due (0x07),
  // indexed by message id minus 0x04 (so index 2, which would be 0x06, is
  // unused). status_words_ holds the significant low bits; raw_words_ the
  // whole u16 as received; logged_words_ the set as last logged.
  static constexpr uint8_t STATUS_WORD_COUNT = 4;
  uint16_t status_words_[STATUS_WORD_COUNT]{0, 0, 0, 0};
  uint16_t raw_words_[STATUS_WORD_COUNT]{0, 0, 0, 0};
  uint16_t logged_words_[STATUS_WORD_COUNT]{0, 0, 0, 0};
  bool have_status_words_{false};

  // The calibration records (0x19, one per pH buffer; PROTOCOL.md §12), and
  // the per-slot messages kept only for the diagnostic text entities: the
  // state stream's 0x06 (lockout state) and 0x08 (control event), which only
  // the controller models send, the status byte of each 0x11 reading, and
  // 0x1A. The first byte of each message selects a slot.
  static constexpr uint8_t RAW_SLOT_COUNT = 4;
  struct CalibrationRecord {
    bool have{false};
    uint16_t count{0};    // write counter
    bool valid{false};    // the buffer holds a real calibration
    float voltage{0};     // electrode voltage at calibration, V
    float temperature{0}; // temperature at calibration, °C
  };
  uint16_t lockout_state_[RAW_SLOT_COUNT]{0, 0, 0, 0};
  bool have_lockout_state_[RAW_SLOT_COUNT]{false, false, false, false};
  uint8_t control_event_[RAW_SLOT_COUNT]{0, 0, 0, 0};
  bool have_control_event_[RAW_SLOT_COUNT]{false, false, false, false};
  uint8_t measurement_status_[RAW_SLOT_COUNT]{0, 0, 0, 0};
  bool have_measurement_status_[RAW_SLOT_COUNT]{false, false, false, false};
  CalibrationRecord calibration_records_[RAW_SLOT_COUNT];
  bool have_calibration_state_{false};
  uint8_t calibration_state_[2]{0, 0};
  std::string last_raw_status_;
  std::string last_raw_calibration_;

  // Threshold writes waiting for the board.
  PendingWrite pending_writes_[SENSOR_COUNT];
  uint8_t next_write_sensor_{SENSOR_EC};  // round-robin start point

  // The last values the board reported, by sensor index.
  float last_value_[SENSOR_COUNT]{NAN, NAN, NAN};
  bool have_alarm_range_[SENSOR_COUNT]{false, false, false};
  float alarm_low_[SENSOR_COUNT]{NAN, NAN, NAN};
  float alarm_high_[SENSOR_COUNT]{NAN, NAN, NAN};

  // Entities, by sensor index where there is one per sensor.
  sensor::Sensor *value_sensors_[SENSOR_COUNT]{nullptr, nullptr, nullptr};
  text_sensor::TextSensor *alarm_status_sensors_[SENSOR_COUNT]{nullptr, nullptr, nullptr};
  text_sensor::TextSensor *reading_status_sensors_[SENSOR_COUNT]{nullptr, nullptr, nullptr};
  GuardianNumber *low_alarm_numbers_[SENSOR_COUNT]{nullptr, nullptr, nullptr};
  GuardianNumber *high_alarm_numbers_[SENSOR_COUNT]{nullptr, nullptr, nullptr};
  binary_sensor::BinarySensor *alarms_set_sensor_{nullptr};
  binary_sensor::BinarySensor *ph_calibration_due_sensor_{nullptr};
  binary_sensor::BinarySensor *link_connected_sensor_{nullptr};
  text_sensor::TextSensor *device_mode_sensor_{nullptr};
  text_sensor::TextSensor *calibration_state_sensors_[PH_BUFFER_COUNT]{nullptr, nullptr, nullptr};
  sensor::Sensor *calibration_voltage_sensors_[PH_BUFFER_COUNT]{nullptr, nullptr, nullptr};
  text_sensor::TextSensor *raw_status_sensor_{nullptr};
  text_sensor::TextSensor *raw_calibration_sensor_{nullptr};
};

}  // namespace guardian_uart
}  // namespace esphome
