/* SDL2 implementation of the KAGE platform boundary (kage_pc_backend.h).
 *
 * Shared by the 64-bit Linux host and the Switch port: one window with a
 * desktop OpenGL compatibility context, the GLFW-shaped keyboard snapshot the
 * generated input code polls, the monotonic clock and swap control.  The game
 * keeps its own render-target and present logic (manual_kage.c); this file
 * only replaces what GLFW/WGL did on Windows. */
#include "kage_pc_backend.h"
#include "kage_vita_input.h"
#include "kage_vita_input_test_ctrl.h" /* SCE_CTRL_* bits */

#include <SDL.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void isaac_vita_log(const char *format, ...);

#ifndef KAGE_SDL_DEFAULT_SCALE
#define KAGE_SDL_DEFAULT_SCALE 1
#endif

/* GLFW key codes (glfw3.h); the snapshot is indexed by these. */
enum {
    GLFW_KEY_SPACE = 32, GLFW_KEY_APOSTROPHE = 39, GLFW_KEY_COMMA = 44,
    GLFW_KEY_MINUS = 45, GLFW_KEY_PERIOD = 46, GLFW_KEY_SLASH = 47,
    GLFW_KEY_0 = 48, GLFW_KEY_SEMICOLON = 59, GLFW_KEY_EQUAL = 61,
    GLFW_KEY_A = 65, GLFW_KEY_LEFT_BRACKET = 91, GLFW_KEY_BACKSLASH = 92,
    GLFW_KEY_RIGHT_BRACKET = 93, GLFW_KEY_GRAVE_ACCENT = 96,
    GLFW_KEY_ESCAPE = 256, GLFW_KEY_ENTER = 257, GLFW_KEY_TAB = 258,
    GLFW_KEY_BACKSPACE = 259, GLFW_KEY_INSERT = 260, GLFW_KEY_DELETE = 261,
    GLFW_KEY_RIGHT = 262, GLFW_KEY_LEFT = 263, GLFW_KEY_DOWN = 264,
    GLFW_KEY_UP = 265, GLFW_KEY_PAGE_UP = 266, GLFW_KEY_PAGE_DOWN = 267,
    GLFW_KEY_HOME = 268, GLFW_KEY_END = 269, GLFW_KEY_CAPS_LOCK = 280,
    GLFW_KEY_F1 = 290, GLFW_KEY_KP_0 = 320, GLFW_KEY_KP_ENTER = 335,
    GLFW_KEY_LEFT_SHIFT = 340, GLFW_KEY_LEFT_CONTROL = 341,
    GLFW_KEY_LEFT_ALT = 342, GLFW_KEY_RIGHT_SHIFT = 344,
    GLFW_KEY_RIGHT_CONTROL = 345, GLFW_KEY_RIGHT_ALT = 346
};

#define KAGE_SDL_STICK_DEADZONE 12000

static struct {
    int mode;
    int ready;
    int close_requested;
    SDL_Window *window;
    SDL_GLContext context;
    SDL_GameController *pad;
    uint32_t width;
    uint32_t height;
    Uint64 clock_origin;
    Uint64 clock_frequency;
    int vsync;
    unsigned vsync_changes;
    unsigned presents;
    char gl_version[128];
    char error[256];
} s_kage = { KAGE_PC_BACKEND_VISIBLE };

static void kage_sdl_error(const char *what)
{
    snprintf(s_kage.error, sizeof s_kage.error, "KAGE SDL backend: %s: %s",
             what, SDL_GetError());
}

