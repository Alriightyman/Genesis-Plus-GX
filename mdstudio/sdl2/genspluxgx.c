#ifdef __cplusplus
extern "C" {
#endif
#include "m68k.h"
#include "z80.h"

#include "SDL.h"
#include "SDL_thread.h"

#include "shared.h"
#include "sms_ntsc.h"
#include "md_ntsc.h"

#include "debug.h"
#include "debug_wrap.h"

#ifdef __cplusplus
}
#endif
#define SOUND_FREQUENCY 48000
#define SOUND_SAMPLES_SIZE  2048

#define VIDEO_WIDTH  320*2
#define VIDEO_HEIGHT 240*2

int windowWidth = VIDEO_WIDTH;
int windowHeight = VIDEO_HEIGHT;

int joynum = 0;
int running = 0;


jmp_buf jmp_env;
dbg_request_t* dbg_req_core;

int log_error   = 0;
int debug_on    = 0;
int turbo_mode  = 0;
int use_sound   = 1;
int fullscreen = 0;// SDL_WINDOW_FULLSCREEN;
SDL_TimerID sdl_timer_id = NULL;

typedef struct
{
    Uint32 sdlKey;
    Uint32 gpgxKey;
} SDLInputMapping;

SDLInputMapping sdlInputMapping[eInput_COUNT] =
{
    { SDLK_i, INPUT_UP },
    { SDLK_k, INPUT_DOWN },
    { SDLK_j, INPUT_LEFT },
    { SDLK_l, INPUT_RIGHT },
    { SDLK_s, INPUT_B },
    { SDLK_d, INPUT_C },
    { SDLK_a, INPUT_A },
    { SDLK_SPACE, INPUT_START },
    { SDLK_c, INPUT_Z },
    { SDLK_x, INPUT_Y },
    { SDLK_z, INPUT_X },
    { SDLK_m, INPUT_MODE }
};

#ifdef ALT_SDL_RENDERER
struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* back_buffer;
    SDL_Rect srect;
    SDL_Rect drect;
    Uint32 frames_rendered;
} sdl_video;
#else
struct {
    SDL_Window* window;
    SDL_Surface* surf_screen;
    SDL_Surface* surf_bitmap;
    SDL_Rect srect;
    SDL_Rect drect;
    Uint32 frames_rendered;
} sdl_video;
#endif
/* sound */

struct {
    char* current_pos;
    char* buffer;
    int current_emulated_samples;
} sdl_sound;


void CheckForError()
{
    const char* error = SDL_GetError();
    if (strcmp(error,"") != 0)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", error, NULL);
    }
}


static uint8 brm_format[0x40] =
{
  0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00,
  0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
};

#pragma region Sound
static short soundframe[SOUND_SAMPLES_SIZE];

static void sdl_sound_callback(void* userdata, Uint8* stream, int len)
{
    if (sdl_sound.current_emulated_samples < len) {
        memset(stream, 0, len);
    }
    else {
        memcpy(stream, sdl_sound.buffer, len);
        /* loop to compensate desync */
        do {
            sdl_sound.current_emulated_samples -= len;
        } while (sdl_sound.current_emulated_samples > 2 * len);
        memcpy(sdl_sound.buffer,
            sdl_sound.current_pos - sdl_sound.current_emulated_samples,
            sdl_sound.current_emulated_samples);
        sdl_sound.current_pos = sdl_sound.buffer + sdl_sound.current_emulated_samples;
    }
}

static int sdl_sound_init()
{
    int n;
    SDL_AudioSpec as_desired;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Audio initialization failed", sdl_video.window);
        return 0;
    }

    as_desired.freq = SOUND_FREQUENCY;
    as_desired.format = AUDIO_S16SYS;
    as_desired.channels = 2;
    as_desired.samples = SOUND_SAMPLES_SIZE;
    as_desired.callback = sdl_sound_callback;

    if (SDL_OpenAudio(&as_desired, NULL) < 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Audio open failed", sdl_video.window);
        return 0;
    }

    sdl_sound.current_emulated_samples = 0;
    n = SOUND_SAMPLES_SIZE * 2 * sizeof(short) * 20;
    sdl_sound.buffer = (char*)malloc(n);
    if (!sdl_sound.buffer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Can't allocate audio buffer", sdl_video.window);
        return 0;
    }
    memset(sdl_sound.buffer, 0, n);
    sdl_sound.current_pos = sdl_sound.buffer;
    return 1;
}

static void sdl_sound_update(int enabled)
{
    int size = audio_update(soundframe) * 2;

    if (enabled)
    {
        int i;
        short* out;

        SDL_LockAudio();
        out = (short*)sdl_sound.current_pos;
        for (i = 0; i < size; i++)
        {
            *out++ = soundframe[i];
        }
        sdl_sound.current_pos = (char*)out;
        sdl_sound.current_emulated_samples += size * sizeof(short);
        SDL_UnlockAudio();
    }
}

static void sdl_sound_close()
{
    SDL_PauseAudio(1);
    SDL_CloseAudio();
    if (sdl_sound.buffer)
        free(sdl_sound.buffer);
}
#pragma endregion

/* video */
md_ntsc_t *md_ntsc;
sms_ntsc_t *sms_ntsc;

#pragma region Video

