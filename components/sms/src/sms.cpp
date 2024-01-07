#include "sms.hpp"

extern "C" {
#include "smsplus/shared.h"
// #include "smsplus/system.h"
// #include "smsplus/sms.h"
};

#include <string>

#include "format.hpp"
#include "fs_init.hpp"
#include "i2s_audio.h"
#include "input.h"
#include "spi_lcd.h"
#include "st7789.hpp"
#include "task.hpp"
#include "statistics.hpp"
#include "video_task.hpp"
#include "audio_task.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static constexpr size_t SMS_SCREEN_WIDTH = 256;
static constexpr size_t SMS_VISIBLE_HEIGHT = 192;

static constexpr size_t GG_SCREEN_WIDTH = 160;
static constexpr size_t GG_VISIBLE_HEIGHT = 144;

static uint16_t palette[PALETTE_SIZE];

static uint8_t *sms_sram = nullptr;
static uint8_t *sms_ram = nullptr;
static uint8_t *sms_vdp_vram = nullptr;

static int frame_buffer_offset = 0;
static int frame_buffer_size = 256*192;
static bool is_gg = false;

static int frame_counter = 0;
static uint16_t muteFrameCount = 0;
static int frame_buffer_index = 0;

void sms_frame(int skip_render);
void sms_init(void);
void sms_reset(void);

extern "C" void system_manage_sram(uint8 *sram, int slot, int mode) {
}

void reset_sms() {
  system_reset();
  frame_counter = 0;
  muteFrameCount = 0;
}