static int glfw_key_from_scancode(SDL_Scancode code)
{
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z)
        return GLFW_KEY_A + (code - SDL_SCANCODE_A);
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9)
        return GLFW_KEY_0 + 1 + (code - SDL_SCANCODE_1);
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12)
        return GLFW_KEY_F1 + (code - SDL_SCANCODE_F1);
    if (code >= SDL_SCANCODE_KP_1 && code <= SDL_SCANCODE_KP_9)
        return GLFW_KEY_KP_0 + 1 + (code - SDL_SCANCODE_KP_1);
    switch (code) {
    case SDL_SCANCODE_0: return GLFW_KEY_0;
    case SDL_SCANCODE_KP_0: return GLFW_KEY_KP_0;
    case SDL_SCANCODE_SPACE: return GLFW_KEY_SPACE;
    case SDL_SCANCODE_APOSTROPHE: return GLFW_KEY_APOSTROPHE;
    case SDL_SCANCODE_COMMA: return GLFW_KEY_COMMA;
    case SDL_SCANCODE_MINUS: return GLFW_KEY_MINUS;
    case SDL_SCANCODE_PERIOD: return GLFW_KEY_PERIOD;
    case SDL_SCANCODE_SLASH: return GLFW_KEY_SLASH;
    case SDL_SCANCODE_SEMICOLON: return GLFW_KEY_SEMICOLON;
    case SDL_SCANCODE_EQUALS: return GLFW_KEY_EQUAL;
    case SDL_SCANCODE_LEFTBRACKET: return GLFW_KEY_LEFT_BRACKET;
    case SDL_SCANCODE_BACKSLASH: return GLFW_KEY_BACKSLASH;
    case SDL_SCANCODE_RIGHTBRACKET: return GLFW_KEY_RIGHT_BRACKET;
    case SDL_SCANCODE_GRAVE: return GLFW_KEY_GRAVE_ACCENT;
    case SDL_SCANCODE_ESCAPE: return GLFW_KEY_ESCAPE;
    case SDL_SCANCODE_RETURN: return GLFW_KEY_ENTER;
    case SDL_SCANCODE_KP_ENTER: return GLFW_KEY_KP_ENTER;
    case SDL_SCANCODE_TAB: return GLFW_KEY_TAB;
    case SDL_SCANCODE_BACKSPACE: return GLFW_KEY_BACKSPACE;
    case SDL_SCANCODE_INSERT: return GLFW_KEY_INSERT;
    case SDL_SCANCODE_DELETE: return GLFW_KEY_DELETE;
    case SDL_SCANCODE_RIGHT: return GLFW_KEY_RIGHT;
    case SDL_SCANCODE_LEFT: return GLFW_KEY_LEFT;
    case SDL_SCANCODE_DOWN: return GLFW_KEY_DOWN;
    case SDL_SCANCODE_UP: return GLFW_KEY_UP;
    case SDL_SCANCODE_PAGEUP: return GLFW_KEY_PAGE_UP;
    case SDL_SCANCODE_PAGEDOWN: return GLFW_KEY_PAGE_DOWN;
    case SDL_SCANCODE_HOME: return GLFW_KEY_HOME;
    case SDL_SCANCODE_END: return GLFW_KEY_END;
    case SDL_SCANCODE_CAPSLOCK: return GLFW_KEY_CAPS_LOCK;
    case SDL_SCANCODE_LSHIFT: return GLFW_KEY_LEFT_SHIFT;
    case SDL_SCANCODE_LCTRL: return GLFW_KEY_LEFT_CONTROL;
    case SDL_SCANCODE_LALT: return GLFW_KEY_LEFT_ALT;
    case SDL_SCANCODE_RSHIFT: return GLFW_KEY_RIGHT_SHIFT;
    case SDL_SCANCODE_RCTRL: return GLFW_KEY_RIGHT_CONTROL;
    case SDL_SCANCODE_RALT: return GLFW_KEY_RIGHT_ALT;
    default: return -1;
    }
}

/* Controller -> the game's default keyboard bindings: left stick/d-pad move
 * (WASD), face buttons shoot (arrows), RB item (Space), LB bomb (E), RT card
 * (Q), LT drop (Left Ctrl), Back map (Tab), Start pause (Esc), right-stick
 * click confirm (Enter). */
/* Gamepad for the game's own controller path: host_vita_xinput.c presents a
 * synthetic XInput module and builds XINPUT_STATE from this snapshot (Vita
 * button bits, sticks 0..255 with 128 neutral and Y growing downward).  The
 * buttons are positional, as on an Xbox pad: bottom face button is XInput A
 * (confirm, shoot down), right is B (back, shoot right), left is X, top is Y.
 * Shoulders: L bomb (LB), R pill/card (RB), ZL/LT active item, ZR/RT drop,
 * -/Back map, +/Start pause.  Once the game owns the pad, the keyboard
 * translation below is off so no button fires twice. */