static int sdl_video_init()
{
#if defined(USE_8BPP_RENDERING)
    const unsigned long surface_format = SDL_PIXELFORMAT_RGB332;
#elif defined(USE_15BPP_RENDERING)
    const unsigned long surface_format = SDL_PIXELFORMAT_RGB555;
#elif defined(USE_16BPP_RENDERING)
    const unsigned long surface_format = SDL_PIXELFORMAT_RGB565;
#elif defined(USE_32BPP_RENDERING)
    const unsigned long surface_format = SDL_PIXELFORMAT_RGB888;
#endif

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Video initialization failed", sdl_video.window);
        return 0;
    }
    CheckForError();
    sdl_video.window = SDL_CreateWindow("GPGX", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, windowWidth, windowHeight, SDL_WINDOW_HIDDEN | SDL_WINDOW_INPUT_FOCUS );

    CheckForError();
#ifdef ALT_SDL_RENDERER
    sdl_video.renderer = SDL_CreateRenderer(sdl_video.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    CheckForError();
    sdl_video.back_buffer = SDL_CreateTexture(sdl_video.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, windowWidth, windowHeight);
    CheckForError();
#else
    sdl_video.surf_screen = SDL_GetWindowSurface(sdl_video.window);
    sdl_video.surf_bitmap = SDL_CreateRGBSurfaceWithFormat(0, VIDEO_WIDTH, VIDEO_HEIGHT, SDL_BITSPERPIXEL(surface_format), surface_format);
    sdl_video.frames_rendered = 0;
#endif // ALT_SDL_RENDER
    SDL_ShowCursor(0);
    return 1;
}

static void sdl_video_update()
{
    if (system_hw == SYSTEM_MCD)
    {
        system_frame_scd(0);
    }
    else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
    {
        system_frame_gen(0);
    }
    else
    {
        system_frame_sms(0);
    }
    /* viewport size changed */

#ifdef ALT_SDL_RENDERER

    SDL_SetRenderTarget(sdl_video.renderer, sdl_video.back_buffer);
    SDL_SetRenderDrawColor(sdl_video.renderer, 120, 120, 120, 255);
    SDL_RenderClear(sdl_video.renderer);

    void* pixels = NULL;
    int		pitch = 0;
    SDL_LockTexture(sdl_video.back_buffer, NULL, &pixels, &pitch);
    memcpy(pixels, bitmap.data, bitmap.viewport.h * bitmap.pitch);
    SDL_UnlockTexture(sdl_video.back_buffer);

    SDL_RenderSetClipRect(sdl_video.renderer, NULL);

    sdl_video.srect.x = sdl_video.srect.y = 0;
    sdl_video.srect.w = bitmap.viewport.w + 2 * bitmap.viewport.x;
    sdl_video.srect.h = bitmap.viewport.h + 2 * bitmap.viewport.y;

    sdl_video.drect.x = sdl_video.drect.y = 0;
    sdl_video.drect.w = windowWidth;
    sdl_video.drect.h = windowHeight;

    SDL_SetRenderTarget(sdl_video.renderer, NULL);
    SDL_RenderCopy(sdl_video.renderer, sdl_video.back_buffer, &sdl_video.srect, &sdl_video.drect);
    SDL_RenderPresent(sdl_video.renderer);
#else
    if (bitmap.viewport.changed & 1)
    {
        bitmap.viewport.changed &= ~1;
        /* source bitmap */
        sdl_video.srect.w = bitmap.viewport.w + 2 * bitmap.viewport.x;
        sdl_video.srect.h = bitmap.viewport.h + 2 * bitmap.viewport.y;
        sdl_video.srect.x = 0;
        sdl_video.srect.y = 0;
        if (sdl_video.srect.w > sdl_video.surf_screen->w)
        {
            sdl_video.srect.x = (sdl_video.srect.w - sdl_video.surf_screen->w) / 2;
            sdl_video.srect.w = sdl_video.surf_screen->w;
        }
        if (sdl_video.srect.h > sdl_video.surf_screen->h)
        {
            sdl_video.srect.y = (sdl_video.srect.h - sdl_video.surf_screen->h) / 2;
            sdl_video.srect.h = sdl_video.surf_screen->h;
        }

        /* destination bitmap */
        sdl_video.drect.w = sdl_video.srect.w;
        sdl_video.drect.h = sdl_video.srect.h;
        sdl_video.drect.x = (sdl_video.surf_screen->w - sdl_video.drect.w) / 2;
        sdl_video.drect.y = (sdl_video.surf_screen->h - sdl_video.drect.h) / 2;

        /* clear destination surface */
        SDL_FillRect(sdl_video.surf_screen, 0, 0);

#if 0
        if (config.render && (interlaced || config.ntsc))  rect.h *= 2;
        if (config.ntsc) rect.w = (reg[12] & 1) ? MD_NTSC_OUT_WIDTH(rect.w) : SMS_NTSC_OUT_WIDTH(rect.w);
        if (config.ntsc)
        {
            sms_ntsc = (sms_ntsc_t*)malloc(sizeof(sms_ntsc_t));
            md_ntsc = (md_ntsc_t*)malloc(sizeof(md_ntsc_t));

            switch (config.ntsc)
            {
            case 1:
                sms_ntsc_init(sms_ntsc, &sms_ntsc_composite);
                md_ntsc_init(md_ntsc, &md_ntsc_composite);
                break;
            case 2:
                sms_ntsc_init(sms_ntsc, &sms_ntsc_svideo);
                md_ntsc_init(md_ntsc, &md_ntsc_svideo);
                break;
            case 3:
                sms_ntsc_init(sms_ntsc, &sms_ntsc_rgb);
                md_ntsc_init(md_ntsc, &md_ntsc_rgb);
                break;
            }
        }
        else
        {
            if (sms_ntsc)
            {
                free(sms_ntsc);
                sms_ntsc = NULL;
            }

            if (md_ntsc)
            {
                free(md_ntsc);
                md_ntsc = NULL;
            }
        }
#endif
    }

    SDL_BlitSurface(sdl_video.surf_bitmap, &sdl_video.srect, sdl_video.surf_screen, &sdl_video.drect);
    SDL_UpdateWindowSurface(sdl_video.window);
#endif
    ++sdl_video.frames_rendered;
}

