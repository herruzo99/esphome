#include "waveshare_epaper.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h" // Required for delay() and millis() if not implicitly included
#include "esp_timer.h" // Required for esp_timer_get_time()

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
static const uint32_t BUFFER_SIZE = WIDTH * HEIGHT / 8u; // Size for one color plane

// General Commands
static const uint8_t SW_RESET = 0x12;
static const uint8_t ACTIVATE = 0x20;
static const uint8_t WRITE_BLACK = 0x24;
static const uint8_t WRITE_COLOR = 0x26; // Typically Red for 3-color displays
static const uint8_t SLEEP[] = {0x10, 0x01};
static const uint8_t UPDATE_FULL[] = {0x22, 0xF7};

// Configuration commands
static const uint8_t DRV_OUT_CTL[] = {0x01, 0x27, 0x01, 0x00};  // driver output control
static const uint8_t DATA_ENTRY[] = {0x11, 0x03};               // data entry mode (Y inc, X inc, address counter YX)
static const uint8_t BORDER_FULL[] = {0x3C, 0x05};              // border waveform
static const uint8_t TEMP_SENS[] = {0x18, 0x80};                // use internal temp sensor
static const uint8_t DISPLAY_UPDATE[] = {0x21, 0x00, 0x80};     // display update control 1 (Normal, Ram option)

// For controlling which part of the image we want to write
// Note: Width is 128 pixels = 16 bytes. So X range is 0x00 to 0x0F.
static const uint8_t RAM_X_RANGE[] = {0x44, 0x00, WIDTH / 8u - 1};
// Note: Height is 296. Y range is 0x0000 to 0x0127.
static const uint8_t RAM_Y_RANGE[] = {0x45, 0x00, 0x00, (uint8_t) (HEIGHT - 1), (uint8_t) (HEIGHT >> 8)};
static const uint8_t RAM_X_POS[] = {0x4E, 0x00};  // Set RAM X address counter // Always start at 0
static const uint8_t RAM_Y_POS[] = {0x4F, 0x00, 0x00}; // Set RAM Y address counter // Always start at 0

#define SEND(x) this->cmd_data(x, sizeof(x))