static int s_xinput_active;
static uint32_t s_pad_packet;
static kage_vita_gamepad_snapshot s_pad_last;

void kage_vita_input_set_xinput_active(int active)
{
    s_xinput_active = active ? 1 : 0;
}

static uint8_t kage_sdl_axis_byte(int value, int invert)
{
    int v = invert ? -value : value;

    v = (v + 32768) >> 8;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

#if defined(__SWITCH__)
static PadState s_switch_pad;
static int s_switch_pad_ready;

static int kage_sdl_pad_read(kage_vita_gamepad_snapshot *out)
{
    u64 held;
    HidAnalogStickState ls, rs;

    if (!s_switch_pad_ready) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&s_switch_pad);
        s_switch_pad_ready = 1;
    }
    padUpdate(&s_switch_pad);
    if (!padIsConnected(&s_switch_pad))
        return 0;
    held = padGetButtons(&s_switch_pad);
    ls = padGetStickPos(&s_switch_pad, 0);
    rs = padGetStickPos(&s_switch_pad, 1);
    if (held & HidNpadButton_Up) out->buttons |= SCE_CTRL_UP;
    if (held & HidNpadButton_Down) out->buttons |= SCE_CTRL_DOWN;
    if (held & HidNpadButton_Left) out->buttons |= SCE_CTRL_LEFT;
    if (held & HidNpadButton_Right) out->buttons |= SCE_CTRL_RIGHT;
    if (held & HidNpadButton_B) out->buttons |= SCE_CTRL_CROSS;
    if (held & HidNpadButton_A) out->buttons |= SCE_CTRL_CIRCLE;
    if (held & HidNpadButton_Y) out->buttons |= SCE_CTRL_SQUARE;
    if (held & HidNpadButton_X) out->buttons |= SCE_CTRL_TRIANGLE;
    if (held & HidNpadButton_L) out->buttons |= SCE_CTRL_LTRIGGER;
    if (held & HidNpadButton_ZR) out->buttons |= SCE_CTRL_SELECT;
    if (held & HidNpadButton_Plus) out->buttons |= SCE_CTRL_START;
    if (held & HidNpadButton_R)
        out->xinput_buttons |= KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER;
    if (held & HidNpadButton_Minus)
        out->xinput_buttons |= KAGE_VITA_INPUT_XINPUT_BACK;
    if (held & HidNpadButton_ZL)
        out->xinput_left_trigger = KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL;
    /* libnx sticks: +y is up. */
    out->lx = kage_sdl_axis_byte(ls.x, 0);
    out->ly = kage_sdl_axis_byte(ls.y, 1);
    out->rx = kage_sdl_axis_byte(rs.x, 0);
    out->ry = kage_sdl_axis_byte(rs.y, 1);
    return 1;
}
#else
static int kage_sdl_pad_read(kage_vita_gamepad_snapshot *out)
{
    SDL_GameController *pad = s_kage.pad;

    if (!pad)
        return 0;
#define PAD(b) SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_##b)
#define AXIS(a) SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_##a)
    if (PAD(DPAD_UP)) out->buttons |= SCE_CTRL_UP;
    if (PAD(DPAD_DOWN)) out->buttons |= SCE_CTRL_DOWN;
    if (PAD(DPAD_LEFT)) out->buttons |= SCE_CTRL_LEFT;
    if (PAD(DPAD_RIGHT)) out->buttons |= SCE_CTRL_RIGHT;
    if (PAD(A)) out->buttons |= SCE_CTRL_CROSS;
    if (PAD(B)) out->buttons |= SCE_CTRL_CIRCLE;
    if (PAD(X)) out->buttons |= SCE_CTRL_SQUARE;
    if (PAD(Y)) out->buttons |= SCE_CTRL_TRIANGLE;
    if (PAD(LEFTSHOULDER)) out->buttons |= SCE_CTRL_LTRIGGER;
    if (AXIS(TRIGGERRIGHT) > 16000) out->buttons |= SCE_CTRL_SELECT;
    if (PAD(START)) out->buttons |= SCE_CTRL_START;
    if (PAD(RIGHTSHOULDER))
        out->xinput_buttons |= KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER;
    if (PAD(BACK))
        out->xinput_buttons |= KAGE_VITA_INPUT_XINPUT_BACK;
    if (AXIS(TRIGGERLEFT) > 16000)
        out->xinput_left_trigger = KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL;
    /* SDL sticks: +y is down, as on the Vita. */
    out->lx = kage_sdl_axis_byte(AXIS(LEFTX), 0);
    out->ly = kage_sdl_axis_byte(AXIS(LEFTY), 0);
    out->rx = kage_sdl_axis_byte(AXIS(RIGHTX), 0);
    out->ry = kage_sdl_axis_byte(AXIS(RIGHTY), 0);
#undef PAD
#undef AXIS
    return 1;
}
#endif