static void sdl_video_close()
{
#ifdef ALT_SDL_RENDERER
    SDL_DestroyTexture(sdl_video.back_buffer);
    SDL_DestroyRenderer(sdl_video.renderer);
#else
    SDL_FreeSurface(sdl_video.surf_bitmap);
#endif
    SDL_DestroyWindow(sdl_video.window);
}
#pragma endregion

/* Timer Sync */
#pragma region Timer
struct {
    SDL_sem* sem_sync;
    unsigned ticks;
} sdl_sync;

static Uint32 sdl_sync_timer_callback(Uint32 interval, void* param)
{
    // need to check for paused. if not, then after resuming
    // the emulator will attempt to "catch up" over 60fps. 
    if (!is_debugger_paused())
    {
        SDL_SemPost(sdl_sync.sem_sync);
        sdl_sync.ticks++;
        if (sdl_sync.ticks == (vdp_pal ? 50 : 20))
        {
            SDL_Event event;
            SDL_UserEvent userevent;

            userevent.type = SDL_USEREVENT;
            userevent.code = vdp_pal ? (sdl_video.frames_rendered / 3) : sdl_video.frames_rendered;
            userevent.data1 = NULL;
            userevent.data2 = NULL;
            sdl_sync.ticks = sdl_video.frames_rendered = 0;

            event.type = SDL_USEREVENT;
            event.user = userevent;

            SDL_PushEvent(&event);
        }
    }
    return interval;
}

