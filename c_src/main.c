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
#define ID_CTRL_REMAPPING  2002

#include "arm.h"
#include "arm_mem.h"
#include "io.h"
#include "video.h"
#include "bios.h"

#define GBA_WIDTH  240
#define GBA_HEIGHT 160
#define SCALE      3   // Window will open at 720x480

#ifdef _WIN32
// So far just a visual shell only
static void create_menu(SDL_Window *window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) return;

    // The actual window
    HWND hwnd = wmInfo.info.win.window;

    HMENU hMenuBar = CreateMenu();
    HMENU hEmuMenu = CreateMenu();
    HMENU hCtrlMenu = CreateMenu();

    HMENU hRomMenu = CreateMenu();
    // Emulation Menu
    AppendMenu(hEmuMenu, MF_STRING, 1002, "&1x Speed");
    AppendMenu(hEmuMenu, MF_STRING, 1001, "&2x Speed");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hEmuMenu, "&Speed");

    // Controllers Menu
    AppendMenu(hCtrlMenu, MF_STRING, 2001, "&Configure Controller...");
    AppendMenu(hCtrlMenu, MF_STRING, 2002, "&Controller Remapping");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hCtrlMenu, "&Controller");
    // Rom selection Menu
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hRomMenu, "&Rom");

    SetMenu(hwnd, hMenuBar);
}
#endif

#ifdef _WIN32

static void configure_controller(HWND hwnd) {
    printf("[Menu] Configure Controller clicked: Success!\n");
    MessageBox(hwnd, "Success", "Controller Configuration", MB_OK);
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

    // 1. Initialize core CPU & memory
    arm_init();
    bios_init();

    // 2. Load ROM
    if (!load_rom(rom_path)) {
        arm_uninit();
        return 1;
    }

    // 3. Load BIOS (or fallback to built-in HLE direct boot)
    bool has_bios = load_bios(bios_path);
    key_input.w = 0x03ff; // All buttons unpressed (active-low)
    int_halt = false;

    if (has_bios) {
        arm_reset();
    } else {
        printf("No BIOS found; booting directly via High-Level Emulation (HLE).\n");
        bios_direct_boot();
    }

    // 4. Initialize SDL2 window and renderer
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
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
        SDL_PIXELFORMAT_ABGR8888, // Previously caused a purple/blue tint
        SDL_TEXTUREACCESS_STREAMING,
        GBA_WIDTH,
        GBA_HEIGHT
    );

#ifdef _WIN32
    // Attach the menu bar
 create_menu(window);
 create_menu(window);
 SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif

    // 5. Main Emulation Loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
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
                    case SDLK_ESCAPE:             running = false; break;
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
                if (cmd_id == ID_CTRL_SETUP) {
                    configure_controller(event.syswm.msg->msg.win.hwnd);
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

    // 6. Cleanup
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    arm_uninit();
    return 0;
}