int kage_vita_input_gamepad_snapshot(kage_vita_gamepad_snapshot *snapshot)
{
    kage_vita_gamepad_snapshot now;

    if (!snapshot || !s_kage.ready)
        return 0;
    memset(&now, 0, sizeof now);
    if (!kage_sdl_pad_read(&now))
        return 0;
    now.packet_number = s_pad_last.packet_number;
    if (memcmp(&now, &s_pad_last, sizeof now) != 0)
        now.packet_number = ++s_pad_packet;
    s_pad_last = now;
    *snapshot = now;
    return 1;
}

static void kage_sdl_pad_keys(uint8_t keys[KAGE_PC_KEY_COUNT])
{
    SDL_GameController *pad = s_kage.pad;
    int lx, ly;

    if (!pad || s_xinput_active)
        return;
#define PAD(b) SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_##b)
#define AXIS(a) SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_##a)
    lx = AXIS(LEFTX);
    ly = AXIS(LEFTY);
    if (PAD(DPAD_UP) || ly < -KAGE_SDL_STICK_DEADZONE)
        keys['W'] = 1;
    if (PAD(DPAD_DOWN) || ly > KAGE_SDL_STICK_DEADZONE)
        keys['S'] = 1;
    if (PAD(DPAD_LEFT) || lx < -KAGE_SDL_STICK_DEADZONE)
        keys['A'] = 1;
    if (PAD(DPAD_RIGHT) || lx > KAGE_SDL_STICK_DEADZONE)
        keys['D'] = 1;
    if (PAD(Y))
        keys[GLFW_KEY_UP] = 1;
    if (PAD(A))
        keys[GLFW_KEY_DOWN] = 1;
    if (PAD(X))
        keys[GLFW_KEY_LEFT] = 1;
    if (PAD(B))
        keys[GLFW_KEY_RIGHT] = 1;
    if (PAD(RIGHTSHOULDER))
        keys[GLFW_KEY_SPACE] = 1;
    if (PAD(LEFTSHOULDER))
        keys['E'] = 1;
    if (AXIS(TRIGGERRIGHT) > 16000)
        keys['Q'] = 1;
    if (AXIS(TRIGGERLEFT) > 16000)
        keys[GLFW_KEY_LEFT_CONTROL] = 1;
    if (PAD(BACK))
        keys[GLFW_KEY_TAB] = 1;
    if (PAD(START))
        keys[GLFW_KEY_ESCAPE] = 1;
    if (PAD(RIGHTSTICK))
        keys[GLFW_KEY_ENTER] = 1;
#undef PAD
#undef AXIS
}

static void kage_sdl_pump(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            s_kage.close_requested = 1;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!s_kage.pad)
                s_kage.pad = SDL_GameControllerOpen(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (s_kage.pad &&
                event.cdevice.which ==
                    SDL_JoystickInstanceID(
                        SDL_GameControllerGetJoystick(s_kage.pad))) {
                SDL_GameControllerClose(s_kage.pad);
                s_kage.pad = NULL;
            }
            break;
        default:
            break;
        }
    }
}

int kage_pc_backend_set_mode(int mode)
{
    if (mode != KAGE_PC_BACKEND_DISABLED && mode != KAGE_PC_BACKEND_HIDDEN &&
        mode != KAGE_PC_BACKEND_VISIBLE)
        return 0;
    s_kage.mode = mode;
    return 1;
}

int kage_pc_backend_mode(void) { return s_kage.mode; }

