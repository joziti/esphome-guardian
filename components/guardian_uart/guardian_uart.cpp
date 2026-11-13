#include "guardian_uart.h"

#include "esphome/core/log.h"

#include <cmath>
#include <cstdio>

namespace esphome {
namespace guardian_uart {

static const char *const TAG = "guardian_uart";

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

// Every frame starts with this byte (PROTOCOL.md §2).
static const uint8_t FRAME_SYNC = 0xAA;

// Message ids. The board and the host use separate id tables (PROTOCOL.md §8
// and §9); an id that is a request going out is usually its acknowledgement
// coming in. Only the ids this component acts on are named here.
//
// Sent to the board:
static const uint8_t MSG_HOST_MODE = 0x03;          // body: u8 mode; always MODE_MEDIATING
static const uint8_t MSG_HOST_STATUS = 0x04;        // body: u8 status; always 0
static const uint8_t REQ_STATE = 0x02;              // answered with MSG_DEVICE_MODE and the status words
static const uint8_t REQ_MEASUREMENT = 0x10;        // answered with MSG_MEASUREMENT x 4
static const uint8_t REQ_CALIBRATION = 0x18;        // answered with MSG_CALIBRATION_RECORD x 3
static const uint8_t REQ_GET_SETTINGS = 0x20;       // answered with MSG_ALARM_RANGE x 3, MSG_ALARMS_SET, ...
static const uint8_t REQ_SET_ALARM_RANGE = 0x4B;    // body: u8 sensor, Float24 low, Float24 high
//
// Received from the board:
static const uint8_t MSG_DEVICE_VERSION = 0x00;     // the idle beacon of a freshly powered board
static const uint8_t MSG_DEVICE_MODE = 0x03;        // body: u8 mode (DeviceMode)
static const uint8_t MSG_LOW_ALARM_STATE = 0x04;    // body: u16; one bit per sensor in low alarm
static const uint8_t MSG_HIGH_ALARM_STATE = 0x05;   // body: u16; one bit per sensor in high alarm
static const uint8_t MSG_LOCKOUT_STATE = 0x06;      // body: u8 sensor, u16 lockout set (controllers only)
static const uint8_t MSG_CALIBRATION_DUE = 0x07;    // body: u16; one bit per pH buffer due for calibration
static const uint8_t MSG_CONTROL_EVENT = 0x08;      // body: u8 sensor, u8 (controllers only)
static const uint8_t MSG_MEASUREMENT = 0x11;        // body: u8 sensor, Float24 value, u8 status
static const uint8_t MSG_CALIBRATION_RECORD = 0x19; // body: u8 buffer, u16 counter, u8 valid, Float24 V, Float24 °C
static const uint8_t MSG_CALIBRATION_STATE = 0x1A;  // body: u8, u8
static const uint8_t MSG_ALARMS_SET = 0x23;         // body: u8 bool
static const uint8_t MSG_ALARM_RANGE = 0x2B;        // body: u8 sensor, Float24 low, Float24 high

// The host mode sent with every request. A board that was once "mediated" by
// the stock Wi-Fi module ignores requests that lack it; a board that was never
// mediated doesn't mind it (PROTOCOL.md §12). No handshake or security code
// is needed.
static const uint8_t MODE_MEDIATING = 0x07;

// A message body length used only to step over the message (see
// message_body_length_()).
static const int BODY_LENGTH_UNKNOWN = -1;

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

// The board sends a frame about once a second and only honours a request that
// arrives 3-100 ms after its own frame (PROTOCOL.md §12). So each request is
// sent a fixed delay after a board frame. ESPHome's loop() period (~16 ms) adds
// to this delay; 20 ms plus that is well inside the window.
static const uint32_t REPLY_DELAY_MS = 20;

// While the board is silent, polls go out on a timer instead. The interval is
// randomised so a free-running timer cannot phase-lock to the board's ~1.000 s
// period and sit outside the window for minutes.
static const uint32_t FALLBACK_MIN_MS = 1200;
static const uint32_t FALLBACK_SPAN_MS = 600;  // interval = 1200 + [0, 600) ms

// The link is declared lost after this long without a data frame.
static const uint32_t LINK_TIMEOUT_MS = 6000;

// While someone uses the Guardian's front panel the board answers nothing and
// only sends an occasional device-mode frame, 1.5-12 s apart (PROTOCOL.md
// §13). That is not a lost link, so a much longer silence is allowed then.
static const uint32_t FRONT_PANEL_LINK_TIMEOUT_MS = 60000;

// Measurements are requested by default. The other data sets are re-read on
// these periods, each timed from the last request for that set and not from
// its answer, so a board that ignores a request cannot starve the
// measurements. Each refresh costs one measurement cycle.
//
// Settings: the alarm ranges and the "alarms set" flag, so that changes made
// on the Guardian's front panel reach Home Assistant.
static const uint32_t SETTINGS_REFRESH_MS = 5000;
// State: the device mode and the status words (PROTOCOL.md §8), which carry
// the board's alarm flags and the pH calibration-due flags.
static const uint32_t STATE_REFRESH_MS = 5000;
// Calibration records (PROTOCOL.md §12), for the per-buffer calibration
// entities. The state and the records are also re-read whenever the device
// mode changes, which is how a calibration done on the front panel shows up
// within a few seconds.
static const uint32_t CALIBRATION_REFRESH_MS = 60000;

// The significant bits of a status word (PROTOCOL.md §8).
static const uint16_t STATUS_WORD_MASK = 0x000F;

// A threshold write that the board has not reported back within this time is
// abandoned, and the entity goes back to the board's actual value. While a
// write is pending every poll carries a settings request, so measurements
// pause; this keeps a write the board refuses from blanking them for long.
static const uint32_t WRITE_TIMEOUT_MS = 20000;

// ---------------------------------------------------------------------------
// Receive buffer
// ---------------------------------------------------------------------------

// Upper bound on the receive buffer. A healthy link never gets near it.
static const size_t RX_BUFFER_MAX = 512;

// Frame layout: sync byte, one length byte, the payload, one checksum byte
// (PROTOCOL.md §2). The length is a varint in general, but every frame on this
// link is shorter than 251 bytes, where the varint is a single byte (§3).
static const size_t FRAME_OVERHEAD = 3;
static const size_t FRAME_PAYLOAD_OFFSET = 2;
static const size_t NOT_FOUND = SIZE_MAX;

static size_t frame_length_from_header_(uint8_t length_byte) { return FRAME_OVERHEAD + length_byte; }

// Human-readable device mode names, indexed by DeviceMode value.
static const char *const DEVICE_MODE_NAMES[] = {"monitor", "control", "settings", "calibration", "update"};
static const uint8_t DEVICE_MODE_NAME_COUNT = sizeof(DEVICE_MODE_NAMES) / sizeof(DEVICE_MODE_NAMES[0]);

static const char *device_mode_name_(DeviceMode mode) {
  uint8_t index = static_cast<uint8_t>(mode);
  return index < DEVICE_MODE_NAME_COUNT ? DEVICE_MODE_NAMES[index] : "unknown";
}

// Human-readable names for the status byte of a reading (PROTOCOL.md §10),
// indexed by its value.
static const char *const READING_STATUS_NAMES[] = {"ok",           "under range",  "over range",
                                                   "disconnected", "sensor fault", "dependent sensor fault"};
static const uint8_t READING_STATUS_NAME_COUNT = sizeof(READING_STATUS_NAMES) / sizeof(READING_STATUS_NAMES[0]);

static const char *reading_status_name_(uint8_t status) {
  return status < READING_STATUS_NAME_COUNT ? READING_STATUS_NAMES[status] : "unknown";
}

// ---------------------------------------------------------------------------
// Float24 (PROTOCOL.md §6): a signed 16-bit mantissa, little-endian, followed
// by a signed 8-bit decimal count. value = mantissa / 10^decimals.
// ---------------------------------------------------------------------------

float GuardianUART::decode_float24_(const uint8_t *bytes) {
  int16_t mantissa = static_cast<int16_t>(bytes[0] | (bytes[1] << 8));
  int8_t decimals = static_cast<int8_t>(bytes[2]);
  return mantissa / powf(10.0f, static_cast<float>(decimals));
}

// Encodes with the most decimal places (4 down to 0) that still fit the
// mantissa, to keep as much precision as possible. Returns false if the value
// cannot be represented at all.
bool GuardianUART::encode_float24_(float value, uint8_t out[3]) {
  if (!std::isfinite(value))
    return false;
  for (int decimals = 4; decimals >= 0; decimals--) {
    float scaled = value * powf(10.0f, static_cast<float>(decimals));
    if (std::fabs(scaled) >= 32769.0f)
      continue;  // can't fit the mantissa (and keeps lroundf() in its defined range)
    long mantissa = lroundf(scaled);
    if (mantissa < -32768 || mantissa > 32767)
      continue;  // rounding pushed it over the edge
    out[0] = static_cast<uint8_t>(mantissa & 0xFF);
    out[1] = static_cast<uint8_t>((mantissa >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>(static_cast<int8_t>(decimals));
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

// Moves everything the UART has received into rx_buffer_. If the buffer ever
// fills up (it shouldn't), the oldest half is dropped.
void GuardianUART::read_incoming_bytes_() {
  while (this->available()) {
    uint8_t byte;
    if (!this->read_byte(&byte))
      break;
    if (this->rx_buffer_.size() >= RX_BUFFER_MAX)
      this->rx_buffer_.erase(this->rx_buffer_.begin(), this->rx_buffer_.begin() + this->rx_buffer_.size() / 2);
    this->rx_buffer_.push_back(byte);
  }
}

// The XOR of every byte of a valid frame, checksum included, is zero
// (PROTOCOL.md §4).
bool GuardianUART::frame_checksum_ok_(const uint8_t *frame, size_t length) {
  uint8_t x = 0;
  for (size_t i = 0; i < length; i++)
    x ^= frame[i];
  return x == 0;
}

// Returns the index of the first complete frame with a valid checksum that
// starts after `start`, or NOT_FOUND.
size_t GuardianUART::find_valid_frame_after_(size_t start) const {
  const std::vector<uint8_t> &buffer = this->rx_buffer_;
  for (size_t i = start + 1; i + 2 < buffer.size(); i++) {
    if (buffer[i] != FRAME_SYNC)
      continue;
    size_t length = frame_length_from_header_(buffer[i + 1]);
    if (i + length > buffer.size())
      continue;
    if (frame_checksum_ok_(&buffer[i], length))
      return i;
  }
  return NOT_FOUND;
}

// Takes every complete frame out of the front of rx_buffer_ and handles it.
// Bytes that can't be the start of a valid frame are discarded; an incomplete
// frame at the end is kept for the next call.
void GuardianUART::process_complete_frames_() {
  std::vector<uint8_t> &buffer = this->rx_buffer_;
  size_t pos = 0;
  while (pos < buffer.size()) {
    if (buffer[pos] != FRAME_SYNC) {
      pos++;
      continue;
    }
    if (pos + 1 >= buffer.size())
      break;  // the length byte hasn't arrived yet

    size_t length = frame_length_from_header_(buffer[pos + 1]);
    if (pos + length > buffer.size()) {
      // The frame is incomplete. Either the rest is still on its way, or this
      // sync byte is really a data byte inside some other frame and its
      // "length" is nonsense. The framing can't tell the two apart
      // (PROTOCOL.md §2), so: if a complete, valid frame starts further on,
      // this one was false and we resynchronise there. Otherwise keep waiting.
      size_t next = this->find_valid_frame_after_(pos);
      if (next == NOT_FOUND)
        break;
      pos = next;
      continue;
    }

    if (!frame_checksum_ok_(&buffer[pos], length)) {
      pos++;  // a false sync byte; rescan from the next byte
      continue;
    }

    // A good frame. Schedule our reply relative to it and handle its contents.
    this->reply_due_ = true;
    this->reply_at_ = millis() + REPLY_DELAY_MS;
    this->handle_frame_(&buffer[pos + FRAME_PAYLOAD_OFFSET], length - FRAME_OVERHEAD);
    pos += length;
  }
  if (pos > 0)
    buffer.erase(buffer.begin(), buffer.begin() + pos);
}

// The body length of each inbound message id (PROTOCOL.md §8). Messages carry
// no length of their own, so this table is what lets a frame holding several
// messages be walked. Ids this component doesn't act on are still listed so
// that the messages after them can be reached. Returns BODY_LENGTH_UNKNOWN for
// an id that isn't in the table; walking stops there.
int GuardianUART::message_body_length_(uint8_t id) {
  switch (id) {
    case 0x00: return 6;   // DeviceVersion
    case 0x01: return 0;   // version acknowledgement
    case 0x02: return 2;   // RequestResult (StateRequest)
    case 0x03: return 1;   // DeviceMode
    case 0x04: return 2;   // low alarm state word
    case 0x05: return 2;   // high alarm state word
    case 0x06: return 3;   // lockout state
    case 0x07: return 2;   // calibration due word
    case 0x08: return 2;   // control event
    case 0x10: return 2;   // RequestResult (MeasurementRequest)
    case 0x11: return 5;   // Measurement
    case 0x18: return 2;   // RequestResult (CalibrationRequest)
    case 0x19: return 10;  // calibration record
    case 0x1A: return 2;   // calibration state
    case 0x20: return 2;   // RequestResult (GetSettingsRequest)
    case 0x21: return 1;   // setting: device mode
    case 0x22: return 1;   // setting: u8
    case 0x23: return 1;   // setting: alarms set
    case 0x24: return 2;   // setting: lockout
    case 0x25: return 2;   // setting: control mode
    case 0x26: return 4;   // setting: required value
    case 0x27: return 7;   // setting: required range
    case 0x29: return 3;   // setting: on-time
    case 0x2A: return 3;   // setting: off-time
    case 0x2B: return 7;   // setting: alarm range
    case 0x30: return 5;   // pod id and version
    case 0x32: return 1;   // pump speed
    default: break;
  }
  if (id >= 0x41 && id <= 0x5F)
    return 2;  // RequestResult for a Set... request
  if (id >= 0x80 && id <= 0x8F)
    return 2;  // RequestResult for a system request
  // 0x31 (pump setting) is variable-length and never appears on a Guardian
  // Monitor. Anything else is unknown.
  return BODY_LENGTH_UNKNOWN;
}

// Walks the messages in one frame's payload and dispatches each one. The walk
// stops, keeping what was already handled, at anything malformed or unknown
// (PROTOCOL.md §8, "decode leniently").
void GuardianUART::handle_frame_(const uint8_t *payload, size_t length) {
  if (length < 1)
    return;
  uint8_t message_count = payload[0];
  size_t pos = 1;
  for (uint8_t n = 0; n < message_count; n++) {
    if (pos >= length)
      return;  // truncated
    uint8_t id = payload[pos++];
    int body_length = message_body_length_(id);
    if (body_length == BODY_LENGTH_UNKNOWN)
      return;
    if (pos + static_cast<size_t>(body_length) > length)
      return;  // the body would run past the end of the payload
    const uint8_t *body = &payload[pos];
    pos += body_length;

    switch (id) {
      case MSG_DEVICE_VERSION:
        // The beacon of an idle board. Not data, and not proof that the board
        // will answer: the next poll switches it to real data.
        break;
      case MSG_MEASUREMENT:
        this->handle_measurement_(body);
        break;
      case MSG_ALARM_RANGE:
        this->handle_alarm_range_(body);
        break;
      case MSG_ALARMS_SET:
        this->handle_alarms_set_(body);
        break;
      case MSG_DEVICE_MODE:
        this->handle_device_mode_(body);
        break;
      case MSG_LOW_ALARM_STATE:
      case MSG_HIGH_ALARM_STATE:
      case MSG_CALIBRATION_DUE:
        this->handle_status_word_(id, body);
        break;
      case MSG_LOCKOUT_STATE:
        this->handle_lockout_state_(body);
        break;
      case MSG_CONTROL_EVENT:
        this->handle_control_event_(body);
        break;
      case MSG_CALIBRATION_RECORD:
        this->handle_calibration_record_(body);
        break;
      case MSG_CALIBRATION_STATE:
        this->handle_calibration_state_(body);
        break;
      default:
        break;  // acknowledgements and everything else: stepped over
    }
  }
}

// Measurement (0x11): u8 sensor, Float24 value, u8 status.
void GuardianUART::handle_measurement_(const uint8_t *body) {
  this->note_board_is_alive_();
  uint8_t sensor = body[0];
  if (sensor < RAW_SLOT_COUNT) {
    // The status byte (PROTOCOL.md §10) feeds the reading status entity and
    // the diagnostic entity.
    uint8_t status = body[4];
    if (!this->have_measurement_status_[sensor] || this->measurement_status_[sensor] != status) {
      this->measurement_status_[sensor] = status;
      this->have_measurement_status_[sensor] = true;
      if (sensor < SENSOR_COUNT)
        this->publish_reading_status_(sensor);
      this->publish_raw_status_();
    }
  }
  if (sensor >= SENSOR_COUNT)
    return;  // sensor 3 ("other") has no entity; see PROTOCOL.md §10
  float value = decode_float24_(&body[1]);
  this->last_value_[sensor] = value;
  if (this->value_sensors_[sensor] != nullptr)
    this->value_sensors_[sensor]->publish_state(value);
}

// Alarm range (0x2B): u8 sensor, Float24 low, Float24 high. Also confirms a
// pending write for that sensor if the values match.
void GuardianUART::handle_alarm_range_(const uint8_t *body) {
  this->note_board_is_alive_();
  uint8_t sensor = body[0];
  if (sensor >= SENSOR_COUNT)
    return;
  float low = decode_float24_(&body[1]);
  float high = decode_float24_(&body[4]);
  this->store_alarm_range_(sensor, low, high);

  // A write is confirmed by reading the new range back, never by the board's
  // acknowledgement, which may be a stale repeat (PROTOCOL.md §12).
  PendingWrite &write = this->pending_writes_[sensor];
  if (write.active && std::fabs(low - write.low) < 0.05f && std::fabs(high - write.high) < 0.05f) {
    write.active = false;
    ESP_LOGD(TAG, "alarm write confirmed for sensor %u: %.2f..%.2f", sensor, low, high);
  }
}

// "Alarms set" (0x23): u8 bool.
void GuardianUART::handle_alarms_set_(const uint8_t *body) {
  this->note_board_is_alive_();
  bool set = body[0] != 0;
  if (this->alarms_set_sensor_ != nullptr &&
      (!this->alarms_set_sensor_->has_state() || this->alarms_set_sensor_->state != set))
    this->alarms_set_sensor_->publish_state(set);
}

// Status words (0x04, 0x05, 0x07) from the state stream, sent with the device
// mode in answer to a state request. Each is a u16 of which the low 4 bits are
// used, one bit per sensor or per pH buffer (PROTOCOL.md §12):
//
//   0x04  low alarm state: the sensor's reading is below its low alarm
//         threshold (bit 0 = EC, 1 = temperature, 2 = pH)
//   0x05  high alarm state: the reading is above its high alarm threshold
//   0x07  calibration due: the pH buffer's calibration is due (bit 0 = pH 4,
//         1 = pH 7, 2 = pH 10), shown as a flashing lamp on the Guardian
//
// 0x07 is the last word of the set, so the entities that depend on the words
// are updated when it arrives. The set is logged whenever it changes.
void GuardianUART::handle_status_word_(uint8_t id, const uint8_t *body) {
  this->note_board_is_alive_();

  uint8_t index = id - MSG_LOW_ALARM_STATE;
  uint16_t raw = static_cast<uint16_t>(body[0] | (body[1] << 8));
  uint16_t word = raw & STATUS_WORD_MASK;
  this->status_words_[index] = word;
  this->raw_words_[index] = raw;
  if (id == MSG_CALIBRATION_DUE) {
    bool changed = !this->have_status_words_;
    for (uint8_t i = 0; i < STATUS_WORD_COUNT; i++)
      changed = changed || this->status_words_[i] != this->logged_words_[i];
    this->have_status_words_ = true;
    if (changed) {
      for (uint8_t i = 0; i < STATUS_WORD_COUNT; i++)
        this->logged_words_[i] = this->status_words_[i];
      ESP_LOGI(TAG, "status words: low=0x%X high=0x%X due=0x%X", this->status_words_[0], this->status_words_[1],
               this->status_words_[3]);
    }
    bool due = word != 0;
    if (this->ph_calibration_due_sensor_ != nullptr &&
        (!this->ph_calibration_due_sensor_->has_state() || this->ph_calibration_due_sensor_->state != due))
      this->ph_calibration_due_sensor_->publish_state(due);
    for (uint8_t buffer = 0; buffer < PH_BUFFER_COUNT; buffer++)
      this->publish_calibration_entities_(buffer);
    for (uint8_t sensor = 0; sensor < SENSOR_COUNT; sensor++)
      this->publish_alarm_status_(sensor);  // the alarm flags are in this set
    this->publish_raw_status_();
  }
}

// Lockout state (0x06): u8 sensor, u16 set of lockout conditions. Part of the
// state stream on the controller models; a Guardian Monitor does not send it.
// Shown by the diagnostic entity if it appears.
void GuardianUART::handle_lockout_state_(const uint8_t *body) {
  this->note_board_is_alive_();
  uint8_t slot = body[0];
  if (slot >= RAW_SLOT_COUNT)
    return;
  this->lockout_state_[slot] = static_cast<uint16_t>(body[1] | (body[2] << 8));
  this->have_lockout_state_[slot] = true;
  this->publish_raw_status_();
}

// Control event (0x08): u8 sensor, u8. Controller models only; as above.
void GuardianUART::handle_control_event_(const uint8_t *body) {
  this->note_board_is_alive_();
  uint8_t slot = body[0];
  if (slot >= RAW_SLOT_COUNT)
    return;
  this->control_event_[slot] = body[1];
  this->have_control_event_[slot] = true;
  this->publish_raw_status_();
}

// Calibration record (0x19): u8 buffer, u16 write counter, u8 valid, Float24
// electrode voltage, Float24 temperature. Three records, one per pH buffer
// (0 = pH 4, 1 = pH 7, 2 = pH 10), come in answer to a calibration request
// (PROTOCOL.md §12).
void GuardianUART::handle_calibration_record_(const uint8_t *body) {
  this->note_board_is_alive_();
  uint8_t slot = body[0];
  if (slot >= RAW_SLOT_COUNT)
    return;
  CalibrationRecord &record = this->calibration_records_[slot];
  record.have = true;
  record.count = static_cast<uint16_t>(body[1] | (body[2] << 8));
  record.valid = body[3] != 0;
  record.voltage = decode_float24_(&body[4]);
  record.temperature = decode_float24_(&body[7]);
  if (slot < PH_BUFFER_COUNT)
    this->publish_calibration_entities_(slot);
  this->publish_raw_calibration_();
}

// The per-buffer calibration entities. The state follows the Guardian's
// lamps: "due" while the buffer's bit in the 0x07 word is set (a flashing
// lamp), otherwise "calibrated" if its record is valid (a steady lamp) or
// "none" (a dark lamp). The voltage is the record's, or unavailable while
// the record is not valid. Needs both the status words and the record, so it
// is called when either arrives.
void GuardianUART::publish_calibration_entities_(uint8_t buffer) {
  const CalibrationRecord &record = this->calibration_records_[buffer];
  if (!this->have_status_words_ || !record.have)
    return;
  bool due = (this->status_words_[MSG_CALIBRATION_DUE - MSG_LOW_ALARM_STATE] >> buffer) & 1;

  text_sensor::TextSensor *state_sensor = this->calibration_state_sensors_[buffer];
  if (state_sensor != nullptr) {
    const char *state = due ? "due" : record.valid ? "calibrated" : "none";
    if (!state_sensor->has_state() || state_sensor->state != state)
      state_sensor->publish_state(state);
  }

  sensor::Sensor *voltage_sensor = this->calibration_voltage_sensors_[buffer];
  if (voltage_sensor != nullptr) {
    float voltage = record.valid ? record.voltage : NAN;
    bool same = voltage_sensor->has_state() &&
                ((std::isnan(voltage) && std::isnan(voltage_sensor->state)) || voltage == voltage_sensor->state);
    if (!same)
      voltage_sensor->publish_state(voltage);
  }
}

// Whether any configured entity needs the calibration records.
bool GuardianUART::wants_calibration_records_() const {
  if (this->raw_calibration_sensor_ != nullptr)
    return true;
  for (uint8_t buffer = 0; buffer < PH_BUFFER_COUNT; buffer++) {
    if (this->calibration_state_sensors_[buffer] != nullptr || this->calibration_voltage_sensors_[buffer] != nullptr)
      return true;
  }
  return false;
}

// Calibration state (0x1A): u8, u8. Defined but not sent by the Guardian
// Monitor; shown by the diagnostic entity if it appears.
void GuardianUART::handle_calibration_state_(const uint8_t *body) {
  this->note_board_is_alive_();
  this->have_calibration_state_ = true;
  this->calibration_state_[0] = body[0];
  this->calibration_state_[1] = body[1];
  this->publish_raw_calibration_();
}

// Formats the four slots of a per-sensor message as "a b c d", with "-" for a
// slot never received. `width` is the number of hex digits per value.
static void format_slots_(char *out, size_t size, const bool *have, const uint16_t *values, int width) {
  size_t pos = 0;
  for (uint8_t slot = 0; slot < 4 && pos < size; slot++) {
    int n = have[slot] ? snprintf(out + pos, size - pos, "%s%0*X", slot ? " " : "", width, values[slot])
                       : snprintf(out + pos, size - pos, "%s-", slot ? " " : "");
    if (n < 0)
      break;
    pos += static_cast<size_t>(n);
  }
}

// "Board status words": the raw state stream plus the status byte of each
// reading, e.g.
//   04=0000 05=0004 07=0000 06=[- - - -] 08=[- - - -] 11=[00 00 01 00]
// Published whenever its text changes, once the first state set is in.
void GuardianUART::publish_raw_status_() {
  if (this->raw_status_sensor_ == nullptr || !this->have_status_words_)
    return;
  uint16_t events[RAW_SLOT_COUNT];
  uint16_t statuses[RAW_SLOT_COUNT];
  for (uint8_t slot = 0; slot < RAW_SLOT_COUNT; slot++) {
    events[slot] = this->control_event_[slot];
    statuses[slot] = this->measurement_status_[slot];
  }
  char lockouts[32];
  char event_text[32];
  char status_text[32];
  format_slots_(lockouts, sizeof(lockouts), this->have_lockout_state_, this->lockout_state_, 4);
  format_slots_(event_text, sizeof(event_text), this->have_control_event_, events, 2);
  format_slots_(status_text, sizeof(status_text), this->have_measurement_status_, statuses, 2);
  char text[160];
  snprintf(text, sizeof(text), "04=%04X 05=%04X 07=%04X 06=[%s] 08=[%s] 11=[%s]", this->raw_words_[0],
           this->raw_words_[1], this->raw_words_[3], lockouts, event_text, status_text);
  if (this->last_raw_status_ == text)
    return;
  this->last_raw_status_ = text;
  this->raw_status_sensor_->publish_state(this->last_raw_status_);
}

// "Board calibration records": the three 0x19 records and 0x1A, e.g.
//   0:n=1 f=1 a=7 b=0 | 1:- | 2:n=2 f=1 a=4.01 b=-1.2 | 1A=0100
// Published whenever its text changes.
void GuardianUART::publish_raw_calibration_() {
  if (this->raw_calibration_sensor_ == nullptr)
    return;
  char text[192];
  size_t pos = 0;
  for (uint8_t slot = 0; slot < 3 && pos < sizeof(text); slot++) {
    const CalibrationRecord &record = this->calibration_records_[slot];
    int n = record.have ? snprintf(text + pos, sizeof(text) - pos, "%s%u:n=%u f=%u a=%g b=%g", slot ? " | " : "", slot,
                                   record.count, record.valid ? 1 : 0, record.voltage, record.temperature)
                        : snprintf(text + pos, sizeof(text) - pos, "%s%u:-", slot ? " | " : "", slot);
    if (n < 0)
      break;
    pos += static_cast<size_t>(n);
  }
  if (pos < sizeof(text)) {
    if (this->have_calibration_state_) {
      snprintf(text + pos, sizeof(text) - pos, " | 1A=%02X%02X", this->calibration_state_[0],
               this->calibration_state_[1]);
    } else {
      snprintf(text + pos, sizeof(text) - pos, " | 1A=-");
    }
  }
  if (this->last_raw_calibration_ == text)
    return;
  this->last_raw_calibration_ = text;
  this->raw_calibration_sensor_->publish_state(this->last_raw_calibration_);
}

// Device mode (0x03): u8 mode. Sent in answer to a state request, and on its
// own whenever the mode changes, for example when the Guardian enters or
// leaves settings mode.
void GuardianUART::handle_device_mode_(const uint8_t *body) {
  this->note_board_is_alive_();
  DeviceMode mode = static_cast<DeviceMode>(body[0]);
  if (mode == this->device_mode_)
    return;
  this->device_mode_ = mode;
  // The mode changed, so the status words and calibration records may have too
  // (for example after a calibration on the front panel). Re-read both.
  this->state_refresh_wanted_ = true;
  this->calibration_refresh_wanted_ = true;
  if (static_cast<uint8_t>(mode) < DEVICE_MODE_NAME_COUNT) {
    ESP_LOGI(TAG, "device mode: %s%s", device_mode_name_(mode),
             this->front_panel_is_busy_() ? " (front panel busy; holding readings)" : "");
  }
  if (this->device_mode_sensor_ != nullptr)
    this->device_mode_sensor_->publish_state(device_mode_name_(mode));
}

// ---------------------------------------------------------------------------
// Link state
// ---------------------------------------------------------------------------

// Called for every frame that carries data (anything but the idle beacon).
void GuardianUART::note_board_is_alive_() {
  this->last_data_frame_at_ = millis();
  if (!this->link_up_) {
    this->link_up_ = true;
    this->state_refresh_wanted_ = true;  // (re)learn the device mode
    ESP_LOGI(TAG, "PIC link up");
  }
}

bool GuardianUART::front_panel_is_busy_() const {
  return this->device_mode_ == DeviceMode::SETTINGS || this->device_mode_ == DeviceMode::CALIBRATION ||
         this->device_mode_ == DeviceMode::UPDATE;
}

// Declares the link lost when the board has been silent too long, for example
// because the EC probe was unplugged (PROTOCOL.md §13). The readings are made
// unavailable rather than left stale. Polling continues on the fallback timer
// and the link comes back by itself when the board answers again.
void GuardianUART::check_link_timeout_(uint32_t now) {
  uint32_t timeout = this->front_panel_is_busy_() ? FRONT_PANEL_LINK_TIMEOUT_MS : LINK_TIMEOUT_MS;
  if (!this->link_up_ || now - this->last_data_frame_at_ <= timeout)
    return;

  this->link_up_ = false;
  this->device_mode_ = DeviceMode::UNKNOWN;
  if (this->device_mode_sensor_ != nullptr)
    this->device_mode_sensor_->publish_state(device_mode_name_(DeviceMode::UNKNOWN));
  ESP_LOGW(TAG, "PIC link lost (silent for %u ms)", static_cast<unsigned>(timeout));
  for (uint8_t sensor = 0; sensor < SENSOR_COUNT; sensor++) {
    this->last_value_[sensor] = NAN;
    if (this->value_sensors_[sensor] != nullptr)
      this->value_sensors_[sensor]->publish_state(NAN);
  }
}

// Publishes the "link connected" entity once at start-up and then on change.
void GuardianUART::publish_link_state_() {
  if (this->link_connected_sensor_ == nullptr)
    return;
  if (this->link_state_ever_published_ && this->link_up_ == this->last_published_link_state_)
    return;
  this->link_connected_sensor_->publish_state(this->link_up_);
  this->last_published_link_state_ = this->link_up_;
  this->link_state_ever_published_ = true;
}

// ---------------------------------------------------------------------------
// Alarm ranges and the values derived from them
// ---------------------------------------------------------------------------

// Records a range reported by the board and updates the entities that depend
// on it.
void GuardianUART::store_alarm_range_(uint8_t sensor, float low, float high) {
  this->have_alarm_range_[sensor] = true;
  this->alarm_low_[sensor] = low;
  this->alarm_high_[sensor] = high;
  // Show the board's thresholds on the number entities, unless the user's own
  // change for this sensor is still waiting to be confirmed.
  if (!this->pending_writes_[sensor].active)
    this->publish_numbers_from_device_(sensor);
}

// Alarm status is the board's alarm flags for the sensor: "low" while its
// bit is set in the low alarm state word (0x04), "high" while it is set in the
// high alarm state word (0x05), otherwise "ok". This is what the stock Wi-Fi
// module reported (PROTOCOL.md §12). Needs the status words.
void GuardianUART::publish_alarm_status_(uint8_t sensor) {
  text_sensor::TextSensor *entity = this->alarm_status_sensors_[sensor];
  if (entity == nullptr || !this->have_status_words_)
    return;
  const char *status = "ok";
  if ((this->status_words_[MSG_HIGH_ALARM_STATE - MSG_LOW_ALARM_STATE] >> sensor) & 1) {
    status = "high";
  } else if ((this->status_words_[MSG_LOW_ALARM_STATE - MSG_LOW_ALARM_STATE] >> sensor) & 1) {
    status = "low";
  }
  if (!entity->has_state() || entity->state != status)
    entity->publish_state(status);
}

// Reading status: the status byte the board attaches to each reading
// (PROTOCOL.md §10), as text. Published on change.
void GuardianUART::publish_reading_status_(uint8_t sensor) {
  text_sensor::TextSensor *entity = this->reading_status_sensors_[sensor];
  if (entity == nullptr || !this->have_measurement_status_[sensor])
    return;
  const char *status = reading_status_name_(this->measurement_status_[sensor]);
  if (!entity->has_state() || entity->state != status)
    entity->publish_state(status);
}

// Makes the number entities show the range the board actually holds.
void GuardianUART::publish_numbers_from_device_(uint8_t sensor) {
  if (!this->have_alarm_range_[sensor])
    return;
  if (this->low_alarm_numbers_[sensor] != nullptr)
    this->low_alarm_numbers_[sensor]->publish_state(this->alarm_low_[sensor]);
  if (this->high_alarm_numbers_[sensor] != nullptr)
    this->high_alarm_numbers_[sensor]->publish_state(this->alarm_high_[sensor]);
}

// ---------------------------------------------------------------------------
// Writing thresholds
// ---------------------------------------------------------------------------

void GuardianUART::request_alarm_write(uint8_t sensor, bool is_high_bound, float value) {
  if (sensor >= SENSOR_COUNT)
    return;

  // The board always takes both bounds (PROTOCOL.md §12). Start from this
  // sensor's pending write if there is one, so that quick successive changes
  // merge; otherwise from the range the board last reported.
  PendingWrite &write = this->pending_writes_[sensor];
  float low;
  float high;
  if (write.active) {
    low = write.low;
    high = write.high;
  } else if (this->have_alarm_range_[sensor]) {
    low = this->alarm_low_[sensor];
    high = this->alarm_high_[sensor];
  } else {
    low = high = value;
  }
  if (is_high_bound) {
    high = value;
  } else {
    low = value;
  }

  uint8_t encoded[3];
  bool encodable = encode_float24_(low, encoded) && encode_float24_(high, encoded);
  if (low > high || !encodable) {
    ESP_LOGW(TAG, "rejected alarm write for sensor %u: %.2f..%.2f (low > high or out of range)", sensor, low, high);
    // Undo the value the number entity showed optimistically.
    if (!write.active) {
      this->publish_numbers_from_device_(sensor);
    } else if (is_high_bound && this->high_alarm_numbers_[sensor] != nullptr) {
      this->high_alarm_numbers_[sensor]->publish_state(write.high);
    } else if (!is_high_bound && this->low_alarm_numbers_[sensor] != nullptr) {
      this->low_alarm_numbers_[sensor]->publish_state(write.low);
    }
    return;
  }

  write.active = true;
  write.low = low;
  write.high = high;
  write.deadline = millis() + WRITE_TIMEOUT_MS;
  ESP_LOGI(TAG, "queued alarm write for sensor %u: %.2f..%.2f", sensor, low, high);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

// Builds and sends one frame: sync, length, payload (count + messages), XOR
// checksum (PROTOCOL.md §2-§5).
void GuardianUART::send_frame_(const std::vector<Message> &messages) {
  std::vector<uint8_t> payload;
  payload.push_back(static_cast<uint8_t>(messages.size()));  // always < 251, so a 1-byte varint
  for (const Message &message : messages) {
    payload.push_back(message.first);
    payload.insert(payload.end(), message.second.begin(), message.second.end());
  }

  std::vector<uint8_t> frame;
  frame.push_back(FRAME_SYNC);
  frame.push_back(static_cast<uint8_t>(payload.size()));  // always < 251, so a 1-byte varint
  frame.insert(frame.end(), payload.begin(), payload.end());
  uint8_t checksum = 0;
  for (uint8_t byte : frame)
    checksum ^= byte;
  frame.push_back(checksum);

  this->write_array(frame);
}

// Sends a poll REPLY_DELAY_MS after each board frame, or on the fallback timer
// while the board is silent.
void GuardianUART::send_poll_if_due_(uint32_t now) {
  bool reply_time_reached = this->reply_due_ && static_cast<int32_t>(now - this->reply_at_) >= 0;
  bool fallback_elapsed = now - this->last_poll_at_ >= this->fallback_interval_ms_;
  if (!reply_time_reached && !fallback_elapsed)
    return;
  this->reply_due_ = false;
  this->last_poll_at_ = now;
  this->fallback_interval_ms_ = FALLBACK_MIN_MS + random_uint32() % FALLBACK_SPAN_MS;
  this->send_next_request_();
}

// Sends at most one pending threshold write, round-robin over the sensors.
// The write shares its frame with a settings request so that the board
// applies it and reports the new range in the same reply (PROTOCOL.md §12).
// Returns true if a write was sent.
bool GuardianUART::send_pending_write_() {
  for (uint8_t k = 0; k < SENSOR_COUNT; k++) {
    uint8_t sensor = (this->next_write_sensor_ + k) % SENSOR_COUNT;
    PendingWrite &write = this->pending_writes_[sensor];
    if (!write.active)
      continue;
    if (static_cast<int32_t>(millis() - write.deadline) > 0) {
      ESP_LOGW(TAG, "alarm write for sensor %u not confirmed; giving up", sensor);
      write.active = false;
      this->publish_numbers_from_device_(sensor);
      continue;
    }

    uint8_t low[3];
    uint8_t high[3];
    encode_float24_(write.low, low);  // both were validated in request_alarm_write()
    encode_float24_(write.high, high);
    std::vector<uint8_t> body;
    body.push_back(sensor);
    body.insert(body.end(), low, low + 3);
    body.insert(body.end(), high, high + 3);

    this->send_frame_({{MSG_HOST_MODE, {MODE_MEDIATING}},
                       {MSG_HOST_STATUS, {0x00}},
                       {REQ_SET_ALARM_RANGE, body},
                       {REQ_GET_SETTINGS, {}}});
    this->next_write_sensor_ = (sensor + 1) % SENSOR_COUNT;
    this->settings_refresh_wanted_ = false;  // this frame carries a settings request
    this->last_settings_requested_at_ = millis();
    return true;
  }
  return false;
}

// Decides what to ask the board for and sends it. Pending writes come first.
// Otherwise measurements are requested, except when the settings, the state
// or the calibration records are due for a refresh (see the *_REFRESH_MS
// constants). A refresh flag is cleared when its request is sent. Every
// request is preceded by the host mode and status (PROTOCOL.md §12).
void GuardianUART::send_next_request_() {
  if (this->send_pending_write_())
    return;

  uint32_t now = millis();
  bool wants_calibration = this->wants_calibration_records_();
  if (now - this->last_settings_requested_at_ > SETTINGS_REFRESH_MS)
    this->settings_refresh_wanted_ = true;
  if (now - this->last_state_requested_at_ > STATE_REFRESH_MS)
    this->state_refresh_wanted_ = true;
  if (wants_calibration && now - this->last_calibration_requested_at_ > CALIBRATION_REFRESH_MS)
    this->calibration_refresh_wanted_ = true;

  uint8_t request;
  if (this->settings_refresh_wanted_) {
    request = REQ_GET_SETTINGS;
    this->settings_refresh_wanted_ = false;
    this->last_settings_requested_at_ = now;
  } else if (this->state_refresh_wanted_) {
    request = REQ_STATE;
    this->state_refresh_wanted_ = false;
    this->last_state_requested_at_ = now;
  } else if (this->calibration_refresh_wanted_ && wants_calibration) {
    request = REQ_CALIBRATION;
    this->calibration_refresh_wanted_ = false;
    this->last_calibration_requested_at_ = now;
  } else {
    request = REQ_MEASUREMENT;
  }
  this->send_frame_({{MSG_HOST_MODE, {MODE_MEDIATING}}, {MSG_HOST_STATUS, {0x00}}, {request, {}}});
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void GuardianUART::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Guardian UART...");
  this->last_poll_at_ = millis() - this->fallback_interval_ms_;  // poll on the first loop
  // Ask for the settings, the state and the calibration records straight away
  // rather than waiting for the refresh timers, so every entity has a value
  // soon after boot.
  this->settings_refresh_wanted_ = true;
  this->state_refresh_wanted_ = true;
  this->calibration_refresh_wanted_ = true;
}

void GuardianUART::loop() {
  this->read_incoming_bytes_();
  this->process_complete_frames_();

  uint32_t now = millis();
  this->check_link_timeout_(now);
  this->publish_link_state_();
  this->send_poll_if_due_(now);
}

void GuardianUART::dump_config() {
  ESP_LOGCONFIG(TAG, "Guardian UART:");
  LOG_SENSOR("  ", "EC", this->value_sensors_[SENSOR_EC]);
  LOG_SENSOR("  ", "Temperature", this->value_sensors_[SENSOR_TEMPERATURE]);
  LOG_SENSOR("  ", "pH", this->value_sensors_[SENSOR_PH]);
}

// ---------------------------------------------------------------------------
// GuardianNumber
// ---------------------------------------------------------------------------

void GuardianNumber::control(float value) {
  this->publish_state(value);  // show it at once; corrected if the board disagrees
  if (this->parent_ != nullptr)
    this->parent_->request_alarm_write(this->sensor_, this->is_high_bound_, value);
}

}  // namespace guardian_uart
}  // namespace esphome
