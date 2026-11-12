#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

// Menu IDs
#define ID_2X_SPEED        1001
#define ID_1X_SPEED        1002
#define ID_CTRL_SETUP      2001

// GBA Button Remap IDs
#define ID_CTRL_REMAP_A        3001
#define ID_CTRL_REMAP_B        3002
#define ID_CTRL_REMAP_L        3003
#define ID_CTRL_REMAP_R        3004
#define ID_CTRL_REMAP_START    3005
#define ID_CTRL_REMAP_SELECT   3006
#define ID_CTRL_REMAP_UP       3007
#define ID_CTRL_REMAP_DOWN     3008
#define ID_CTRL_REMAP_LEFT     3009
#define ID_CTRL_REMAP_RIGHT    3010

#define ID_ROM_OPEN        4001

#include "arm.h"
#include "arm_mem.h"
#include "io.h"
#include "video.h"
#include "bios.h"

#define GBA_WIDTH  240
#define GBA_HEIGHT 160
#define SCALE      3   // Window will open at 720x480


// Controller Mappings best stored as enum eg.

typedef enum {
    GBA_BIND_A = 0,
    GBA_BIND_B,
    GBA_BIND_L,
    GBA_BIND_R,
    GBA_BIND_START,
    GBA_BIND_SELECT,
    GBA_BIND_UP,
    GBA_BIND_DOWN,
    GBA_BIND_LEFT,
    GBA_BIND_RIGHT,
    GBA_BIND_COUNT // 10
} GbaBindIndex;

// Default bindings
static int pad_mappings[GBA_BIND_COUNT] = {
    SDL_CONTROLLER_BUTTON_A,             // GBA A
    SDL_CONTROLLER_BUTTON_B,             // GBA B
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  // GBA L
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, // GBA R
    SDL_CONTROLLER_BUTTON_START,         // Start
    SDL_CONTROLLER_BUTTON_BACK,          // Select
    SDL_CONTROLLER_BUTTON_DPAD_UP,       // Up
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,     // Down
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,     // Left
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT     // Right
};

static const char *pad_bind_keys[GBA_BIND_COUNT] = {
    "btn_a", "btn_b", "btn_l", "btn_r", "btn_start",
    "btn_select", "btn_up", "btn_down", "btn_left", "btn_right"
};

static const char *pad_bind_labels[GBA_BIND_COUNT] = {
    "A", "B", "L (Shoulder)", "R (Shoulder)", "Start",
    "Select", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"
};

// State for active remapping
static bool is_remapping = false;
static int remap_target_index = -1;

// Active gamepad pointer
static SDL_GameController *active_controller = NULL;

// Saves each of the 10 mappings to controller_config.txt
static void save_controller_mapping(void) {
    FILE *f = fopen("controller_config.txt", "w");
    if (!f) {
        printf("[Config] Warning: Could not save controller_config.txt\n");
        return;
    }

    for (int i = 0; i < GBA_BIND_COUNT; i++) {
        fprintf(f, "%s=%d\n", pad_bind_keys[i], pad_mappings[i]);
    }

    fclose(f);
    printf("[Config] Controller mappings saved to controller_config.txt\n");
}

// Loads the mappings from controller_config.txt if the file exists
static void load_controller_mapping(void) {
    FILE *f = fopen("controller_config.txt", "r");
    if (!f) {
        printf("[Config] No controller_config.txt found. Using default mappings.\n");
        return;
    }

    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char key[32];
        int val = -1;
        if (sscanf(line, "%31[^=]=%d", key, &val) == 2) {
            for (int i = 0; i < GBA_BIND_COUNT; i++) {
                if (strcmp(key, pad_bind_keys[i]) == 0) {
                    pad_mappings[i] = val;
                    break;
                }
            }
        }
    }

    fclose(f);
    printf("[Config] Loaded custom controller mappings from controller_config.txt\n");
}