int kage_pc_backend_initialize(uint32_t width, uint32_t height)
{
    const char *size = getenv("ISAAC_WINDOW");
    int window_w = (int)width * KAGE_SDL_DEFAULT_SCALE;
    int window_h = (int)height * KAGE_SDL_DEFAULT_SCALE;
    int drawable_w, drawable_h;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    const unsigned char *version;
    const char *(*get_string)(unsigned);

    if (s_kage.ready)
        return 1;
    if (size)
        sscanf(size, "%dx%d", &window_w, &window_h);
#if defined(__SWITCH__)
    /* Full screen; SDL scales to the docked output. */
    window_w = 1280;
    window_h = 720;
    flags = SDL_WINDOW_OPENGL;
#endif
    if (s_kage.mode == KAGE_PC_BACKEND_HIDDEN)
        flags |= SDL_WINDOW_HIDDEN;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        kage_sdl_error("SDL_Init");
        return 0;
    }
    /* The game is written against a legacy (compatibility) context: client
     * vertex arrays, glAlphaFunc, GLSL 1.10/1.20. */
#if defined(__SWITCH__)
    /* The Switch SDL port defaults to an OpenGL ES context; libepoxy in the
     * game then finds no desktop GL provider and aborts. Mesa on the Switch
     * also offers desktop GL, so ask for it explicitly. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    s_kage.window = SDL_CreateWindow(
        "The Binding of Isaac: Repentance", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, window_w, window_h, flags);
    if (!s_kage.window) {
        kage_sdl_error("SDL_CreateWindow");
        SDL_Quit();
        return 0;
    }
    s_kage.context = SDL_GL_CreateContext(s_kage.window);
    if (!s_kage.context) {
        kage_sdl_error("SDL_GL_CreateContext");
        SDL_DestroyWindow(s_kage.window);
        s_kage.window = NULL;
        SDL_Quit();
        return 0;
    }
    SDL_GL_MakeCurrent(s_kage.window, s_kage.context);
    SDL_GL_SetSwapInterval(0);
    s_kage.vsync = 0;
    SDL_GL_GetDrawableSize(s_kage.window, &drawable_w, &drawable_h);
    s_kage.width = (uint32_t)drawable_w;
    s_kage.height = (uint32_t)drawable_h;

    get_string = (const char *(*)(unsigned))SDL_GL_GetProcAddress(
        "glGetString");
    version = get_string ? (const unsigned char *)get_string(0x1f02u) : NULL;
    snprintf(s_kage.gl_version, sizeof s_kage.gl_version, "%s",
             version ? (const char *)version : "unknown");
    if (get_string && get_string(0x1f01u)) {
        size_t used = strlen(s_kage.gl_version);
        snprintf(s_kage.gl_version + used, sizeof s_kage.gl_version - used,
                 " / %s", (const char *)get_string(0x1f01u));
    }

    if (SDL_NumJoysticks() > 0) {
        int i;
        for (i = 0; i < SDL_NumJoysticks() && !s_kage.pad; ++i)
            if (SDL_IsGameController(i))
                s_kage.pad = SDL_GameControllerOpen(i);
    }
    s_kage.clock_frequency = SDL_GetPerformanceFrequency();
    s_kage.clock_origin = SDL_GetPerformanceCounter();
    s_kage.close_requested = 0;
    s_kage.presents = 0u;
    s_kage.ready = 1;
    fprintf(stderr, "KAGE SDL READY: window=%dx%d drawable=%ux%u GL=%s\n",
            window_w, window_h, (unsigned)s_kage.width,
            (unsigned)s_kage.height, s_kage.gl_version);
    return 1;
}

void kage_pc_backend_shutdown(void)
{
    if (s_kage.pad)
        SDL_GameControllerClose(s_kage.pad);
    if (s_kage.context)
        SDL_GL_DeleteContext(s_kage.context);
    if (s_kage.window)
        SDL_DestroyWindow(s_kage.window);
    if (s_kage.ready)
        SDL_Quit();
    s_kage.pad = NULL;
    s_kage.context = NULL;
    s_kage.window = NULL;
    s_kage.ready = 0;
}

#if defined(__SWITCH__)
/* Per-window time split (performance counter ticks): render, frame limiter
 * (idle wait), buffer swap.  The rest of the frame is game update. */