static void init(uint8_t *romdata, size_t rom_data_size) {
  static bool initialized = false;
  if (!initialized) {
    sms_sram = (uint8_t*)heap_caps_malloc(0x8000, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    sms_ram = (uint8_t*)heap_caps_malloc(0x2000, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    sms_vdp_vram = (uint8_t*)heap_caps_malloc(0x4000, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  }

  bitmap.width = SMS_SCREEN_WIDTH;
  bitmap.height = SMS_VISIBLE_HEIGHT;
  bitmap.pitch = bitmap.width;
  bitmap.data = get_frame_buffer0();

  cart.sram = sms_sram;
  sms.wram = sms_ram;
  sms.use_fm = 0;
  vdp.vram = sms_vdp_vram;

  set_option_defaults();

  option.sndrate = AUDIO_SAMPLE_RATE;
  option.overscan = 0;
  option.extra_gg = 0;

  system_init2();
  system_reset();

  frame_counter = 0;
  muteFrameCount = 0;

  initialized = true;
  reset_frame_time();
}

void init_sms(uint8_t *romdata, size_t rom_data_size) {
  is_gg = false;
  hal::set_native_size(SMS_SCREEN_WIDTH, SMS_VISIBLE_HEIGHT, SMS_SCREEN_WIDTH);
  load_rom_data(romdata, rom_data_size);
  sms.console = CONSOLE_SMS;
  sms.display = DISPLAY_NTSC;
  sms.territory = TERRITORY_DOMESTIC;
  frame_buffer_offset = 0;
  frame_buffer_size = SMS_SCREEN_WIDTH * SMS_VISIBLE_HEIGHT;
  init(romdata, rom_data_size);
  fmt::print("sms init done\n");
}

void init_gg(uint8_t *romdata, size_t rom_data_size) {
  is_gg = true;
  hal::set_native_size(GG_SCREEN_WIDTH, GG_VISIBLE_HEIGHT, SMS_SCREEN_WIDTH);
  load_rom_data(romdata, rom_data_size);
  sms.console = CONSOLE_GG;
  sms.display = DISPLAY_NTSC;
  sms.territory = TERRITORY_DOMESTIC;
  frame_buffer_offset = 48; // from odroid_display.cpp lines 1055
  frame_buffer_size = GG_SCREEN_WIDTH * GG_VISIBLE_HEIGHT;
  init(romdata, rom_data_size);
  fmt::print("gg init done\n");
}

void run_sms_rom() {
  auto start = std::chrono::high_resolution_clock::now();
  // handle input here (see system.h and use input.pad and input.system)
  InputState state;
  get_input_state(&state);

  // pad[0] is player 0
  input.pad[0] = 0;
  input.pad[0]|= state.up ? INPUT_UP : 0;
  input.pad[0]|= state.down ? INPUT_DOWN : 0;
  input.pad[0]|= state.left ? INPUT_LEFT : 0;
  input.pad[0]|= state.right ? INPUT_RIGHT : 0;
  input.pad[0]|= state.a ? INPUT_BUTTON2 : 0;
  input.pad[0]|= state.b ? INPUT_BUTTON1 : 0;

  // pad[1] is player 1
  input.pad[1] = 0;

  // system is where we input the start button, as well as soft reset
  input.system = 0;
  input.system |= state.start ? INPUT_START : 0;
  input.system |= state.select ? INPUT_PAUSE : 0;

  // emulate the frame
  if (0 || (frame_counter % 2) == 0) {
    memset(bitmap.data, 0, frame_buffer_size);
    system_frame(0);

    // copy the palette
    render_copy_palette(palette);
    // flip the bytes in the palette
    for (int i = 0; i < PALETTE_SIZE; i++) {
      uint16_t rgb565 = palette[i];
      palette[i] = (rgb565 >> 8) | (rgb565 << 8);
    }
    // set the palette
    hal::set_palette(palette, PALETTE_SIZE);

    // render the frame
    hal::push_frame((uint8_t*)bitmap.data + frame_buffer_offset);
    // ping pong the frame buffer
    frame_buffer_index = !frame_buffer_index;
    bitmap.data = frame_buffer_index
      ? (uint8_t*)get_frame_buffer1()
      : (uint8_t*)get_frame_buffer0();
  } else {
    system_frame(1);
  }

  ++frame_counter;

  // Process audio
  int16_t *audio_buffer = (int16_t*)get_audio_buffer0();
  for (int x = 0; x < sms_snd.sample_count; x++) {
    uint32_t sample;

    if (muteFrameCount < 60 * 2) {
      // When the emulator starts, audible poping is generated.
      // Audio should be disabled during this startup period.
      sample = 0;
      ++muteFrameCount;
    } else {
      sample = (sms_snd.output[0][x] << 16) + sms_snd.output[1][x];
    }

    audio_buffer[x] = sample;
  }
  auto audio_buffer_len = sms_snd.sample_count - 1;

  // push the audio buffer to the audio task
  hal::set_audio_sample_count(audio_buffer_len);
  hal::push_audio(audio_buffer);

  // manage statistics
  auto end = std::chrono::high_resolution_clock::now();
  auto elapsed = std::chrono::duration<float>(end-start).count();
  update_frame_time(elapsed);
  // frame rate should be 60 FPS, so 1/60th second is what we want to sleep for
  static constexpr auto delay = std::chrono::duration<float>(1.0f/60.0f);
  std::this_thread::sleep_until(start + delay);
}

void load_sms(std::string_view save_path) {
  if (save_path.size()) {
    auto f = fopen(save_path.data(), "rb");
    system_load_state(f);
    fclose(f);
  }
}

void save_sms(std::string_view save_path) {
  // open the save path as a file descriptor
  auto f = fopen(save_path.data(), "wb");
  system_save_state(f);
  fclose(f);
}

std::vector<uint8_t> get_sms_video_buffer() {
  int height = is_gg ? GG_VISIBLE_HEIGHT : SMS_VISIBLE_HEIGHT;
  int width = is_gg ? GG_SCREEN_WIDTH : SMS_SCREEN_WIDTH;
  int pitch = SMS_SCREEN_WIDTH;
  std::vector<uint8_t> frame(width * height * 2);
  // the frame data for the SMS is stored in bitmap.data as a 8 bit index into
  // the palette we need to convert this to a 16 bit RGB565 value
  const uint8_t *frame_buffer = bitmap.data + frame_buffer_offset;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t index = frame_buffer[y * pitch + x];
      uint16_t rgb565 = palette[index];
      frame[(y * width + x)*2] = rgb565 & 0xFF;
      frame[(y * width + x)*2+1] = (rgb565 >> 8) & 0xFF;
    }
  }
  return frame;
}

void deinit_sms() {
  // TODO:
}