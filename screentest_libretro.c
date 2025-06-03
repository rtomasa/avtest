#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "libretro.h"
#include "images.h"

#define FRAME_BUF_WIDTH 320
#define FRAME_BUF_HEIGHT_NTSC 240
#define FRAME_BUF_HEIGHT_PAL 288
#define FRAME_BUF_MAX_HEIGHT FRAME_BUF_HEIGHT_PAL

static uint32_t *frame_buf;
static bool is_50hz = false;
static bool prev_a_pressed = false;
static bool prev_b_pressed = false;
char retro_base_directory[4096];
char retro_game_path[4096];

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

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
   // Check if A or B button are pressed
   int16_t input_a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
   int16_t input_b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);

   if ((input_a && !prev_a_pressed) || (input_b && !prev_b_pressed))
      toggle_video_mode();

   // Update the previous state of the A and B buttons
   prev_a_pressed = input_a != 0;
   prev_b_pressed = input_b != 0;
}

void retro_init(void)
{
   load_bg(false);
   push_geometry();

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
   is_50hz = false;
   prev_a_pressed = false;
   prev_b_pressed = false;
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
   info->library_name = "Screen Test";
   info->library_version = "1.0";
   info->need_fullpath = true;
   info->valid_extensions = "";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->timing.sample_rate = 48000.0f;
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
}

bool retro_load_game(const struct retro_game_info *info)
{
   static struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A - Switch 50/60Hz" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B - Switch 50/60Hz" },
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