static Uint64 s_t_render, s_t_limiter, s_t_swap, s_mark_render, s_mark_limiter;

/* main_switch.c raises the CPU clock for the initial load; drop it once the
 * game renders steadily (boost mode lowers the GPU clock).  Also log the
 * frame rate now and then. */
static void kage_sdl_switch_frame_note(void)
{
    static Uint64 last, window_start;
    static unsigned steady, window_frames;
    static int boosted = 1;
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();

    if (boosted) {
        if (last && now - last < freq / 10u)
            ++steady;
        else
            steady = 0;
        if (steady >= 60u) {
            appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
            boosted = 0;
            isaac_vita_log("isaac-switch: steady frames, CPU boost off");
        }
    }
    last = now;
    if (!window_start)
        window_start = now;
    if (++window_frames == 600u) {
        double seconds = (double)(now - window_start) / (double)freq;
        double ms = 1000.0 / (double)freq / (double)window_frames;
        isaac_vita_log("isaac-switch: %.1f fps over %u frames; per frame "
                       "render %.2f ms, limiter %.2f ms, swap %.2f ms, "
                       "total %.2f ms",
                       seconds > 0.0 ? window_frames / seconds : 0.0,
                       window_frames, (double)s_t_render * ms,
                       (double)s_t_limiter * ms, (double)s_t_swap * ms,
                       (double)(now - window_start) * ms);
        s_t_render = s_t_limiter = s_t_swap = 0u;
        window_start = now;
        window_frames = 0u;
    }
}
#endif

int kage_pc_backend_present(void)
{
    int w, h;

    if (!s_kage.ready) {
        snprintf(s_kage.error, sizeof s_kage.error,
                 "KAGE SDL backend: present without a context");
        return 0;
    }
#if defined(__SWITCH__)
    {
        Uint64 t0 = SDL_GetPerformanceCounter();
        SDL_GL_SwapWindow(s_kage.window);
        s_t_swap += SDL_GetPerformanceCounter() - t0;
    }
#else
    SDL_GL_SwapWindow(s_kage.window);
#endif
    kage_sdl_pump();
    SDL_GL_GetDrawableSize(s_kage.window, &w, &h);
    s_kage.width = (uint32_t)w;
    s_kage.height = (uint32_t)h;
    s_kage.presents++;
#if defined(__SWITCH__)
    kage_sdl_switch_frame_note();
#endif
    if (s_kage.close_requested) {
        snprintf(s_kage.error, sizeof s_kage.error,
                 "KAGE SDL backend: window close requested");
        return 0;
    }
    return 1;
}

void kage_pc_backend_note_loop_head(void) {}
void kage_pc_backend_note_service_entry(void) {}
void kage_pc_backend_note_update_entry(void) {}
void kage_pc_backend_audio_cooperative_poll(struct CPU *cpu) { (void)cpu; }
void kage_pc_backend_note_render_entry(void)
{
#if defined(__SWITCH__)
    s_mark_render = SDL_GetPerformanceCounter();
#endif
}
void kage_pc_backend_note_render_return(void)
{
#if defined(__SWITCH__)
    if (s_mark_render)
        s_t_render += SDL_GetPerformanceCounter() - s_mark_render;
    s_mark_render = 0u;
#endif
}
void kage_pc_backend_note_limiter_entry(void)
{
#if defined(__SWITCH__)
    s_mark_limiter = SDL_GetPerformanceCounter();
#endif
}
void kage_pc_backend_note_limiter_exit(void)
{
#if defined(__SWITCH__)
    if (s_mark_limiter)
        s_t_limiter += SDL_GetPerformanceCounter() - s_mark_limiter;
    s_mark_limiter = 0u;
#endif
}
void kage_pc_backend_note_menu_init(uint32_t site) { (void)site; }
void kage_pc_backend_note_menu_render(uint32_t site) { (void)site; }
void kage_pc_backend_note_menu_character(uint32_t site) { (void)site; }

