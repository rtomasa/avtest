#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "libretro.h"
#include "images.h"
#include "audio_data.h"

#define FRAME_BUF_WIDTH 320
#define FRAME_BUF_HEIGHT_NTSC 240
#define FRAME_BUF_HEIGHT_PAL 288
#define FRAME_BUF_MAX_HEIGHT FRAME_BUF_HEIGHT_PAL

static uint32_t *frame_buf;
static bool is_50hz = false;
static bool prev_a_pressed = false;
static bool prev_b_pressed = false;
static bool prev_start_pressed = false;
static bool audio_paused = false;
static double audio_sample_rate = 48000.0;
static double audio_frame_accum = 0.0;
static int16_t *audio_buf = NULL;
static size_t audio_buf_frames = 0;
static bool audio_sequential = false;
static bool audio_play_right = false;
char retro_base_directory[4096];
char retro_game_path[4096];

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

struct wav_data {
   const uint8_t *pcm;
   size_t frames;
   uint32_t sample_rate;
   uint16_t channels;
};

static struct wav_data left_wav_data;
static struct wav_data right_wav_data;
static bool audio_ready = false;
static bool audio_use_stereo = false;
static bool audio_has_right = false;
static size_t left_pos = 0;
static size_t right_pos = 0;
static size_t stereo_pos = 0;

static uint16_t read_le_u16(const uint8_t *data)
{
   return (uint16_t)data[0] | (uint16_t)(data[1] << 8);
}

static uint32_t read_le_u32(const uint8_t *data)
{
   return (uint32_t)data[0] |
          (uint32_t)(data[1] << 8) |
          (uint32_t)(data[2] << 16) |
          (uint32_t)(data[3] << 24);
}

static int16_t read_le_s16(const uint8_t *data)
{
   return (int16_t)((uint16_t)data[0] | (uint16_t)(data[1] << 8));
}

static bool parse_wav(const uint8_t *wav, size_t wav_size, struct wav_data *out)
{
   if (!wav || wav_size < 12)
      return false;

   if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
      return false;

   bool found_fmt = false;
   bool found_data = false;
   uint16_t audio_format = 0;
   uint16_t channels = 0;
   uint32_t sample_rate = 0;
   uint16_t bits_per_sample = 0;
   const uint8_t *pcm = NULL;
   uint32_t pcm_size = 0;

   size_t offset = 12;
   while (offset + 8 <= wav_size) {
      const uint8_t *chunk = wav + offset;
      uint32_t chunk_size = read_le_u32(chunk + 4);
      offset += 8;

      if (offset + chunk_size > wav_size)
         return false;

      if (memcmp(chunk, "fmt ", 4) == 0) {
         if (chunk_size < 16)
            return false;
         audio_format = read_le_u16(wav + offset + 0);
         channels = read_le_u16(wav + offset + 2);
         sample_rate = read_le_u32(wav + offset + 4);
         bits_per_sample = read_le_u16(wav + offset + 14);
         found_fmt = true;
      } else if (memcmp(chunk, "data", 4) == 0) {
         pcm = wav + offset;
         pcm_size = chunk_size;
         found_data = true;
      }

      offset += chunk_size;
      if (chunk_size & 1)
         offset++;
   }

   if (!found_fmt || !found_data)
      return false;

   if (audio_format != 1 || (channels != 1 && channels != 2) || bits_per_sample != 16)
      return false;

   size_t frame_size = (size_t)channels * (bits_per_sample / 8);
   if (frame_size == 0 || pcm_size < frame_size)
      return false;

   out->pcm = pcm;
   out->frames = pcm_size / frame_size;
   out->sample_rate = sample_rate;
   out->channels = channels;
   return true;
}

static void ensure_audio_buffer(size_t frames)
{
   if (frames <= audio_buf_frames)
      return;

   int16_t *new_buf = realloc(audio_buf, frames * 2 * sizeof(int16_t));
   if (!new_buf)
      return;

   audio_buf = new_buf;
   audio_buf_frames = frames;
}

static void audio_reset_positions(void)
{
   left_pos = 0;
   right_pos = 0;
   stereo_pos = 0;
   audio_frame_accum = 0.0;
   audio_play_right = false;
}

