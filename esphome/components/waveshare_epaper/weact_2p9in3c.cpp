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
static const uint8_t UPDATE_FULL[] = {0x22, 0xC7};

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
  LOG_DISPLAY("", "WeAct E-Paper (3 Color)", this)
  ESP_LOGCONFIG(TAG, "  Model: 2.90in Red+Black");
  LOG_PIN("  CS Pin: ", this->cs_)
  LOG_PIN("  Reset Pin: ", this->reset_pin_)
  LOG_PIN("  DC Pin: ", this->dc_pin_)
  LOG_PIN("  Busy Pin: ", this->busy_pin_)
  LOG_UPDATE_INTERVAL(this)
}

// Device lifecycle

// In weact_2p9in3c.cpp.txt
void WeActEPaper2P9In3C::setup() {
  ESP_LOGD(TAG, "WeActEPaper2P9In3C::setup()");
  WaveshareEPaperBase::setup(); // This will call your overridden initialize()
  // Any other ESPHome component-specific setup can go here if needed
}

void WeActEPaper2P9In3C::initialize() {
  ESP_LOGD(TAG, "WeActEPaper2P9In3C::initialize() - Hardware Init");
  // The base class setup already handled pins, SPI, and reset.
  // We need to wait for idle after reset.
  this->wait_until_idle_();
  this->command(SW_RESET);
  this->wait_until_idle_();

  // Add Power On Command
  this->command(0x04); // Power ON
  this->wait_until_idle_();

  // Add Booster Soft Start (example from GxEPD2 for SSD1680)
  this->command(0x0C);
  this->data(0xD7);
  this->data(0xD6);
  this->data(0x9D);

  // Add VCOM Setting (example from GxEPD2 for GDEW029C90 which uses SSD1680)
  this->command(0x2C); // Write VCOM Register
  this->data(0x68); // Check datasheet/GxEPD2 for optimal value (GxEPD2 uses 0x68 for GDEW029C90, 0xA8 is also common)

  // Your existing init commands (adjust as necessary)
  SEND(DRV_OUT_CTL);    // {0x01, 0x27, 0x01, 0x00} -> Correct for 296 height
  SEND(DATA_ENTRY);     // {0x11, 0x03} -> Correct

  // Border Waveform: GxEPD2 for GDEW029C90 often uses 0x01.
  // Your 0x05 might be okay, but 0x01 is also worth trying if borders are problematic.
  // static const uint8_t BORDER_FULL[] = {0x3C, 0x01}; // Alternative to try
  SEND(BORDER_FULL);    // {0x3C, 0x05}

  SEND(TEMP_SENS);      // {0x18, 0x80} -> Correct
  SEND(DISPLAY_UPDATE); // {0x21, 0x00, 0x80} -> Correct

  this->wait_until_idle_();
  ESP_LOGD(TAG, "Hardware Initialized");
}

void WeActEPaper2P9In3C::send_reset_() {
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->digital_write(false);
    delay(2);
    this->reset_pin_->digital_write(true);
  }
}


void WeActEPaper2P9In3C::deep_sleep() { SEND(SLEEP); }

// Pixel stuff

// t and b are y positions, i.e. line numbers.
void WeActEPaper2P9In3C::set_window_(int t, int b) {
  SEND(RAM_X_RANGE);
  SEND(RAM_Y_RANGE);
  SEND(RAM_X_POS);

  uint8_t buffer[3];
  buffer[0] = RAM_Y_POS;
  buffer[1] = (uint8_t) t % 256;
  buffer[2] = (uint8_t) (t / 256);
  SEND(buffer);
}

// send the buffer starting on line `top`, up to line `bottom`.
void WeActEPaper2P9In3C::write_buffer_(int top, int bottom) {
  auto width_bytes = this->get_width_internal() / 8u;
  auto offset = top * width_bytes;
  auto length = (bottom - top) * width_bytes;

  this->wait_until_idle_();
  this->set_window_(top, bottom);

  this->command(WRITE_BLACK);
  this->start_data_();
  this->write_array(this->buffer_ + offset, length);
  this->end_data_();

  offset += this->get_buffer_length_() / 2u;
  this->command(WRITE_COLOR);
  this->start_data_();
  this->write_array(this->buffer_ + offset, length);
  this->end_data_();
}

void HOT WeActEPaper2P9In3C::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x >= this->get_width_internal() || y >= this->get_height_internal() || x < 0 || y < 0)
      return;

  const uint32_t pos = (x + y * this->get_width_internal()) / 8u;
  const uint8_t subpos = 0x80 >> (x & 0x07); // Bit mask for the pixel

  // Black/White plane
  // Assuming standard SSD1680: 0 for black, 1 for white
  if (color.is_on()) { // ESPHome color is ON (typically black)
      this->buffer_[pos] &= ~subpos; // Clear bit for black
  } else { // ESPHome color is OFF (typically white)
      this->buffer_[pos] |= subpos;  // Set bit for white
  }

  // Color (Red) plane
  // Assuming standard SSD1680: 0 for red, 1 for not-red (use B/W plane)
  const uint32_t color_buf_offset = this->get_buffer_length_() / 2u;
  if ((color.red > 0) && (color.green == 0) && (color.blue == 0)) { // If ESPHome color is pure red
      this->buffer_[pos + color_buf_offset] &= ~subpos; // Clear bit to indicate RED
  } else {
      this->buffer_[pos + color_buf_offset] |= subpos;  // Set bit to indicate NOT RED (use B/W plane)
  }
}

void WeActEPaper2P9In3C::full_update_() {
  ESP_LOGI(TAG, "Performing full e-paper update.");
  this->write_buffer_(0, this->get_height_internal());
  SEND(UPDATE_FULL);
  this->command(ACTIVATE);  // don't wait here
  this->is_busy_ = false;
}

void WeActEPaper2P9In3C::display() {
  if (this->is_busy_ || (this->busy_pin_ != nullptr && this->busy_pin_->digital_read()))
    return;
  this->is_busy_ = true;
  this->full_update_();
}

}  // namespace waveshare_epaper
}  // namespace esphome
