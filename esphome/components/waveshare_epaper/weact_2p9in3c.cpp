#include "waveshare_epaper.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

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
static const uint8_t TEMP_SENS[] = {0x18, 0x80};                // mse internal temp sensor
static const uint8_t DISPLAY_UPDATE[] = {0x21, 0x00, 0x80};     // display update control

// For controlling which part of the image we want to write
static const uint8_t RAM_X_RANGE[] = {0x44, 0x00, WIDTH / 8u - 1};
// Corrected Y Range calculation for 296 height (0x128) - end is 0x127
static const uint8_t RAM_Y_RANGE[] = {0x45, 0x00, 0x00, (uint8_t)(HEIGHT - 1), (uint8_t)((HEIGHT -1) >> 8)};
static const uint8_t RAM_X_POS[] = {0x4E, 0x00};  // Always start X at 0
static const uint8_t RAM_Y_POS_CMD = 0x4F; // Renamed from RAM_Y_POS to avoid confusion with array below

#define SEND(x) this->cmd_data(x, sizeof(x))

// Helper macro for timing sections
#define TIME_SECTION(description, code_block) \
  do { \
    int64_t start_time_##__LINE__ = millis(); \
    code_block; \
    int64_t end_time_##__LINE__ = millis(); \
    ESP_LOGD(TAG, "%s took %lld ms", description, end_time_##__LINE__ - start_time_##__LINE__); \
  } while(0)


// Basics

int WeActEPaper2P9In3C::get_width_internal() { return WIDTH; }
int WeActEPaper2P9In3C::get_height_internal() { return HEIGHT; }
// Make sure base class or component defines a suitable idle_timeout_()
// uint32_t WeActEPaper2P9In3C::idle_timeout_() { return 2500; } // Example if needed

void WeActEPaper2P9In3C::dump_config() {
  LOG_DISPLAY("", "WeAct E-Paper (3 Color)", this);
  ESP_LOGCONFIG(TAG, "  Model: 2.90in Red+Black");
  ESP_LOGCONFIG(TAG, "  Resolution: %dx%d", WIDTH, HEIGHT);
  LOG_PIN("  CS Pin: ", this->cs_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}

// Device lifecycle

void WeActEPaper2P9In3C::setup() {
  ESP_LOGD(TAG, "Setting up WeAct 2.90in 3-Color E-Paper...");
  int64_t setup_start_time = millis();

  TIME_SECTION("Pin setup", this->setup_pins_());

  ESP_LOGD(TAG, "Performing hardware reset...");
  TIME_SECTION("Hardware reset sequence", {
    this->send_reset_(); // Contains its own delays
    // delay(10); // Delay after reset if needed, send_reset_ has one already
    this->command(SW_RESET);
    delay(10); // Delay after SW_RESET
  });

  ESP_LOGD(TAG, "Sending initialization commands...");
  TIME_SECTION("Initialization commands", {
    SEND(DRV_OUT_CTL);
    SEND(DATA_ENTRY);
    SEND(BORDER_FULL);
    SEND(TEMP_SENS);
    SEND(DISPLAY_UPDATE);
    // Don't set window here, wait for first write
  });

  ESP_LOGD(TAG, "Waiting for device to become idle after setup...");
  TIME_SECTION("Initial wait_until_idle", this->wait_until_idle_());

  // Initialize internal busy flag state based on hardware pin
  if (this->busy_pin_ != nullptr) {
     this->is_busy_ = this->busy_pin_->digital_read();
     ESP_LOGD(TAG, "Initial busy state (from pin): %s", ONOFF(this->is_busy_));
  } else {
     this->is_busy_ = false; // Assume not busy if pin not configured
     ESP_LOGD(TAG, "Busy pin not configured, assuming not busy initially.");
  }

  int64_t setup_end_time = millis();
  ESP_LOGI(TAG, "Setup complete. Total setup time: %lld ms", setup_end_time - setup_start_time);
}

void WeActEPaper2P9In3C::send_reset_() {
  if (this->reset_pin_ != nullptr) {
    ESP_LOGD(TAG, "Triggering hardware reset via pin.");
    int64_t reset_start_time = millis();
    this->reset_pin_->digital_write(false);
    delay(10); // Keep delay >= 2ms, 10ms is safe
    this->reset_pin_->digital_write(true);
    delay(10); // Wait for reset to complete
    int64_t reset_end_time = millis();
    ESP_LOGD(TAG, "Hardware reset duration: %lld ms", reset_end_time - reset_start_time);
  } else {
    ESP_LOGD(TAG, "Hardware reset pin not configured, skipping.");
  }
}

// must implement, but we override setup to have more control
void WeActEPaper2P9In3C::initialize() {
    ESP_LOGD(TAG, "initialize() called (delegating to setup()).");
    // Setup is called elsewhere by the component lifecycle
}

void WeActEPaper2P9In3C::deep_sleep() {
  ESP_LOGI(TAG, "Entering deep sleep mode...");
  int64_t sleep_start_time = millis();
  TIME_SECTION("Send SLEEP command", SEND(SLEEP));
  int64_t sleep_end_time = millis();
  ESP_LOGD(TAG, "Deep sleep command sent in %lld ms.", sleep_end_time - sleep_start_time);
}

// Pixel stuff

// Set Memory Area coordinates (X range, Y range)
// Set Memory Address Pointer (X counter, Y counter)
// t and b are y positions (line numbers).
void WeActEPaper2P9In3C::set_window_(int t, int b) {
    // X range is always full width for this driver's common msage
    SEND(RAM_X_RANGE);
    // Set Y range based on top (t) and bottom (b) lines
    // Note: bottom line 'b' seems exclusive in some contexts, or inclusive in others.
    // Assuming 'b' is the last line to be written (inclusive). Let y_end = b - 1 if needed.
    // For this controller, Y range seems inclusive. Height = 296, so lines are 0 to 295.
    uint16_t y_start = t;
    uint16_t y_end = (b > 0) ? (b - 1) : 0; // Adjust if b is exclusive end line number + 1
    if (y_end < y_start) y_end = y_start; // Ensure end >= start

    uint8_t y_range_cmd[5] = {0x45,
                              (uint8_t)(y_start & 0xFF), (uint8_t)(y_start >> 8),
                              (uint8_t)(y_end & 0xFF), (uint8_t)(y_end >> 8)};
    SEND(y_range_cmd);

    // Set RAM address counter to start of window (X=0, Y=t)
    SEND(RAM_X_POS); // Set X counter to 0

    uint8_t y_pos_cmd[3];
    y_pos_cmd[0] = RAM_Y_POS_CMD;
    y_pos_cmd[1] = (uint8_t)(y_start & 0xFF);
    y_pos_cmd[2] = (uint8_t)(y_start >> 8);
    SEND(y_pos_cmd);
}

// send the buffer starting on line `top`, up to line `bottom`.
void WeActEPaper2P9In3C::write_buffer_(int top, int bottom) {
  ESP_LOGD(TAG, "Writing buffer section: lines %d to %d", top, bottom);
  int64_t write_section_start_time = millis();

  auto width_bytes = this->get_width_internal() / 8u;
  auto num_lines = bottom - top;
  if (num_lines <= 0) {
      ESP_LOGW(TAG, "Write buffer called with non-positive line count (%d -> %d)", top, bottom);
      return;
  }
  auto black_offset = top * width_bytes;
  auto length = num_lines * width_bytes;
  auto color_offset = black_offset + (this->get_buffer_length_() / 2u);

  ESP_LOGD(TAG, "Buffer section details: black_offset=%d, color_offset=%d, length=%d bytes",
           black_offset, color_offset, length);


  ESP_LOGD(TAG, "Waiting for idle before writing buffer section...");
  TIME_SECTION("Wait before write", this->wait_until_idle_());

  ESP_LOGD(TAG, "Setting window for lines %d to %d", top, bottom);
  TIME_SECTION("Set window", this->set_window_(top, bottom));

  ESP_LOGD(TAG, "Writing BLACK data for section");
  this->command(WRITE_BLACK);
  TIME_SECTION("Write BLACK data", {
      this->start_data_();
      this->write_array(this->buffer_ + black_offset, length);
      this->end_data_();
  });

  // No intermediate wait needed based on original code

  ESP_LOGD(TAG, "Writing COLOR data for section");
  this->command(WRITE_COLOR);
  TIME_SECTION("Write COLOR data", {
      this->start_data_();
      this->write_array(this->buffer_ + color_offset, length);
      this->end_data_();
  });

  int64_t write_section_end_time = millis();
  ESP_LOGD(TAG, "Finished writing buffer section (%d lines). Duration: %lld ms",
           num_lines, write_section_end_time - write_section_start_time);
}

void HOT WeActEPaper2P9In3C::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x >= this->get_width_internal() || y >= this->get_height_internal() || x < 0 || y < 0)
    return;

  const uint32_t pos = (x / 8u) + (y * (this->get_width_internal() / 8u)); // Corrected calculation
  const uint8_t subpos = 0x80 >> (x & 0x07); // same as x % 8

  // Check bounds for buffer access
  if (pos >= (this->get_buffer_length_() / 2u)) {
       ESP_LOGE(TAG, "Pixel position calculation error: pos (%u) out of bounds for half buffer size (%u)", pos, this->get_buffer_length_() / 2u);
       return; // Prevent buffer overflow
  }

  // Black/White plane: Original logic seems inverted compared to some datasheets,
  // but matches common GxEPD2 practice where 0=Black, 1=White.
  // COLOR_OFF typically maps to black.
  if (color == display::COLOR_OFF) { // Black pixel
    this->buffer_[pos] &= ~subpos; // Clear bit for Black
  } else { // Non-black pixel (White or Red)
    this->buffer_[pos] |= subpos;  // Set bit for Non-black
  }

  // Color (Red) plane:
  // Set bit to 1 for Red, Clear bit to 0 for not-Red (Black or White)
  const uint32_t color_pos = pos + (this->get_buffer_length_() / 2u); // Offset to color plane buffer

  // Check if specifically Red (R>0, G=0, B=0)
  // Assumes Color struct has r, g, b members > 0 for color presence. Adjust if msing different color representation.
  if (((color.red > 0) && (color.green == 0) && (color.blue == 0))) { // Red pixel
    this->buffer_[color_pos] |= subpos; // Set bit for Red
  } else { // Not-Red pixel (Black or White)
    this->buffer_[color_pos] &= ~subpos; // Clear bit for not-Red
  }
}