static void audio_init(void)
{
   struct wav_data left = {0};
   struct wav_data right = {0};

   bool left_ok = parse_wav(Left_wav, Left_wav_len, &left);
   bool right_ok = parse_wav(Right_wav, Right_wav_len, &right);

   audio_ready = false;
   audio_use_stereo = false;
   audio_has_right = false;
   audio_sequential = false;

   if (!left_ok && !right_ok) {
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "Audio: failed to parse embedded WAV data.\n");
      return;
   }

   if (left_ok && left.channels == 2) {
      left_wav_data = left;
      audio_sample_rate = left.sample_rate;
      audio_use_stereo = true;
      audio_ready = true;
   } else {
      if (right_ok && right.channels != 1) {
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Audio: right WAV is not mono, ignoring right channel.\n");
         right_ok = false;
      }

      if (left_ok)
         left_wav_data = left;
      if (right_ok)
         right_wav_data = right;

      audio_has_right = right_ok && right.channels == 1;
      audio_sequential = left_ok && right_ok;
      audio_ready = left_ok || right_ok;

      if (left_ok)
         audio_sample_rate = left.sample_rate;
      else
         audio_sample_rate = right.sample_rate;

      if (left_ok && right_ok && left.sample_rate != right.sample_rate && log_cb) {
         log_cb(RETRO_LOG_WARN,
                "Audio: left/right sample rates differ (%u vs %u), using left.\n",
                left.sample_rate, right.sample_rate);
      }

      if (!left_ok && right_ok) {
         left_wav_data = right;
         audio_has_right = false;
         audio_sequential = false;
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Audio: left WAV missing, mirroring right channel.\n");
      }
   }

   if (audio_sample_rate <= 0.0)
      audio_sample_rate = 48000.0;

   size_t max_frames = (size_t)(audio_sample_rate / 50.0) + 2;
   ensure_audio_buffer(max_frames);
   audio_reset_positions();
}

static void audio_generate(int16_t *out, size_t frames)
{
   if (!out || frames == 0) {
      return;
   }

   if (!audio_ready || audio_paused) {
      memset(out, 0, frames * 2 * sizeof(int16_t));
      return;
   }

   if (audio_use_stereo && left_wav_data.frames > 0) {
      for (size_t i = 0; i < frames; i++) {
         size_t pos = stereo_pos;
         if (pos >= left_wav_data.frames)
            pos = 0;
         const uint8_t *pcm = left_wav_data.pcm + (pos * 4);
         out[i * 2 + 0] = read_le_s16(pcm + 0);
         out[i * 2 + 1] = read_le_s16(pcm + 2);
         stereo_pos = pos + 1;
      }
      return;
   }

   if (audio_sequential && left_wav_data.frames > 0 && right_wav_data.frames > 0) {
      for (size_t i = 0; i < frames; i++) {
         if (!audio_play_right) {
            if (left_pos >= left_wav_data.frames) {
               left_pos = 0;
               audio_play_right = true;
               right_pos = 0;
            }
         } else {
            if (right_pos >= right_wav_data.frames) {
               right_pos = 0;
               audio_play_right = false;
               left_pos = 0;
            }
         }

         int16_t left_sample = 0;
         int16_t right_sample = 0;

         if (!audio_play_right) {
            left_sample = read_le_s16(left_wav_data.pcm + (left_pos * 2));
            left_pos++;
         } else {
            right_sample = read_le_s16(right_wav_data.pcm + (right_pos * 2));
            right_pos++;
         }

         out[i * 2 + 0] = left_sample;
         out[i * 2 + 1] = right_sample;
      }
      return;
   }

   for (size_t i = 0; i < frames; i++) {
      size_t lpos = left_pos;
      if (left_wav_data.frames > 0 && lpos >= left_wav_data.frames)
         lpos = 0;

      size_t rpos = right_pos;
      if (right_wav_data.frames > 0 && rpos >= right_wav_data.frames)
         rpos = 0;

      int16_t left_sample = 0;
      int16_t right_sample = 0;

      if (left_wav_data.frames > 0)
         left_sample = read_le_s16(left_wav_data.pcm + (lpos * 2));

      if (audio_has_right && right_wav_data.frames > 0)
         right_sample = read_le_s16(right_wav_data.pcm + (rpos * 2));
      else
         right_sample = left_sample;

      out[i * 2 + 0] = left_sample;
      out[i * 2 + 1] = right_sample;

      left_pos = lpos + 1;
      right_pos = rpos + 1;
   }
}

static void render_audio(void)
{
   if (!audio_batch_cb && !audio_cb)
      return;

   double fps = is_50hz ? 50.0 : 60.0;
   if (fps <= 0.0 || audio_sample_rate <= 0.0)
      return;

   audio_frame_accum += audio_sample_rate / fps;
   size_t frames = (size_t)audio_frame_accum;
   audio_frame_accum -= frames;

   if (frames == 0)
      return;

   ensure_audio_buffer(frames);
   if (audio_buf_frames < frames)
      return;

   audio_generate(audio_buf, frames);

   if (audio_batch_cb) {
      audio_batch_cb(audio_buf, frames);
      return;
   }

   for (size_t i = 0; i < frames; i++)
      audio_cb(audio_buf[i * 2 + 0], audio_buf[i * 2 + 1]);
}

void load_bg(bool is_50) 
{
   free(frame_buf);

   const uint8_t *data = is_50 ? grid_50_bin : grid_60_bin;
   const unsigned width = FRAME_BUF_WIDTH;
   const unsigned height = is_50 ? FRAME_BUF_HEIGHT_PAL : FRAME_BUF_HEIGHT_NTSC;

   frame_buf = malloc(width * FRAME_BUF_MAX_HEIGHT * sizeof(uint32_t));

   for(unsigned i = 0; i < width * height; i++) {
      uint8_t r = data[i*3 + 0];
      uint8_t g = data[i*3 + 1];
      uint8_t b = data[i*3 + 2];
      frame_buf[i] = (r << 16) | (g << 8) | b;
   }
}

