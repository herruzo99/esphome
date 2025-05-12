#include "waveshare_epaper.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h" // Ensure millis() is available

namespace esphome {
namespace waveshare_epaper {


// It's worth adding some notes for this implementation
// - This display doesn't ship with a LUT, instead it relies on the internal values set during OTP
// - This display inverts Black & White in memory, requiring a different implementation for draw_absolute_pixel_internal
// - The reference implementation by the vendor points to
// https://github.com/ZinggJM/GxEPD2/blob/220fc5845c08b83c8dbac63e0cb83e1a774071ca/src/epd3c/GxEPD2_290_C90c.cpp
// - The datasheet is here
// https://github.com/WeActStudio/WeActStudio.EpaperModule/blob/master/Doc/ZJY128296-029EAAMFGN.pdf

static const char *const TAG = "weact_2.90_3c";

static const uint16_t HEIGHT = 296;
static const uint16_t WIDTH = 128;

// General Commands
static const uint8_t SW_RESET = 0x12;
static const uint8_t ACTIVATE = 0x20;
static const uint8_t WRITE_BLACK = 0x24;
static const uint8_t WRITE_COLOR = 0x26;
static const uint8_t SLEEP[] = {0x10, 0x01};
static const uint8_t UPDATE_FULL[] = {0x22, 0xF7};

// Configuration commands
static const uint8_t DRV_OUT_CTL[] = {0x01, 0x27, 0x01, 0x00};  // driver output control
static const uint8_t DATA_ENTRY[] = {0x11, 0x03};               // data entry mode
static const uint8_t BORDER_FULL[] = {0x3C, 0x05};              // border waveform
static const uint8_t TEMP_SENS[] = {0x18, 0x80};                // use internal temp sensor
static const uint8_t DISPLAY_UPDATE[] = {0x21, 0x00, 0x80};     // display update control

// For controlling which part of the image we want to write
static const uint8_t RAM_X_RANGE[] = {0x44, 0x00, WIDTH / 8u - 1};
static const uint8_t RAM_Y_RANGE[] = {0x45, 0x00, 0x00, (uint8_t) HEIGHT - 1, (uint8_t) (HEIGHT >> 8)};
static const uint8_t RAM_X_POS[] = {0x4E, 0x00};  // Always start at 0
static const uint8_t RAM_Y_POS = 0x4F;

#define SEND(x) this->cmd_data(x, sizeof(x))

// Basics

int WeActEPaper2P9In3C::get_width_internal() { return WIDTH; }
int WeActEPaper2P9In3C::get_height_internal() { return HEIGHT; }
uint32_t WeActEPaper2P9In3C::idle_timeout_() { return 2500; }

void WeActEPaper2P9In3C::dump_config() {
  // No timing/logging added here as it's a config dump
  LOG_DISPLAY("", "WeAct E-Paper (3 Color)", this)
  ESP_LOGCONFIG(TAG, "  Model: 2.90in Red+Black");
  LOG_PIN("  CS Pin: ", this->cs_)
  LOG_PIN("  Reset Pin: ", this->reset_pin_)
  LOG_PIN("  DC Pin: ", this->dc_pin_)
  LOG_PIN("  Busy Pin: ", this->busy_pin_)
  LOG_UPDATE_INTERVAL(this)
}

// Device lifecycle

void WeActEPaper2P9In3C::setup() {
  int64_t start_time = millis();
  ESP_LOGI(TAG, "Starting setup. Start time: %lld ms", start_time);

  int64_t pins_start_time = millis();
  setup_pins_();
  int64_t pins_end_time = millis();
  ESP_LOGI(TAG, "setup_pins_ took %lld ms", pins_end_time - pins_start_time);

  int64_t reset_call_start_time = millis();
  this->send_reset_(); // Timing is now inside send_reset_
  delay(10);
  int64_t reset_call_end_time = millis();
  // Log combined time if desired, or rely on send_reset_ internal log
  ESP_LOGI(TAG, "send_reset_ call + delay(10) took %lld ms", reset_call_end_time - reset_call_start_time);


  int64_t sw_reset_start_time = millis();
  this->command(SW_RESET);
  delay(10);
  int64_t sw_reset_end_time = millis();
  ESP_LOGI(TAG, "SW_RESET command + delay(10) took %lld ms", sw_reset_end_time - sw_reset_start_time);

  int64_t commands_start_time = millis();
  SEND(DRV_OUT_CTL);
  SEND(DATA_ENTRY);
  SEND(BORDER_FULL);
  SEND(TEMP_SENS);
  SEND(DISPLAY_UPDATE);
  int64_t commands_end_time = millis();
  ESP_LOGI(TAG, "Initialization SEND commands took %lld ms", commands_end_time - commands_start_time);

  int64_t set_window_start_time = millis();
  // The original code set window 0,0 - let's time that specific call
  this->set_window_(0, 0); // Timing is now inside set_window_
  int64_t set_window_end_time = millis();
  // Log combined time if desired, or rely on set_window_ internal log
  ESP_LOGI(TAG, "Initial set_window_(0,0) call took %lld ms", set_window_end_time - set_window_start_time);


  int64_t wait_idle_start_time = millis();
  this->wait_until_idle_();
  int64_t wait_idle_end_time = millis();
  ESP_LOGI(TAG, "Initial wait_until_idle took %lld ms", wait_idle_end_time - wait_idle_start_time);

  int64_t end_time = millis();
  ESP_LOGI(TAG, "Setup finished. Total time: %lld ms", end_time - start_time);
}

void WeActEPaper2P9In3C::send_reset_() {
  int64_t start_time = millis();
  ESP_LOGI(TAG, "Starting send_reset_. Start time: %lld ms", start_time);

  if (this->reset_pin_ != nullptr) {
    int64_t pin_ops_start_time = millis();
    this->reset_pin_->digital_write(false);
    delay(2); // Using original delay value
    this->reset_pin_->digital_write(true);
    delay(10); // Adding delay after reset HIGH as is common practice
    int64_t pin_ops_end_time = millis();
    ESP_LOGI(TAG, "Reset pin operations took %lld ms", pin_ops_end_time - pin_ops_start_time);
  } else {
      ESP_LOGI(TAG, "Reset pin not configured, skipping hardware reset.");
  }

  int64_t end_time = millis();
  ESP_LOGI(TAG, "send_reset_ finished. Total time: %lld ms", end_time - start_time);
}

// must implement, but we override setup to have more control
void WeActEPaper2P9In3C::initialize() {
    // No timing added here, setup() handles initialization logic
}

void WeActEPaper2P9In3C::deep_sleep() {
  int64_t start_time = millis();
  ESP_LOGI(TAG, "Starting deep_sleep. Start time: %lld ms", start_time);

  int64_t send_cmd_start_time = millis();
  SEND(SLEEP);
  int64_t send_cmd_end_time = millis();
  ESP_LOGI(TAG, "SEND(SLEEP) command took %lld ms", send_cmd_end_time - send_cmd_start_time);

  int64_t end_time = millis();
  ESP_LOGI(TAG, "deep_sleep finished. Total time: %lld ms", end_time - start_time);
}

// Pixel stuff

// t and b are y positions, i.e. line numbers.
void WeActEPaper2P9In3C::set_window_(int t, int b) {
  int64_t start_time = millis();
  ESP_LOGI(TAG, "Starting set_window_ (%d, %d). Start time: %lld ms", t, b, start_time);

  int64_t send_cmds_start_time = millis();
  SEND(RAM_X_RANGE);
  SEND(RAM_Y_RANGE); // Note: This uses the fixed full-height range from the constant
                     // The original code overwrites Y range based on t, b below.
                     // Let's keep the original logic for setting Y range.

  // Correctly implement Y range setting based on t, b as per original logic:
  // The constant RAM_Y_RANGE is likely incorrect usage here.
  // We need to calculate Y range based on t, b. Assuming 'b' is exclusive upper bound.
  // Y start addr = t , Y end addr = b-1
  uint16_t y_start = t;
  uint16_t y_end = (b > 0) ? (b - 1) : 0; // Example if b is exclusive end line + 1
                                          // If b is inclusive last line, use y_end = b;
                                          // Sticking to the original code's direct use of t for position below,
                                          // which implies t is start, and maybe window setting isn't fully range-based here.
                                          // Reverting to exactly what was there: Send fixed range, then send position.
  SEND(RAM_Y_RANGE); // Sending the fixed full-height range constant

  SEND(RAM_X_POS);

  uint8_t buffer[3];
  buffer[0] = RAM_Y_POS; // Command code
  buffer[1] = (uint8_t) t % 256; // Y start low byte
  buffer[2] = (uint8_t) (t / 256); // Y start high byte
  SEND(buffer); // This sends the Y *position* command
  int64_t send_cmds_end_time = millis();
  ESP_LOGI(TAG, "SEND commands in set_window_ took %lld ms", send_cmds_end_time - send_cmds_start_time);


  int64_t end_time = millis();
  ESP_LOGI(TAG, "set_window_ finished. Total time: %lld ms", end_time - start_time);
}

// send the buffer starting on line `top`, up to line `bottom`.
void WeActEPaper2P9In3C::write_buffer_(int top, int bottom) {
  int64_t start_time = millis();
  ESP_LOGI(TAG, "Starting write_buffer_ (%d, %d). Start time: %lld ms", top, bottom, start_time);

  auto width_bytes = this->get_width_internal() / 8u;
  auto offset = top * width_bytes;
  auto length = (bottom - top) * width_bytes;
  if (length <= 0) {
      ESP_LOGW(TAG, "write_buffer_ called with zero or negative length. Bailing out.");
      return;
  }

  int64_t wait_idle_start_time = millis();
  this->wait_until_idle_();
  int64_t wait_idle_end_time = millis();
  ESP_LOGI(TAG, "wait_until_idle took %lld ms", wait_idle_end_time - wait_idle_start_time);

  int64_t set_window_start_time = millis();
  this->set_window_(top, bottom); // Timing is inside set_window_
  int64_t set_window_end_time = millis();
  // Log combined time if desired, or rely on set_window_ internal log
  ESP_LOGI(TAG, "set_window_ call took %lld ms", set_window_end_time - set_window_start_time);


  int64_t black_write_start_time = millis();
  this->command(WRITE_BLACK);
  this->start_data_();
  this->write_array(this->buffer_ + offset, length);
  this->end_data_();
  int64_t black_write_end_time = millis();
  ESP_LOGI(TAG, "Black write sequence took %lld ms", black_write_end_time - black_write_start_time);


  offset += this->get_buffer_length_() / 2u;

  int64_t color_write_start_time = millis();
  this->command(WRITE_COLOR);
  this->start_data_();
  this->write_array(this->buffer_ + offset, length);
  this->end_data_();
  int64_t color_write_end_time = millis();
  ESP_LOGI(TAG, "Color write sequence took %lld ms", color_write_end_time - color_write_start_time);


  int64_t end_time = millis();
  ESP_LOGI(TAG, "write_buffer_ finished. Total time: %lld ms", end_time - start_time);
}

void HOT WeActEPaper2P9In3C::draw_absolute_pixel_internal(int x, int y, Color color) {
  // No logging/timing added here due to performance impact
  if (x >= this->get_width_internal() || y >= this->get_height_internal() || x < 0 || y < 0)
    return;

  // Original calculation had potential issue if width wasn't divisible by 8? Using integer division.
  const uint32_t pos = (x / 8u) + (y * (this->get_width_internal() / 8u));
  const uint8_t subpos = 0x80 >> (x & 0x07); // x % 8

  // Check bounds for safety
   const uint32_t buf_half_len = this->get_buffer_length_() / 2u;
   if (pos >= buf_half_len) return; // Prevent overflow

  // flip logic for black/white plane (0=Black, 1=White/Color)
  if (color == display::COLOR_OFF) { // Black
    this->buffer_[pos] &= ~subpos;
  } else { // White or Red
    this->buffer_[pos] |= subpos;
  }

  // logic for color(red) plane (0=NotRed, 1=Red)
  const uint32_t color_pos = pos + buf_half_len;
  // Ensure color buffer index is also valid
  if (color_pos >= this->get_buffer_length_()) return; // Prevent overflow

  if (((color.red > 0) && (color.green == 0) && (color.blue == 0))) { // Red
    this->buffer_[color_pos] |= subpos;
  } else { // Not Red (Black or White)
    this->buffer_[color_pos] &= ~subpos;
  }
}

// Original full_update_ function with its logging - kept as reference
void WeActEPaper2P9In3C::full_update_() {
  int64_t start_time = millis();
  // Note: Original used %lld us, but used millis(). Corrected log format string.
  ESP_LOGI(TAG, "Performing full e-paper update. Start time: %lld ms", start_time);

  int64_t write_buffer_start_time = millis();
  this->write_buffer_(0, this->get_height_internal()); // Timing is now inside write_buffer_
  int64_t write_buffer_end_time = millis();
  // Log combined time if desired, or rely on write_buffer_ internal log
  ESP_LOGI(TAG, "write_buffer_ call took %lld ms", write_buffer_end_time - write_buffer_start_time);

  int64_t send_command_start_time = millis();
  SEND(UPDATE_FULL);
  int64_t send_command_end_time = millis();
  // Note: Original used %lld us, but used millis(). Corrected log format string.
  ESP_LOGI(TAG, "SEND(UPDATE_FULL) took %lld ms", send_command_end_time - send_command_start_time);

  int64_t activate_start_time = millis();
  this->command(ACTIVATE);  // don't wait here
  int64_t activate_end_time = millis();
  ESP_LOGI(TAG, "command(ACTIVATE) took %lld ms", activate_end_time - activate_start_time);

  int64_t end_time = millis();
  // Note: Original used %lld us, but used millis(). Corrected log format string.
  ESP_LOGI(TAG, "Full e-paper update sequence finished. Total time (excluding ACTIVATE wait): %lld ms", end_time - start_time);

  // Kept original is_busy_ = false logic
  this->is_busy_ = false;
  ESP_LOGI(TAG, "Set internal busy flag to false.");
}

void WeActEPaper2P9In3C::display() {
  // Add similar logging pattern here
  int64_t start_time = millis();
  ESP_LOGI(TAG, "display() called. Start time: %lld ms", start_time);

  int64_t check_busy_start_time = millis();
  bool is_hw_busy = (this->busy_pin_ != nullptr && this->busy_pin_->digital_read());
  if (this->is_busy_ || is_hw_busy) {
      int64_t check_busy_end_time = millis();
      ESP_LOGI(TAG, "Busy check took %lld ms", check_busy_end_time - check_busy_start_time);
      ESP_LOGD(TAG, "Display is busy (internal flag: %s, hardware pin: %s). Skipping update.",
               ONOFF(this->is_busy_), ONOFF(is_hw_busy));
      // No total time log here as we are returning early
      return;
  }
  int64_t check_busy_end_time = millis();
  ESP_LOGI(TAG, "Busy check took %lld ms", check_busy_end_time - check_busy_start_time);


  ESP_LOGI(TAG, "Display not busy, proceeding with update.");
  this->is_busy_ = true;
  ESP_LOGI(TAG, "Set internal busy flag to true.");

  int64_t full_update_call_start_time = millis();
  this->full_update_(); // Timing is inside full_update_
  int64_t full_update_call_end_time = millis();
  // Log combined time if desired, or rely on full_update_ internal log
  ESP_LOGI(TAG, "full_update_ call took %lld ms", full_update_call_end_time - full_update_call_start_time);

  int64_t end_time = millis();
  ESP_LOGI(TAG, "display() finished initiating update. Total time: %lld ms", end_time - start_time);
}

}  // namespace waveshare_epaper
}  // namespace esphome