static int sdl_sync_init()
{
    if (SDL_InitSubSystem(SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL Timer initialization failed", sdl_video.window);
        return 0;
    }

    sdl_sync.sem_sync = SDL_CreateSemaphore(0);
    sdl_sync.ticks = 0;
    return 1;
}

static void sdl_sync_close()
{
    if (sdl_sync.sem_sync)
        SDL_DestroySemaphore(sdl_sync.sem_sync);

    if (sdl_timer_id)
    {
        SDL_RemoveTimer(sdl_timer_id);
    }
}

#pragma endregion 


static const uint16 vc_table[4][2] =
{
    /* NTSC, PAL */
    {0xDA , 0xF2},  /* Mode 4 (192 lines) */
    {0xEA , 0x102}, /* Mode 5 (224 lines) */
    {0xDA , 0xF2},  /* Mode 4 (192 lines) */
    {0x106, 0x10A}  /* Mode 5 (240 lines) */
};

#pragma region Input
// Unused - key press to update is extremely slow.. 
// takes a couple of seconds to register
static int sdl_control_update(SDL_Keycode keystate)
{
    switch (keystate)
    {
      case SDLK_TAB:
      {
        system_reset();
        break;
      }
      /*
      case SDLK_F1:
      {
        if (SDL_ShowCursor(-1)) SDL_ShowCursor(0);
        else SDL_ShowCursor(1);
        break;
      }

      case SDLK_F2:
      {
        fullscreen = (fullscreen ? 0 : SDL_WINDOW_FULLSCREEN);
        SDL_SetWindowFullscreen(sdl_video.window, fullscreen);
        sdl_video.surf_screen  = SDL_GetWindowSurface(sdl_video.window);
        bitmap.viewport.changed = 1;
        break;
      }

      case SDLK_F3:
      {
        if (config.bios == 0) config.bios = 3;
        else if (config.bios == 3) config.bios = 1;
        break;
      }

      case SDLK_F4:
      {
        if (!turbo_mode) use_sound ^= 1;
        break;
      }

      case SDLK_F5:
      {
        log_error ^= 1;
        break;
      }

      case SDLK_F6:
      {
        if (!use_sound)
        {
          turbo_mode ^=1;
          sdl_sync.ticks = 0;
        }
        break;
      }

      case SDLK_F7:
      {
        FILE *f = fopen("game.gp0","rb");
        if (f)
        {
          uint8 buf[STATE_SIZE];
          fread(&buf, STATE_SIZE, 1, f);
          state_load(buf);
          fclose(f);
        }
        break;
      }

      case SDLK_F8:
      {
        FILE *f = fopen("game.gp0","wb");
        if (f)
        {
          uint8 buf[STATE_SIZE];
          int len = state_save(buf);
          fwrite(&buf, len, 1, f);
          fclose(f);
        }
        break;
      }

      case SDLK_F9:
      {
        config.region_detect = (config.region_detect + 1) % 5;
        get_region(0);

        // framerate has changed, reinitialize audio timings 
        audio_init(snd.sample_rate, 0);

        // system with region BIOS should be reinitialized 
        if ((system_hw == SYSTEM_MCD) || ((system_hw & SYSTEM_SMS) && (config.bios & 1)))
        {
          system_init();
          system_reset();
        }
        else
        {
          // reinitialize I/O region register 
          if (system_hw == SYSTEM_MD)
          {
            io_reg[0x00] = 0x20 | region_code | (config.bios & 1);
          }
          else
          {
            io_reg[0x00] = 0x80 | (region_code >> 1);
          }

          // reinitialize VDP 
          if (vdp_pal)
          {
            status |= 1;
            lines_per_frame = 313;
          }
          else
          {
            status &= ~1;
            lines_per_frame = 262;
          }

          // reinitialize VC max value
          switch (bitmap.viewport.h)
          {
            case 192:
              vc_max = vc_table[0][vdp_pal];
              break;
            case 224:
              vc_max = vc_table[1][vdp_pal];
              break;
            case 240:
              vc_max = vc_table[3][vdp_pal];
              break;
          }
        }
        break;
      }

      case SDLK_F10:
      {
        gen_reset(0);
        break;
      }

      case SDLK_F11:
      {
        config.overscan =  (config.overscan + 1) & 3;
        if ((system_hw == SYSTEM_GG) && !config.gg_extra)
        {
          bitmap.viewport.x = (config.overscan & 2) ? 14 : -48;
        }
        else
        {
          bitmap.viewport.x = (config.overscan & 2) * 7;
        }
        bitmap.viewport.changed = 3;
        break;
      }

      case SDLK_F12:
      {
        joynum = (joynum + 1) % MAX_DEVICES;
        while (input.dev[joynum] == NO_DEVICE)
        {
          joynum = (joynum + 1) % MAX_DEVICES;
        }
        break;
      }
      */
      case SDLK_ESCAPE:
      {
        return 0;
      }

      default:
        break;
    }    

   return 1;
}

int sdl_input_update(void)
{
    const uint8* keystate = SDL_GetKeyboardState(NULL);

    /* reset input */
    input.pad[joynum] = 0;

    switch (input.dev[joynum])
    {
        case DEVICE_LIGHTGUN:
        {
    #ifndef ALT_SDL_RENDERER
            /* get mouse coordinates (absolute values) */
            int x, y;
            int state = SDL_GetMouseState(&x, &y);

            /* X axis */
            input.analog[joynum][0] = x - (sdl_video.surf_screen->w - bitmap.viewport.w) / 2;

            /* Y axis */
            input.analog[joynum][1] = y - (sdl_video.surf_screen->h - bitmap.viewport.h) / 2;

            /* TRIGGER, B, C (Menacer only), START (Menacer & Justifier only) */
            if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_A;
            if (state & SDL_BUTTON_RMASK) input.pad[joynum] |= INPUT_B;
            if (state & SDL_BUTTON_MMASK) input.pad[joynum] |= INPUT_C;
            if (keystate[SDL_SCANCODE_F])  input.pad[joynum] |= INPUT_START;
    #endif // !ALT_SDL_RENDER
            break;
        }

        case DEVICE_PADDLE:
        {
    #ifndef ALT_SDL_RENDERER
            /* get mouse (absolute values) */
            int x;
            int state = SDL_GetMouseState(&x, NULL);

            /* Range is [0;256], 128 being middle position */
            input.analog[joynum][0] = x * 256 / sdl_video.surf_screen->w;

            /* Button I -> 0 0 0 0 0 0 0 I*/
            if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_B;
    #endif
            break;
        }

        case DEVICE_SPORTSPAD:
        {
            /* get mouse (relative values) */
            int x, y;
            int state = SDL_GetRelativeMouseState(&x, &y);

            /* Range is [0;256] */
            input.analog[joynum][0] = (unsigned char)(-x & 0xFF);
            input.analog[joynum][1] = (unsigned char)(-y & 0xFF);

            /* Buttons I & II -> 0 0 0 0 0 0 II I*/
            if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_B;
            if (state & SDL_BUTTON_RMASK) input.pad[joynum] |= INPUT_C;

            break;
        }

        case DEVICE_MOUSE:
        {
            /* get mouse (relative values) */
            int x, y;
            int state = SDL_GetRelativeMouseState(&x, &y);

            /* Sega Mouse range is [-256;+256] */
            input.analog[joynum][0] = x * 2;
            input.analog[joynum][1] = y * 2;

            /* Vertical movement is upsidedown */
            if (!config.invert_mouse)
                input.analog[joynum][1] = 0 - input.analog[joynum][1];

            /* Start,Left,Right,Middle buttons -> 0 0 0 0 START MIDDLE RIGHT LEFT */
            if (state & SDL_BUTTON_LMASK) input.pad[joynum] |= INPUT_B;
            if (state & SDL_BUTTON_RMASK) input.pad[joynum] |= INPUT_C;
            if (state & SDL_BUTTON_MMASK) input.pad[joynum] |= INPUT_A;
            if (keystate[SDL_SCANCODE_F])  input.pad[joynum] |= INPUT_START;

            break;
        }

        case DEVICE_XE_1AP:
        {
            /* A,B,C,D,Select,START,E1,E2 buttons -> E1(?) E2(?) START SELECT(?) A B C D */
            if (keystate[SDL_SCANCODE_A])  input.pad[joynum] |= INPUT_START;
            if (keystate[SDL_SCANCODE_S])  input.pad[joynum] |= INPUT_A;
            if (keystate[SDL_SCANCODE_D])  input.pad[joynum] |= INPUT_C;
            if (keystate[SDL_SCANCODE_F])  input.pad[joynum] |= INPUT_Y;
            if (keystate[SDL_SCANCODE_Z])  input.pad[joynum] |= INPUT_B;
            if (keystate[SDL_SCANCODE_X])  input.pad[joynum] |= INPUT_X;
            if (keystate[SDL_SCANCODE_C])  input.pad[joynum] |= INPUT_MODE;
            if (keystate[SDL_SCANCODE_V])  input.pad[joynum] |= INPUT_Z;

            /* Left Analog Stick (bidirectional) */
            if (keystate[SDL_SCANCODE_UP])     input.analog[joynum][1] -= 2;
            else if (keystate[SDL_SCANCODE_DOWN])   input.analog[joynum][1] += 2;
            else input.analog[joynum][1] = 128;
            if (keystate[SDL_SCANCODE_LEFT])   input.analog[joynum][0] -= 2;
            else if (keystate[SDL_SCANCODE_RIGHT])  input.analog[joynum][0] += 2;
            else input.analog[joynum][0] = 128;

            /* Right Analog Stick (unidirectional) */
            if (keystate[SDL_SCANCODE_KP_8])    input.analog[joynum + 1][0] -= 2;
            else if (keystate[SDL_SCANCODE_KP_2])   input.analog[joynum + 1][0] += 2;
            else if (keystate[SDL_SCANCODE_KP_4])   input.analog[joynum + 1][0] -= 2;
            else if (keystate[SDL_SCANCODE_KP_6])  input.analog[joynum + 1][0] += 2;
            else input.analog[joynum + 1][0] = 128;

            /* Limiters */
            if (input.analog[joynum][0] > 0xFF) input.analog[joynum][0] = 0xFF;
            else if (input.analog[joynum][0] < 0) input.analog[joynum][0] = 0;
            if (input.analog[joynum][1] > 0xFF) input.analog[joynum][1] = 0xFF;
            else if (input.analog[joynum][1] < 0) input.analog[joynum][1] = 0;
            if (input.analog[joynum + 1][0] > 0xFF) input.analog[joynum + 1][0] = 0xFF;
            else if (input.analog[joynum + 1][0] < 0) input.analog[joynum + 1][0] = 0;
            if (input.analog[joynum + 1][1] > 0xFF) input.analog[joynum + 1][1] = 0xFF;
            else if (input.analog[joynum + 1][1] < 0) input.analog[joynum + 1][1] = 0;

            break;
        }

        case DEVICE_PICO:
        {
    #ifndef ALT_SDL_RENDERER
            /* get mouse (absolute values) */
            int x, y;
            int state = SDL_GetMouseState(&x, &y);

            /* Calculate X Y axis values */
            input.analog[0][0] = 0x3c + (x * (0x17c - 0x03c + 1)) / sdl_video.surf_screen->w;
            input.analog[0][1] = 0x1fc + (y * (0x2f7 - 0x1fc + 1)) / sdl_video.surf_screen->h;

            /* Map mouse buttons to player #1 inputs */
            if (state & SDL_BUTTON_MMASK) pico_current = (pico_current + 1) & 7;
            if (state & SDL_BUTTON_RMASK) input.pad[0] |= INPUT_PICO_RED;
            if (state & SDL_BUTTON_LMASK) input.pad[0] |= INPUT_PICO_PEN;
    #endif // !ALT_SDL_RENDER
            break;
        }

        case DEVICE_TEREBI:
        {
    #ifndef ALT_SDL_RENDERER
            /* get mouse (absolute values) */
            int x, y;
            int state = SDL_GetMouseState(&x, &y);

            /* Calculate X Y axis values */
            input.analog[0][0] = (x * 250) / sdl_video.surf_screen->w;
            input.analog[0][1] = (y * 250) / sdl_video.surf_screen->h;

            /* Map mouse buttons to player #1 inputs */
            if (state & SDL_BUTTON_RMASK) input.pad[0] |= INPUT_B;
    #endif // !ALT_SDL_RENDER
            break;
        }

        case DEVICE_GRAPHIC_BOARD:
        {
    #ifndef ALT_SDL_RENDERER

            /* get mouse (absolute values) */
            int x, y;
            int state = SDL_GetMouseState(&x, &y);

            /* Calculate X Y axis values */
            input.analog[0][0] = (x * 255) / sdl_video.surf_screen->w;
            input.analog[0][1] = (y * 255) / sdl_video.surf_screen->h;

            /* Map mouse buttons to player #1 inputs */
            if (state & SDL_BUTTON_LMASK) input.pad[0] |= INPUT_GRAPHIC_PEN;
            if (state & SDL_BUTTON_RMASK) input.pad[0] |= INPUT_GRAPHIC_MENU;
            if (state & SDL_BUTTON_MMASK) input.pad[0] |= INPUT_GRAPHIC_DO;
    #endif // !ALT_SDL_RENDER
            break;
        }

        case DEVICE_ACTIVATOR:
        {
            if (keystate[SDL_SCANCODE_G])  input.pad[joynum] |= INPUT_ACTIVATOR_7L;
            if (keystate[SDL_SCANCODE_H])  input.pad[joynum] |= INPUT_ACTIVATOR_7U;
            if (keystate[SDL_SCANCODE_J])  input.pad[joynum] |= INPUT_ACTIVATOR_8L;
            if (keystate[SDL_SCANCODE_K])  input.pad[joynum] |= INPUT_ACTIVATOR_8U;
        }

        default:
        {
            
            for (int i = 0; i < eInput_COUNT; i++)
            {
                if (keystate[SDL_GetScancodeFromKey(sdlInputMapping[i].sdlKey)])
                {
                    input.pad[joynum] |= sdlInputMapping[i].gpgxKey;
                }
            }

            /*if(keystate[SDL_SCANCODE_A])
                input.pad[joynum] |= INPUT_A;
            if(keystate[SDL_SCANCODE_S])
                input.pad[joynum] |= INPUT_B;
            if(keystate[SDL_SCANCODE_D])
                input.pad[joynum] |= INPUT_C;
            if(keystate[SDL_SCANCODE_F])
                input.pad[joynum] |= INPUT_START;
            if(keystate[SDL_SCANCODE_Z])
                input.pad[joynum] |= INPUT_X;
            if(keystate[SDL_SCANCODE_X])
                input.pad[joynum] |= INPUT_Y;
            if(keystate[SDL_SCANCODE_C])
                input.pad[joynum] |= INPUT_Z;
            if(keystate[SDL_SCANCODE_V])
                input.pad[joynum] |= INPUT_MODE;

            if(keystate[SDL_SCANCODE_UP])
                input.pad[joynum] |= INPUT_UP;
            else
            if(keystate[SDL_SCANCODE_DOWN])
                input.pad[joynum] |= INPUT_DOWN;
            if(keystate[SDL_SCANCODE_LEFT])
                input.pad[joynum] |= INPUT_LEFT;
            else
            if(keystate[SDL_SCANCODE_RIGHT])
                input.pad[joynum] |= INPUT_RIGHT;*/
            
            break;
        }
    }
    return 1;
}
#pragma endregion

#pragma region Window Operations
void ShowSDLWindow()
{
    SDL_ShowWindow(sdl_video.window);
}

void HideSDLWindow()
{
    SDL_HideWindow(sdl_video.window);
}

void SetWindowPosition(int x, int y)
{
    SDL_SetWindowPosition(sdl_video.window, x, y);
}

int GetWindowXPosition()
{
    int x, y;

    SDL_GetWindowPosition(sdl_video.window, &x, &y);

    return x;
}

int GetWindowYPosition()
{
    int x, y;

    SDL_GetWindowPosition(sdl_video.window, &x, &y);

    return y;
}

void BringToFront()
{
    SDL_RaiseWindow(sdl_video.window);
}
#pragma endregion

// function prototypes for shutdown
void SaveMcdBram();
void SaveSram();

#pragma region Editor Commands for debugging

int Shutdown()
{
    running = 0;
    set_cpu_hook(NULL);
    stop_debugging();
    close_shared_mem(&dbg_req_core, 1);

    audio_shutdown();
    error_shutdown();

    sdl_video_close();
    sdl_sound_close();
    sdl_sync_close();
    //SDL_Quit(); // hmm.. calling this and relaunching causes an exception with class registration
}

int Reset()
{
    SaveMcdBram();
    SaveSram();
    system_reset();
}

void SoftReset()
{
    // doesn't exist?
    // just reset
    system_reset();
}

int GetDReg(int index)
{  
    int addrRegIndex = M68K_REG_D0 + index;
    return m68k_get_reg(addrRegIndex);
}

int GetAReg(int index)
{
    int addrRegIndex = M68K_REG_A0 + index;
    return m68k_get_reg(addrRegIndex);
}

int GetSR()
{    
    return m68k_get_reg(M68K_REG_SR);
}

int GetCurrentPC()
{
    return m68k_get_reg(M68K_REG_PC);
}

int GetZ80Reg(int index)
{
    switch (index)
    {
    case 0:
        return (int)Z80.af.d;
    case 1:
        return (int)Z80.bc.d;
    case 2:
        return (int)Z80.de.d;
    case 3:
        return (int)Z80.hl.d;
    case 4:
        return (int)Z80.af2.d;
    case 5:
        return (int)Z80.bc2.d;
    case 6:
        return (int)Z80.de2.d;
    case 7:
        return (int)Z80.hl2.d;
    case 8:
        return (int)Z80.ix.d;
    case 9:
        return (int)Z80.iy.d;
    case 10:
        return (int)Z80.sp.d;
    case 11:
        return (int)Z80.pc.d;
    }
    return 0;
}

unsigned char ReadByte(unsigned int address)
{
    // TODO:
    return 0;
}

unsigned short ReadWord(unsigned int address)
{
    // TODO:
    return 0;
}

unsigned int ReadLong(unsigned int address)
{
    return 0;
}

void ReadMemory(unsigned int address, unsigned int size, BYTE* memory)
{
    for (unsigned int i = 0; i < size; i+=2)
    {
        // work ram is byte swapped..
        memory[i] = work_ram[i+1];
        memory[i + 1] = work_ram[i];
    }
}

unsigned char ReadZ80Byte(unsigned int address)
{
    // TODO:
    return 0;
}

void SetInputMapping(int input, int mapping)
{
    sdlInputMapping[input].sdlKey = mapping;
}

int GetInputMapping(int input)
{
    return sdlInputMapping[input].sdlKey;
}

int GetPaletteEntry(int index)
{
    return 0;
}

unsigned char GetVDPRegisterValue(int index)
{
    return 0;
}

unsigned int Disassemble(unsigned int address, char* text)
{
    return 0;
}

void SetVolume(int vol, int isdebugVol)
{
}

void PauseAudio(int pause)
{
}

int AddBreakpoint(int addr)
{
    bpt_data_t* _bpt_data = &dbg_req_core->bpt_data;

    _bpt_data->enabled = 1;
    _bpt_data->address = addr;
    _bpt_data->width = 0;
    _bpt_data->type = BPT_M68K_E;

    int forceProcessRequest = 0;

    if (running == 0)
    {
        forceProcessRequest = 1;
    }

    send_dbg_request_forced(dbg_req_core, REQ_ADD_BREAK, forceProcessRequest);
    return 0;
}

void ClearBreakpoint(int addr)
{
    bpt_data_t* bpt_data = &dbg_req_core->bpt_data;
    bpt_data->address = addr;
    bpt_data->type = BPT_M68K_E;

    int forceProcessRequest = 0;

    if (running == 0)
    {
        forceProcessRequest = 1;
    }

    send_dbg_request_forced(dbg_req_core, REQ_DEL_BREAK, forceProcessRequest);
}

void ClearBreakpoints()
{
    int forceProcessRequest = 0;

    if (running == 0)
    {
        forceProcessRequest = 1;
    }

    send_dbg_request_forced(dbg_req_core, REQ_CLEAR_BREAKS, forceProcessRequest);
}

int AddWatchpoint(int fromAddr, int toAddr)
{
    // TODO: add breakpoint debugging
    return 0;
}

void ClearWatchpoint(int fromAddr)
{
    // TODO: add breakpoint debugging
}

void ClearWatchpoints()
{
    // TODO: add breakpoint debugging
}

int StepInto()
{
    if (!is_debugger_paused())
    {
        // force pause
        send_dbg_request_forced(dbg_req_core, REQ_PAUSE, 0);
    }
    else
    {
        send_dbg_request_forced(dbg_req_core, REQ_STEP_INTO, 0);
    }
    return 0;
}

int Resume()
{
    send_dbg_request_forced(dbg_req_core, REQ_RESUME, 0);
    return 0;
}

int Break()
{
    send_dbg_request_forced(dbg_req_core, REQ_PAUSE, 0);
    return 0;
}

int IsDebugging()
{    
    return is_debugger_paused() && running == 1;
}

unsigned int* GetProfilerResults(int* instructionCount)
{
    // TODO: add breakpoint debugging
    *instructionCount = 0;
    return NULL;
}

unsigned int GetInstructionCycleCount(unsigned int address)
{
    // TODO: add breakpoint debugging
    return 0;
}

unsigned char* GetVRAM()
{
    return NULL;
}

#pragma endregion

#pragma region init and update

int Update()
{
    if (is_debugger_paused())
    {
        longjmp(jmp_env, 1);
    }

    int is_paused = setjmp(jmp_env);

    if (is_paused)
    {
        process_request();
        return 0;
    }

    running = 1;

    SDL_Event event;
    if (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_USEREVENT:
        {
            char caption[100];
            sprintf(caption, "Genesis Plus GX - %d fps - %s", event.user.code, (rominfo.international[0] != 0x20) ? rominfo.international : rominfo.domestic);
            SDL_SetWindowTitle(sdl_video.window, caption);
            break;
        }

        case SDL_QUIT:
        {
            running = 0;
            break;
        }

        case SDL_KEYDOWN:
        {
            running = sdl_control_update(event.key.keysym.sym);
        }
        }
    }

    sdl_video_update();
    sdl_sound_update(use_sound);

    if (!turbo_mode && sdl_sync.sem_sync && sdl_video.frames_rendered % 3 == 0)
    {
        SDL_SemWait(sdl_sync.sem_sync);
    }

    process_request();

    return running;
}