void WeActEPaper2P9In3C::full_update_() {
  ESP_LOGI(TAG, "Performing full e-paper update sequence...");
  int64_t start_time = millis(); // mse microseconds

  ESP_LOGD(TAG, "Calling write_buffer_ for full screen (0 to %d)", this->get_height_internal());
  TIME_SECTION("Full write_buffer_ call", this->write_buffer_(0, this->get_height_internal()));

  ESP_LOGD(TAG, "Sending Display Update Sequence command...");
  TIME_SECTION("Send UPDATE_FULL command", SEND(UPDATE_FULL));

  ESP_LOGD(TAG, "Sending Turn On Display command (ACTIVATE)...");
  TIME_SECTION("Send ACTIVATE command", this->command(ACTIVATE));
  // We DO NOT wait here. Refresh starts now. Busy pin will go low.

  // Note: is_busy_ was set to false here in the original code.
  // This seems premature as the display IS busy refreshing now.
  // However, sticking to the original logic for now as requested.
  // The display() function's check should prevent issues if called again quickly.
  this->is_busy_ = false;
  ESP_LOGD(TAG, "Set internal busy flag to false (Note: Display HW is still busy refreshing)");

  int64_t end_time = millis(); // mse microseconds
  ESP_LOGI(TAG, "Full e-paper update sequence initiated. Sequence setup time: %lld ms", end_time - start_time);
  ESP_LOGI(TAG, "Display refresh is now in progress (BUSY pin should be LOW)...");
}

void WeActEPaper2P9In3C::display() {
  ESP_LOGD(TAG, "display() called.");

  // Check internal flag first (as per original logic)
  if (this->is_busy_) {
    ESP_LOGD(TAG, "Skipping display() call: internal busy flag is set.");
    return;
  }

  // Check hardware busy pin if available (original logic check: true == busy)
  if (this->busy_pin_ != nullptr && this->busy_pin_->digital_read()) {
    ESP_LOGW(TAG, "E-Paper display is busy (Hardware BUSY pin is HIGH). Skipping update request.");
    // Optionally sync internal flag: this->is_busy_ = true;
    return;
  }

  ESP_LOGD(TAG, "Display not busy, starting full update.");
  this->is_busy_ = true; // Set internal flag before starting
  ESP_LOGD(TAG, "Set internal busy flag to true.");

  // Call the internal update function
  this->full_update_();

  // Crucially, do NOT wait here. Let the main loop continue.
  // The busy flag / pin check at the start of the next call handles overlaps.
}

}  // namespace waveshare_epaper
}  // namespace esphome