/* Tell the frontend each time you toggle */
static void push_geometry(void)
{
    struct retro_game_geometry geom = {
        FRAME_BUF_WIDTH,
        is_50hz ? FRAME_BUF_HEIGHT_PAL : FRAME_BUF_HEIGHT_NTSC,
        FRAME_BUF_WIDTH,
        FRAME_BUF_MAX_HEIGHT,          /* still 288 */
        (float)FRAME_BUF_WIDTH /
        (float)(is_50hz ? FRAME_BUF_HEIGHT_PAL : FRAME_BUF_HEIGHT_NTSC)
    };
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
}

static void toggle_video_mode(void)
{
    is_50hz = !is_50hz;
    audio_frame_accum = 0.0;
    push_geometry();                   /* ① resize agreement          */

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);     /* ② (optional) real refresh   */
    environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);

    load_bg(is_50hz);                  /* ③ redraw                    */
}

static void check_variables(void)
{
   log_cb(RETRO_LOG_INFO, "Variable updated\n");

   free(frame_buf);
   load_bg(false);

   struct retro_system_av_info av_info;
   retro_get_system_av_info(&av_info);
   environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
}

static void update_input(void)
{
   if (input_poll_cb)
      input_poll_cb();

   // Check if A or B button are pressed
   int16_t input_a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
   int16_t input_b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
   int16_t input_start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);

   if ((input_a && !prev_a_pressed) || (input_b && !prev_b_pressed))
      toggle_video_mode();

   if (input_start && !prev_start_pressed)
      audio_paused = !audio_paused;

   // Update the previous state of the A and B buttons
   prev_a_pressed = input_a != 0;
   prev_b_pressed = input_b != 0;
   prev_start_pressed = input_start != 0;
}

void retro_init(void)
{
   load_bg(false);
   push_geometry();
   audio_init();

   const char *dir = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s", dir);
   }
}

void retro_deinit(void)
{
   free(frame_buf);
   frame_buf = NULL;
   free(audio_buf);
   audio_buf = NULL;
   audio_buf_frames = 0;
   is_50hz = false;
   prev_a_pressed = false;
   prev_b_pressed = false;
   prev_start_pressed = false;
   audio_paused = false;
   audio_ready = false;
   audio_use_stereo = false;
   audio_has_right = false;
   audio_sequential = false;
   audio_play_right = false;
   audio_sample_rate = 48000.0;
   audio_frame_accum = 0.0;
   left_pos = 0;
   right_pos = 0;
   stereo_pos = 0;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = "A/V Test";
   info->library_version = "2.0";
   info->need_fullpath = true;
   info->valid_extensions = "";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->timing.sample_rate = (float)audio_sample_rate;
    info->timing.fps         = is_50hz ? 50.0f : 60.0f;

    info->geometry.base_width   = FRAME_BUF_WIDTH;
    info->geometry.base_height  = is_50hz ? FRAME_BUF_HEIGHT_PAL   /* 288 */
                                          : FRAME_BUF_HEIGHT_NTSC; /* 240 */
    info->geometry.max_width    = FRAME_BUF_WIDTH;
    info->geometry.max_height   = FRAME_BUF_MAX_HEIGHT;            /* **288** no matter what */
    info->geometry.aspect_ratio = (float)info->geometry.base_width /
                                  (float)info->geometry.base_height;
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   struct retro_log_callback logging;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
   {
      log_cb = logging.log;
   }

   static const struct retro_controller_description controllers[] = {
      { "Retropad", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 1 },
      { NULL, 0 },
   };

   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
}

void retro_run(void)
{
   update_input();

   bool updated = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      check_variables();
   }

   unsigned pitch = FRAME_BUF_WIDTH * sizeof(uint32_t); // 4 bytes per pixel

   if (is_50hz)
      video_cb(frame_buf, FRAME_BUF_WIDTH, FRAME_BUF_HEIGHT_PAL, pitch);
   else
      video_cb(frame_buf, FRAME_BUF_WIDTH, FRAME_BUF_HEIGHT_NTSC, pitch);

   render_audio();
}

bool retro_load_game(const struct retro_game_info *info)
{
   static struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A - Switch 50/60Hz" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B - Switch 50/60Hz" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start - Pause/Resume Audio" },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
      return false;
   }

   snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);

   struct retro_audio_callback audio_cb = { NULL, NULL };
   environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio_cb);

   (void)info;
   return true;
}

void retro_unload_game(void)
{
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned idx, bool enabled, const char *code)
{
   (void)idx;
   (void)enabled;
   (void)code;
}