void kage_pc_backend_note_menu_character_wheel_snapshot(
    uint32_t this_ptr, uint32_t character_count,
    uint32_t scratch_begin, uint32_t scratch_end,
    uint32_t scratch_capacity, uint32_t guard)
{
    (void)this_ptr;
    (void)character_count;
    (void)scratch_begin;
    (void)scratch_end;
    (void)scratch_capacity;
    (void)guard;
}

void kage_pc_backend_keyboard_snapshot(uint8_t keys[KAGE_PC_KEY_COUNT])
{
    const Uint8 *state;
    int count = 0;
    int code;

    if (!keys)
        return;
    memset(keys, 0, KAGE_PC_KEY_COUNT);
    if (!s_kage.ready)
        return;
    SDL_PumpEvents();
    state = SDL_GetKeyboardState(&count);
    for (code = 0; code < count; ++code) {
        int key;
        if (!state[code])
            continue;
        key = glfw_key_from_scancode((SDL_Scancode)code);
        if (key >= 0 && key < (int)KAGE_PC_KEY_COUNT)
            keys[key] = 1;
    }
    kage_sdl_pad_keys(keys);
}

int kage_pc_backend_console_trace_enabled(void) { return 0; }
int kage_pc_backend_note_console_command(uint32_t guest_stack)
{
    (void)guest_stack;
    return 0;
}

int kage_pc_backend_virtual_clipboard_open(uintptr_t owner, uint32_t *result)
{
    (void)owner;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_clipboard_get_data(
    uint32_t format, uintptr_t *result)
{
    (void)format;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_global_lock(
    uintptr_t handle, uintptr_t *result)
{
    (void)handle;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_global_unlock(
    uintptr_t handle, uint32_t *result)
{
    (void)handle;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_clipboard_close(uint32_t *result)
{
    (void)result;
    return 0;
}

void kage_pc_backend_note_guest_gl_call(const char *name) { (void)name; }

int kage_pc_backend_time_seconds(double *seconds)
{
    if (!seconds || !s_kage.clock_frequency) {
        snprintf(s_kage.error, sizeof s_kage.error,
                 "KAGE SDL backend: clock requested before initialize");
        return 0;
    }
    *seconds = (double)(SDL_GetPerformanceCounter() - s_kage.clock_origin) /
               (double)s_kage.clock_frequency;
    return 1;
}

int kage_pc_backend_set_vsync(int enabled)
{
    if (!s_kage.ready)
        return 0;
    if (SDL_GL_SetSwapInterval(enabled ? 1 : 0) != 0) {
        kage_sdl_error("SDL_GL_SetSwapInterval");
        return 0;
    }
    if ((enabled != 0) != s_kage.vsync)
        s_kage.vsync_changes++;
    s_kage.vsync = enabled != 0;
    return 1;
}

int kage_pc_backend_vsync_enabled(void) { return s_kage.vsync; }

uint32_t kage_pc_backend_refresh_rate(void)
{
    SDL_DisplayMode mode;
    int display;

    if (!s_kage.ready)
        return 0u;
    display = SDL_GetWindowDisplayIndex(s_kage.window);
    if (display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0 &&
        mode.refresh_rate > 0)
        return (uint32_t)mode.refresh_rate;
    return 60u;
}

unsigned kage_pc_backend_vsync_change_count(void)
{
    return s_kage.vsync_changes;
}
int kage_pc_backend_ready(void) { return s_kage.ready; }
uint32_t kage_pc_backend_width(void) { return s_kage.width; }
uint32_t kage_pc_backend_height(void) { return s_kage.height; }
const char *kage_pc_backend_last_error(void)
{
    return s_kage.error[0] ? s_kage.error : "KAGE SDL backend: no error";
}
const char *kage_pc_backend_gl_version(void) { return s_kage.gl_version; }
unsigned kage_pc_backend_clear_count(void) { return 0u; }
unsigned kage_pc_backend_present_count(void) { return s_kage.presents; }
int kage_pc_backend_accelerated(void) { return s_kage.ready; }

void kage_pc_backend_probe_rgba(uint8_t rgba[4])
{
    if (rgba)
        memset(rgba, 0, 4u);
}

uintptr_t kage_pc_backend_gl_proc(const char *name)
{
    if (!s_kage.ready || !name)
        return (uintptr_t)0;
    return (uintptr_t)SDL_GL_GetProcAddress(name);
}