#ifdef _WIN32
// searches for connected gamepads and opens the first one
static void configure_controller(HWND hwnd) {
    int count = SDL_NumJoysticks();

    if (count <= 0) {
        MessageBox(hwnd,
            "No game controllers detected!\n\n"
            "Please plug in an Xbox, PlayStation, or USB gamepad and try again.\n\n"
            "(In the meantime, keyboard controls WASD/Arrows work!)",
            "Controller Setup",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (active_controller) {
        SDL_GameControllerClose(active_controller);
        active_controller = NULL;
    }

    for (int i = 0; i < count; i++) {
        if (SDL_IsGameController(i)) {
            active_controller = SDL_GameControllerOpen(i);
            if (active_controller) break;
        }
    }

    if (active_controller) {
        const char *name = SDL_GameControllerName(active_controller);
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Controller Connected!\n\n"
            "Device: %s\n\n"
            "Status: Active and ready to play.\n"
            "Use 'Remap Buttons' to customize bindings.",
            name ? name : "Gamepad");

        printf("[Controller] Connected to: %s\n", name ? name : "Gamepad");
        MessageBox(hwnd, msg, "Controller Connected", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBox(hwnd,
            "Found USB device, but SDL could not open it as a standard game controller.",
            "Controller Setup",
            MB_OK | MB_ICONERROR);
    }
}

// Triggers remapping mode for a specific GBA button
static void start_remap_button(SDL_Window *window, HWND hwnd, int bind_index) {
    (void)hwnd;
    remap_target_index = bind_index;
    is_remapping = true;

    char title[128];
    snprintf(title, sizeof(title), "GMU GBA - [PRESS ANY CONTROLLER BUTTON FOR %s]",
             pad_bind_labels[bind_index]);
    SDL_SetWindowTitle(window, title);

    printf("[Remap] Waiting for controller input for GBA %s...\n",
           pad_bind_labels[bind_index]);
}

// Create the native Windows menu bar
static void create_menu(SDL_Window *window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) return;

    HWND hwnd = wmInfo.info.win.window;

    HMENU hMenuBar   = CreateMenu();
    HMENU hEmuMenu   = CreateMenu();
    HMENU hCtrlMenu  = CreateMenu();
    HMENU hRemapMenu = CreateMenu();
    HMENU hRomMenu   = CreateMenu();

    
    AppendMenu(hEmuMenu, MF_STRING, ID_1X_SPEED, "&1x Speed");
    AppendMenu(hEmuMenu, MF_STRING, ID_2X_SPEED, "&2x Speed");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hEmuMenu, "&Speed");

    
    AppendMenu(hCtrlMenu, MF_STRING, ID_CTRL_SETUP, "&Configure Controller...");
    AppendMenu(hCtrlMenu, MF_SEPARATOR, 0, NULL);

    // Remapping inside submenu
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_A, "&A");
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_B, "&B");
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_L, "&L (Shoulder)");
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_R, "&R (Shoulder)");
    AppendMenu(hRemapMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_START,  "&Start");
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_SELECT, "&Select");
    AppendMenu(hRemapMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_UP,    "D-Pad &Up");
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_DOWN,  "D-Pad &Down");
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_LEFT,  "D-Pad &Left");
    AppendMenu(hRemapMenu, MF_STRING, ID_CTRL_REMAP_RIGHT, "D-Pad &Right");

    AppendMenu(hCtrlMenu, MF_POPUP, (UINT_PTR)hRemapMenu, "&Remap Buttons");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hCtrlMenu, "&Controller");

    // 3. ROM Selection Menu
    AppendMenu(hRomMenu, MF_STRING, ID_ROM_OPEN, "&Open ROM...");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hRomMenu, "&Rom");

    SetMenu(hwnd, hMenuBar);
}
#endif

// Round ROM size up to the next power of 2 for memory mirroring mask
static uint32_t next_pow2(uint32_t val) {
    val--;
    val |= (val >> 1);
    val |= (val >> 2);
    val |= (val >> 4);
    val |= (val >> 8);
    val |= (val >> 16);
    return val + 1;
}

// Load the Game Boy Advance ROM into memory
static bool load_rom(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Could not open ROM file '%s'\n", filename);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fprintf(stderr, "Error: ROM file is empty.\n");
        fclose(f);
        return false;
    }

    cart_rom_size = (size > 0x2000000) ? 0x2000000 : size;
    cart_rom_mask = next_pow2(cart_rom_size) - 1;

    memset(rom, 0, 0x2000000);
    fread(rom, 1, cart_rom_size, f);
    fclose(f);

    printf("Loaded ROM: %s (%ld KB)\n", filename, size / 1024);
    return true;
}