// Helper macro for timing sections
#define TIME_SECTION(description, code_block) \
  do { \
    int64_t start_time_##__LINE__ = esp_timer_get_time(); \
    code_block; \
    int64_t end_time_##__LINE__ = esp_timer_get_time(); \
    ESP_LOGD(TAG, "%s took %lld us", description, end_time_##__LINE__ - start_time_##__LINE__); \
  } while(0)

// Basics

int WeActEPaper2P9In3C::get_width_internal() { return WIDTH; }
int WeActEPaper2P9In3C::get_height_internal() { return HEIGHT; }

// Default timeout for wait_until_idle_() in milliseconds
// This specific display can take around 15-20 seconds for a full refresh.
// The base class likely has a default, ensure it's long enough or override this.
uint32_t WeActEPaper2P9In3C::idle_timeout_() { return 30000; } // 30 seconds timeout

void WeActEPaper2P9In3C::dump_config() {
  LOG_DISPLAY("", "WeAct E-Paper (3 Color)", this);
  ESP_LOGCONFIG(TAG, "  Model: 2.90in Red+Black (Controller Similar to GDEW029C32)");
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
  int64_t setup_start_time = esp_timer_get_time();

  TIME_SECTION("Pin setup", this->setup_pins_());

  ESP_LOGD(TAG, "Performing hardware reset...");
  TIME_SECTION("Hardware reset sequence", {
    this->send_reset_();
    this->delay_microseconds(10000); // delay 10ms
    this->command(SW_RESET);
    this->delay_microseconds(10000); // delay 10ms after SW_RESET
  });

  ESP_LOGD(TAG, "Sending initialization commands...");
  TIME_SECTION("Initialization commands", {
      SEND(DRV_OUT_CTL);
      SEND(DATA_ENTRY);
      SEND(BORDER_FULL);
      SEND(TEMP_SENS);
      SEND(DISPLAY_UPDATE);
      SEND(RAM_X_RANGE);
      SEND(RAM_Y_RANGE);
      SEND(RAM_X_POS);
      SEND(RAM_Y_POS); // Initialize RAM position
  });

  ESP_LOGD(TAG, "Waiting for device to become idle after setup...");
  TIME_SECTION("Initial wait_until_idle", this->wait_until_idle_());

  int64_t setup_end_time = esp_timer_get_time();
  ESP_LOGI(TAG, "Setup complete. Total setup time: %lld us", setup_end_time - setup_start_time);
}

// Perform hardware reset
void WeActEPaper2P9In3C::send_reset_() {
  if (this->reset_pin_ != nullptr) {
    ESP_LOGD(TAG, "Triggering hardware reset via pin.");
    this->reset_pin_->digital_write(false);
    this->delay_microseconds(10000); // Datasheet typically recommends >= 10ms
    this->reset_pin_->digital_write(true);
    this->delay_microseconds(10000); // Wait for reset to complete
  } else {
    ESP_LOGD(TAG, "Hardware reset pin not configured, skipping.");
  }
}

// must implement, but setup() is used for initialization logic
void WeActEPaper2P9In3C::initialize() {}

void WeActEPaper2P9In3C::deep_sleep() {
  ESP_LOGI(TAG, "Entering deep sleep mode...");
  int64_t sleep_start_time = esp_timer_get_time();
  // Optional: Wait until idle before sleeping if potentially busy
  // TIME_SECTION("Wait before sleep", this->wait_until_idle_());
  TIME_SECTION("Send SLEEP command", SEND(SLEEP));
  int64_t sleep_end_time = esp_timer_get_time();
  ESP_LOGD(TAG, "Deep sleep command sent in %lld us.", sleep_end_time - sleep_start_time);
}

// Set RAM address window (X is implicitly full width here)
void WeActEPaper2P9In3C::set_ram_window_(uint16_t y_start, uint16_t y_end) {
    // X range is set once during init, assuming full width writes
    // SEND(RAM_X_RANGE);

    // Set Y range
    uint8_t y_range_cmd[5] = {0x45, (uint8_t)(y_start & 0xFF), (uint8_t)(y_start >> 8),
                              (uint8_t)(y_end & 0xFF), (uint8_t)(y_end >> 8)};
    SEND(y_range_cmd);
}

// Set RAM address counter start position
void WeActEPaper2P9In3C::set_ram_position_(uint8_t x_byte_addr, uint16_t y_addr) {
    // Set X position (byte address)
    uint8_t x_pos_cmd[2] = {0x4E, x_byte_addr};
    SEND(x_pos_cmd);

    // Set Y position
    uint8_t y_pos_cmd[3] = {0x4F, (uint8_t)(y_addr & 0xFF), (uint8_t)(y_addr >> 8)};
    SEND(y_pos_cmd);
}


// Send the buffer contents to the display RAM
// This implementation sends the entire buffer at once.
void WeActEPaper2P9In3C::write_buffer_() {
  ESP_LOGD(TAG, "Writing full buffer to display RAM...");
  int64_t write_start_time = esp_timer_get_time();

  uint32_t black_buffer_size = BUFFER_SIZE;
  uint32_t color_buffer_size = BUFFER_SIZE;
  uint8_t* black_buffer = this->buffer_;
  uint8_t* color_buffer = this->buffer_ + black_buffer_size; // Color buffer follows black buffer

  ESP_LOGD(TAG, "Waiting for idle before buffer write...");
  TIME_SECTION("Wait before buffer write", this->wait_until_idle_());

  ESP_LOGD(TAG, "Setting RAM position to (0, 0)");
  TIME_SECTION("Set RAM position (0,0)", this->set_ram_position_(0, 0));

  ESP_LOGD(TAG, "Writing BLACK buffer (%d bytes)", black_buffer_size);
  this->command(WRITE_BLACK);
  TIME_SECTION("Write BLACK data", {
    this->start_data_();
    this->write_array(black_buffer, black_buffer_size);
    this->end_data_();
  });

  ESP_LOGD(TAG, "Waiting for idle after BLACK write..."); // May or may not be needed depending on controller
  TIME_SECTION("Wait after BLACK write", this->wait_until_idle_());

  ESP_LOGD(TAG, "Setting RAM position to (0, 0) for COLOR buffer");
  TIME_SECTION("Set RAM position (0,0)", this->set_ram_position_(0, 0));

  ESP_LOGD(TAG, "Writing COLOR (Red) buffer (%d bytes)", color_buffer_size);
  this->command(WRITE_COLOR);
  TIME_SECTION("Write COLOR data", {
    this->start_data_();
    this->write_array(color_buffer, color_buffer_size);
    this->end_data_();
   });

  int64_t write_end_time = esp_timer_get_time();
  ESP_LOGD(TAG, "Finished writing buffers. Total buffer write time: %lld us", write_end_time - write_start_time);
}

void HOT WeActEPaper2P9In3C::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= this->get_width_internal() || y < 0 || y >= this->get_height_internal()) {
    // ESP_LOGVV(TAG, "Pixel (%d, %d) out of bounds", x, y); // Very verbose
    return;
  }

  // Calculate buffer position
  // Buffer layout: [Black Plane (WIDTH*HEIGHT/8 bytes)][Color Plane (WIDTH*HEIGHT/8 bytes)]
  uint32_t byte_index = (x / 8) + (y * (this->get_width_internal() / 8));
  uint8_t bit_mask = 0x80 >> (x % 8);

  // Determine pixel color for Black/White plane (inverted logic)
  // White pixel: Set bit to 1 (0xFF = white according to datasheet page 15, but seems inverted in common GxEPD2 usage)
  // Black pixel: Clear bit to 0 (0x00 = black)
  // Check color brightness (white is high brightness)
  if (color.is_on()) { // Treat non-black as "potentially white" for B/W plane
    // This covers White (1,1,1) and Red (1,0,0)
     this->buffer_[byte_index] |= bit_mask; // Set bit for NON-BLACK (White or Red)
  } else { // color is COLOR_OFF (Black)
    this->buffer_[byte_index] &= ~bit_mask; // Clear bit for BLACK
  }

  // Determine pixel color for Red plane
  // Red pixel: Set bit to 1
  // Not-Red pixel: Clear bit to 0
  uint32_t color_byte_index = byte_index + BUFFER_SIZE; // Offset to color plane
  // Check if the color is specifically Red (and not White or other colors)
  if (color.r > 0 && color.g == 0 && color.b == 0) { // Red
      this->buffer_[color_byte_index] |= bit_mask; // Set bit for RED
  } else { // Not Red (Black or White)
      this->buffer_[color_byte_index] &= ~bit_mask; // Clear bit for NOT-RED
  }
}