int InitSystem()
{
    /* initialize system hardware */
    audio_init(SOUND_FREQUENCY, 0);
    system_init();

    FILE* fp;
    /* Mega CD specific */
    if (system_hw == SYSTEM_MCD)
    {
        /* load internal backup RAM */
        fp = fopen("./scd.brm", "rb");
        if (fp != NULL)
        {
            fread(scd.bram, 0x2000, 1, fp);
            fclose(fp);
        }

        /* check if internal backup RAM is formatted */
        if (memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
        {
            /* clear internal backup RAM */
            memset(scd.bram, 0x00, 0x200);

            /* Internal Backup RAM size fields */
            brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = 0x00;
            brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (sizeof(scd.bram) / 64) - 3;

            /* format internal backup RAM */
            memcpy(scd.bram + 0x2000 - 0x40, brm_format, 0x40);
        }

        /* load cartridge backup RAM */
        if (scd.cartridge.id)
        {
            fp = fopen("./cart.brm", "rb");
            if (fp != NULL)
            {
                fread(scd.cartridge.area, scd.cartridge.mask + 1, 1, fp);
                fclose(fp);
            }

            /* check if cartridge backup RAM is formatted */
            if (memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
            {
                /* clear cartridge backup RAM */
                memset(scd.cartridge.area, 0x00, scd.cartridge.mask + 1);

                /* Cartridge Backup RAM size fields */
                brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = (((scd.cartridge.mask + 1) / 64) - 3) >> 8;
                brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (((scd.cartridge.mask + 1) / 64) - 3) & 0xff;

                /* format cartridge backup RAM */
                memcpy(scd.cartridge.area + scd.cartridge.mask + 1 - sizeof(brm_format), brm_format, sizeof(brm_format));
            }
        }
    }

    if (sram.on)
    {
        /* load SRAM */
        fp = fopen("./game.srm", "rb");
        if (fp != NULL)
        {
            fread(sram.sram, 0x10000, 1, fp);
            fclose(fp);
        }
    }

    /* reset system hardware */
    system_reset();

    if (use_sound) SDL_PauseAudio(0);

    /* 3 frames = 50 ms (60hz) or 60 ms (50hz) */
    if (sdl_sync.sem_sync)
        sdl_timer_id = SDL_AddTimer(vdp_pal ? 60 : 50, sdl_sync_timer_callback, NULL);

    // TODO: Debug init
    ShowSDLWindow();
    SDL_PauseAudio(0);

    return 1;
}

int LoadRom(const char* path)
{
    load_rom(path);

    // this all relies on the results of load_rom
    // so, instead of this happing in Init, it needs
    // to happen *after* load_rom instead
    InitSystem();

    return 1;
}

void SaveMcdBram()
{
    FILE* fp;
    if (system_hw == SYSTEM_MCD)
    {
        /* save internal backup RAM (if formatted) */
        if (!memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
        {
            fp = fopen("./scd.brm", "wb");
            if (fp != NULL)
            {
                fwrite(scd.bram, 0x2000, 1, fp);
                fclose(fp);
            }
        }

        /* save cartridge backup RAM (if formatted) */
        if (scd.cartridge.id)
        {
            if (!memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
            {
                fp = fopen("./cart.brm", "wb");
                if (fp != NULL)
                {
                    fwrite(scd.cartridge.area, scd.cartridge.mask + 1, 1, fp);
                    fclose(fp);
                }
            }
        }
    }
}

void SaveSram()
{
    FILE* fp;
    if (sram.on)
    {
        /* save SRAM */
        fp = fopen("./game.srm", "wb");
        if (fp != NULL)
        {
            fwrite(sram.sram, 0x10000, 1, fp);
            fclose(fp);
        }
    }
}

int Init(int width, int height, void* parent, int pal, char region, int use_gamepad)
{
    windowHeight = height;
    windowWidth = width;

    FILE* fp;

    /* set default config */
    error_init();
    set_config_defaults();

    // init debugging
    dbg_req_core = create_shared_mem();
    start_debugging();

    set_cpu_hook(process_breakpoints);
    
    /* mark all BIOS as unloaded */ 
    system_bios = 0;

    /* Genesis BOOT ROM support (2KB max) */
    memset(boot_rom, 0xFF, 0x800);
    fp = fopen(MD_BIOS, "rb");
    if (fp != NULL)
    {
        int i;

        /* read BOOT ROM */
        fread(boot_rom, 1, 0x800, fp);
        fclose(fp);

        /* check BOOT ROM */
        if (!memcmp((char*)(boot_rom + 0x120), "GENESIS OS", 10))
        {
            /* mark Genesis BIOS as loaded */
            system_bios = SYSTEM_MD;
        }

        /* Byteswap ROM */
        for (i = 0; i < 0x800; i += 2)
        {
            uint8 temp = boot_rom[i];
            boot_rom[i] = boot_rom[i + 1];
            boot_rom[i + 1] = temp;
        }
    }

    /* initialize SDL */
    if (SDL_Init(0) < 0)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "SDL initialization failed", sdl_video.window);
        return 1;
    }
    CheckForError();
    sdl_video_init();
    if (use_sound) sdl_sound_init();
    sdl_sync_init();

    /* initialize Genesis virtual system */
#ifdef ALT_SDL_RENDERER    
    memset(&bitmap, 0, sizeof(t_bitmap));
    bitmap.width = windowWidth;
    bitmap.height = windowHeight;
    #if defined(USE_8BPP_RENDERING)
        bitmap.pitch = (bitmap.width * 1);
    #elif defined(USE_15BPP_RENDERING)
        bitmap.pitch = (bitmap.width * 2);
    #elif defined(USE_16BPP_RENDERING)
        bitmap.pitch = (bitmap.width * 2);
    #elif defined(USE_32BPP_RENDERING)
        bitmap.pitch = (bitmap.width * 4);
    #endif
    bitmap.data = (unsigned char*)malloc(bitmap.pitch * bitmap.height);;
#else
    SDL_LockSurface(sdl_video.surf_bitmap);
    memset(&bitmap, 0, sizeof(t_bitmap));
    bitmap.width = VIDEO_WIDTH;
    bitmap.height = VIDEO_HEIGHT;
#if defined(USE_8BPP_RENDERING)
    bitmap.pitch = (bitmap.width * 1);
#elif defined(USE_15BPP_RENDERING)
    bitmap.pitch = (bitmap.width * 2);
#elif defined(USE_16BPP_RENDERING)
    bitmap.pitch = (bitmap.width * 2);
#elif defined(USE_32BPP_RENDERING)
    bitmap.pitch = (bitmap.width * 4);
#endif
    bitmap.data = sdl_video.surf_bitmap->pixels;
    SDL_UnlockSurface(sdl_video.surf_bitmap);
#endif
    bitmap.viewport.changed = 3;
 
    return 0;
}

#pragma endregion