// Load optional 16 KB official GBA BIOS
static bool load_bios(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        return false;
    }

    memset(bios, 0, 16384);
    size_t bytes = fread(bios, 1, 16384, f);
    fclose(f);

    if (bytes >= 16384) {
        printf("Loaded BIOS: %s\n", filename);
        return true;
    }
    return false;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("====================================================\n");
        printf("  GMU GBA - Game Boy Advance Emulator (In C!)       \n");
        printf("====================================================\n");
        printf("Usage: %s <rom.gba> [bios.bin]\n\n", argv[0]);
        printf("Controls:\n");
        printf("  D-Pad:  Arrow keys or WASD\n");
        printf("  A / B:  Z / X\n");
        printf("  L / R:  Q / E\n");
        printf("  Start:  Enter\n");
        printf("  Select: Shift or Backspace\n");
        printf("  Quit:   Escape\n");
        return 1;
    }

    const char *rom_path = argv[1];
    const char *bios_path = (argc >= 3) ? argv[2] : "gba_bios.bin";

    
    arm_init();
    bios_init();

   
    if (!load_rom(rom_path)) {
        arm_uninit();
        return 1;
    }

    )
    bool has_bios = load_bios(bios_path);
    key_input.w = 0x03ff; // All buttons unpressed (active-low)
    int_halt = false;

    if (has_bios) {
        arm_reset();
    } else {
        printf("No BIOS found; booting directly via High-Level Emulation (HLE).\n");
        bios_direct_boot();
    }

    load_controller_mapping();

   
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        arm_uninit();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "GMU GBA",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        GBA_WIDTH * SCALE,
        GBA_HEIGHT * SCALE,
        SDL_WINDOW_SHOWN
    );

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    // 32-bit streaming texture for the 240x160 GBA framebuffer
    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        GBA_WIDTH,
        GBA_HEIGHT
    );

    // Auto-open first connected controller if present
    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; i++) {
        if (SDL_IsGameController(i)) {
            active_controller = SDL_GameControllerOpen(i);
            if (active_controller) {
                printf("[Controller] Auto-detected: %s\n", SDL_GameControllerName(active_controller));
                break;
            }
        }
    }

#ifdef _WIN32
    // Attach the menu bar and enable Windows menu messages
    create_menu(window);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif

    // 6. Main Emulation Loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }

            // Controller Button Pressed
            else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                int btn = event.cbutton.button;

                // If currently in Remapping Mode grab input and store
                if (is_remapping && remap_target_index >= 0) {
                    pad_mappings[remap_target_index] = btn;
                    printf("[Remap] GBA %s is now mapped to controller button %d!\n",
                           pad_bind_labels[remap_target_index], btn);

                    // Save immediately to controller_config.txt
                    save_controller_mapping();

                    // Reset title and state
                    SDL_SetWindowTitle(window, "GMU GBA");
                    is_remapping = false;
                    remap_target_index = -1;
                    continue;
                }

                // Normal Gameplay Mapping
                if      (btn == pad_mappings[GBA_BIND_A])      key_input.w &= ~BTN_A;
                else if (btn == pad_mappings[GBA_BIND_B])      key_input.w &= ~BTN_B;
                else if (btn == pad_mappings[GBA_BIND_L])      key_input.w &= ~BTN_LT;
                else if (btn == pad_mappings[GBA_BIND_R])      key_input.w &= ~BTN_RT;
                else if (btn == pad_mappings[GBA_BIND_START])  key_input.w &= ~BTN_STA;
                else if (btn == pad_mappings[GBA_BIND_SELECT]) key_input.w &= ~BTN_SEL;
                else if (btn == pad_mappings[GBA_BIND_UP])     key_input.w &= ~BTN_U;
                else if (btn == pad_mappings[GBA_BIND_DOWN])   key_input.w &= ~BTN_D;
                else if (btn == pad_mappings[GBA_BIND_LEFT])   key_input.w &= ~BTN_L;
                else if (btn == pad_mappings[GBA_BIND_RIGHT])  key_input.w &= ~BTN_R;
            }

            // Controller Button Released
            else if (event.type == SDL_CONTROLLERBUTTONUP) {
                if (is_remapping) continue;

                int btn = event.cbutton.button;
                if      (btn == pad_mappings[GBA_BIND_A])      key_input.w |= BTN_A;
                else if (btn == pad_mappings[GBA_BIND_B])      key_input.w |= BTN_B;
                else if (btn == pad_mappings[GBA_BIND_L])      key_input.w |= BTN_LT;
                else if (btn == pad_mappings[GBA_BIND_R])      key_input.w |= BTN_RT;
                else if (btn == pad_mappings[GBA_BIND_START])  key_input.w |= BTN_STA;
                else if (btn == pad_mappings[GBA_BIND_SELECT]) key_input.w |= BTN_SEL;
                else if (btn == pad_mappings[GBA_BIND_UP])     key_input.w |= BTN_U;
                else if (btn == pad_mappings[GBA_BIND_DOWN])   key_input.w |= BTN_D;
                else if (btn == pad_mappings[GBA_BIND_LEFT])   key_input.w |= BTN_L;
                else if (btn == pad_mappings[GBA_BIND_RIGHT])  key_input.w |= BTN_R;
            }

            // Analog Stick Movement (Left Stick to GBA D-Pad with 12000 deadzone)
            else if (event.type == SDL_CONTROLLERAXISMOTION) {
                const int DEADZONE = 12000;
                if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    if (event.caxis.value < -DEADZONE) {
                        key_input.w &= ~BTN_L; key_input.w |= BTN_R;
                    } else if (event.caxis.value > DEADZONE) {
                        key_input.w &= ~BTN_R; key_input.w |= BTN_L;
                    } else {
                        key_input.w |= (BTN_L | BTN_R);
                    }
                } else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    if (event.caxis.value < -DEADZONE) {
                        key_input.w &= ~BTN_U; key_input.w |= BTN_D;
                    } else if (event.caxis.value > DEADZONE) {
                        key_input.w &= ~BTN_D; key_input.w |= BTN_U;
                    } else {
                        key_input.w |= (BTN_U | BTN_D);
                    }
                }
            }

            
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_UP:    case SDLK_w: key_input.w &= ~BTN_U; break;
                    case SDLK_DOWN:  case SDLK_s: key_input.w &= ~BTN_D; break;
                    case SDLK_LEFT:  case SDLK_a: key_input.w &= ~BTN_L; break;
                    case SDLK_RIGHT: case SDLK_d: key_input.w &= ~BTN_R; break;
                    case SDLK_z:                  key_input.w &= ~BTN_A; break;
                    case SDLK_x:                  key_input.w &= ~BTN_B; break;
                    case SDLK_q:                  key_input.w &= ~BTN_LT; break;
                    case SDLK_e:                  key_input.w &= ~BTN_RT; break;
                    case SDLK_RETURN:             key_input.w &= ~BTN_STA; break;
                    case SDLK_LSHIFT:
                    case SDLK_BACKSPACE:          key_input.w &= ~BTN_SEL; break;
                    case SDLK_ESCAPE:
                        if (is_remapping) {
                            // Pressing Escape cancels remapping mode
                            is_remapping = false;
                            remap_target_index = -1;
                            SDL_SetWindowTitle(window, "GMU GBA");
                            printf("[Remap] Remapping cancelled.\n");
                        } else {
                            running = false;
                        }
                        break;
                    default: break;
                }
            } else if (event.type == SDL_KEYUP) {
                switch (event.key.keysym.sym) {
                    case SDLK_UP:    case SDLK_w: key_input.w |= BTN_U; break;
                    case SDLK_DOWN:  case SDLK_s: key_input.w |= BTN_D; break;
                    case SDLK_LEFT:  case SDLK_a: key_input.w |= BTN_L; break;
                    case SDLK_RIGHT: case SDLK_d: key_input.w |= BTN_R; break;
                    case SDLK_z:                  key_input.w |= BTN_A; break;
                    case SDLK_x:                  key_input.w |= BTN_B; break;
                    case SDLK_q:                  key_input.w |= BTN_LT; break;
                    case SDLK_e:                  key_input.w |= BTN_RT; break;
                    case SDLK_RETURN:             key_input.w |= BTN_STA; break;
                    case SDLK_LSHIFT:
                    case SDLK_BACKSPACE:          key_input.w |= BTN_SEL; break;
                    default: break;
                }
            }