// Perform a full update sequence
void WeActEPaper2P9In3C::full_update_() {
  ESP_LOGI(TAG, "Performing full e-paper update...");
  int64_t full_update_start_time = esp_timer_get_time();

  // Write the buffer contents to the display RAM
  this->write_buffer_(); // Timing is now inside this function

  ESP_LOGD(TAG, "Sending Display Update Sequence command...");
  TIME_SECTION("Send UPDATE_FULL command", SEND(UPDATE_FULL));

  ESP_LOGD(TAG, "Sending Turn On Display command (ACTIVATE)...");
  TIME_SECTION("Send ACTIVATE command", this->command(ACTIVATE));
  // NOTE: We DO NOT wait for idle here. The refresh process starts now.
  // The busy pin will go low and stay low until the refresh is complete.
  // The display() function (or the main loop) should handle waiting.

  int64_t full_update_end_time = esp_timer_get_time();
  ESP_LOGI(TAG, "Full e-paper update sequence initiated. Setup time: %lld us", full_update_end_time - full_update_start_time);
  ESP_LOGI(TAG, "Display refresh is now in progress (BUSY pin should be LOW)...");
}

// Public method to trigger display update
void WeActEPaper2P9In3C::display() {
  ESP_LOGD(TAG, "display() called.");

  // Check if the device is busy from a previous update
  if (this->busy_pin_ != nullptr && this->busy_pin_->digital_read() == LOW) {
       ESP_LOGW(TAG, "E-Paper display is busy (refresh in progress). Skipping update request.");
       // Log how long it has been busy (optional, requires tracking start time)
       return;
  }
  // If busy pin is not configured, we can't reliably check status.
  // Adding a software busy flag might be needed in that case, but hardware check is preferred.

  ESP_LOGD(TAG, "Display is idle or busy pin not configured. Starting full update.");
  // Note: The busy state is implicitly handled by checking the pin at the start.
  // No need for an explicit is_busy_ software flag if the hardware pin works.
  this->full_update_();

  // It's crucial NOT to call wait_until_idle_() here within the display() function
  // itself, as it would block the main loop for the entire refresh duration (potentially 15-20 seconds).
  // The component's update loop or external logic should poll the busy state if needed,
  // or simply rely on the check at the beginning of the next display() call.
}

// Override wait_until_idle_ to add logging (optional, if base class doesn't log)
void WeActEPaper2P9In3C::wait_until_idle_() {
  if (this->busy_pin_ == nullptr) {
    ESP_LOGD(TAG, "Busy pin not configured, using fallback delay: %d ms", this->idle_timeout_());
    delay(this->idle_timeout_());
    return;
  }
  ESP_LOGD(TAG, "Waiting for BUSY pin to go HIGH...");
  int64_t wait_start_time = esp_timer_get_time();
  uint32_t timeout_ms = this->idle_timeout_(); // Get configured timeout
  uint32_t start_ms = millis();

  // GDEW029C32 Busy is LOW when busy
  while (this->busy_pin_->digital_read() == LOW) {
    if (millis() - start_ms > timeout_ms) {
      ESP_LOGW(TAG, "Timeout waiting for BUSY pin after %d ms!", timeout_ms);
      return; // Avoid getting stuck forever
    }
    yield(); // Allow other tasks to run
    // Consider a small delay if yield() isn't sufficient, e.g., delay(10);
  }
  int64_t wait_end_time = esp_timer_get_time();
  ESP_LOGD(TAG, "BUSY pin is HIGH (idle). Wait duration: %lld us", wait_end_time - wait_start_time);
}


}  // namespace waveshare_epaper
}  // namespace esphome