#ifdef _WIN32
            else if (event.type == SDL_SYSWMEVENT) {
                if (event.syswm.msg->msg.win.msg == WM_COMMAND) {
                    WORD cmd_id = LOWORD(event.syswm.msg->msg.win.wParam);

                    switch (cmd_id) {
                        case ID_CTRL_SETUP:
                            configure_controller(event.syswm.msg->msg.win.hwnd);
                            break;

                        case ID_CTRL_REMAP_A:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_A);
                            break;

                        case ID_CTRL_REMAP_B:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_B);
                            break;

                        case ID_CTRL_REMAP_L:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_L);
                            break;

                        case ID_CTRL_REMAP_R:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_R);
                            break;

                        case ID_CTRL_REMAP_START:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_START);
                            break;

                        case ID_CTRL_REMAP_SELECT:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_SELECT);
                            break;

                        case ID_CTRL_REMAP_UP:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_UP);
                            break;

                        case ID_CTRL_REMAP_DOWN:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_DOWN);
                            break;

                        case ID_CTRL_REMAP_LEFT:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_LEFT);
                            break;

                        case ID_CTRL_REMAP_RIGHT:
                            start_remap_button(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_RIGHT);
                            break;

                        case ID_1X_SPEED:
                            printf("[Speed] 1x Speed selected\n");
                            break;

                        case ID_2X_SPEED:
                            printf("[Speed] 2x Speed selected\n");
                            break;

                        case ID_ROM_OPEN:
                            printf("[ROM] Open ROM clicked\n");
                            break;
                    }
                }
            }
#endif
        }

        // Render one full GBA video frame (scanlines 0 through 227)
        void *pixels = NULL;
        int pitch = 0;
        if (SDL_LockTexture(texture, NULL, &pixels, &pitch) == 0) {
            gba_step_frame(pixels);
            SDL_UnlockTexture(texture);
        }

        // Draw to window
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        // Cap frame rate to ~60 FPS (16 ms per frame)
        SDL_Delay(16);
    }

    // Cleanup
    if (active_controller) {
        SDL_GameControllerClose(active_controller);
        active_controller = NULL;
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    arm_uninit();
    return 0;
}
