#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "sqlite3.h"

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

// Keyboard remap IDs
#define ID_KEY_REMAP_A        5001
#define ID_KEY_REMAP_B        5002
#define ID_KEY_REMAP_L        5003
#define ID_KEY_REMAP_R        5004
#define ID_KEY_REMAP_START    5005
#define ID_KEY_REMAP_SELECT   5006
#define ID_KEY_REMAP_UP       5007
#define ID_KEY_REMAP_DOWN     5008
#define ID_KEY_REMAP_LEFT     5009
#define ID_KEY_REMAP_RIGHT    5010

#include "arm.h"
#include "arm_mem.h"
#include "io.h"
#include "video.h"
#include "bios.h"

#define GBA_WIDTH  240
#define GBA_HEIGHT 160
#define SCALE      3   // Window will open at 720x480

// Controller Mappings stored as enum
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

// Default keyboard bindings
static SDL_Keycode key_mappings[GBA_BIND_COUNT] = {
    SDLK_z,      // GBA A
    SDLK_x,      // GBA B
    SDLK_q,      // GBA L
    SDLK_e,      // GBA R
    SDLK_RETURN, // Start
    SDLK_LSHIFT, // Select
    SDLK_UP,     // D-Pad Up
    SDLK_DOWN,   // D-Pad Down
    SDLK_LEFT,   // D-Pad Left
    SDLK_RIGHT   // D-Pad Right
};

// State for active remapping
static bool is_remapping = false;
static int remap_target_index = -1;

static bool is_key_remapping = false;
static int key_remap_target_index = -1;

// Active gamepad pointer
static SDL_GameController *active_controller = NULL;

// DB Forward Declarations
sqlite3 *setupDB(void);
bool db_create_table(sqlite3 *db);
bool db_insert(const char *button, sqlite3 *db, int gba_id);
bool db_save_controller_bind(sqlite3 *db, int gba_id, int sdl_button);
static void save_controller_mapping(sqlite3 *db);
static void load_controller_mapping(sqlite3 *db);
static void save_keyboard_mapping(sqlite3 *db);
static void load_keyboard_mapping(sqlite3 *db);

#ifdef _WIN32
// Searches for connected gamepads and opens the first one
static void configure_controller(HWND hwnd) {
    int count = SDL_NumJoysticks();

    if (count <= 0) {
        MessageBox(hwnd,
            "No game controllers detected!\n\n"
            "Please plug in an Xbox, PlayStation, or USB gamepad and try again.\n\n"
            "(In the meantime, keyboard controls work and can be customized in the Keyboard menu!)",
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

// Triggers remapping mode for a specific GBA controller button
static void start_remap_button(SDL_Window *window, HWND hwnd, int bind_index) {
    (void)hwnd;
    is_key_remapping = false;
    key_remap_target_index = -1;
    remap_target_index = bind_index;
    is_remapping = true;

    char title[128];
    snprintf(title, sizeof(title), "GMU GBA - [PRESS ANY CONTROLLER BUTTON FOR %s]",
             pad_bind_labels[bind_index]);
    SDL_SetWindowTitle(window, title);

    printf("[Remap] Waiting for controller input for GBA %s...\n",
           pad_bind_labels[bind_index]);
}

// Triggers remapping mode for a specific GBA keyboard key
static void start_remap_key(SDL_Window *window, HWND hwnd, int bind_index) {
    (void)hwnd;
    is_remapping = false;
    remap_target_index = -1;
    key_remap_target_index = bind_index;
    is_key_remapping = true;

    char title[128];
    snprintf(title, sizeof(title), "GMU GBA - [PRESS ANY KEY FOR %s]",
             pad_bind_labels[bind_index]);
    SDL_SetWindowTitle(window, title);

    printf("[Remap] Waiting for keyboard key for GBA %s...\n",
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
    HMENU hKeyMenu   = CreateMenu();

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

    // Keyboard menu
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_A,      "&A");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_B,      "&B");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_L,      "&Left shoulder");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_R,      "&Right Shoulder");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_START,  "&Start");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_SELECT, "&Select");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_UP,     "&Up");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_DOWN,   "&Down");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_LEFT,   "&Left");
    AppendMenu(hKeyMenu, MF_STRING, ID_KEY_REMAP_RIGHT,  "&Right");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hKeyMenu, "&Keyboard");

    // ROM Selection Menu
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

// ============================================================================
// SQLite DB Functions
// ============================================================================

sqlite3 *setupDB(void) {
    sqlite3 *db;
    int rc = sqlite3_open("mydatabase.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database %s\n", sqlite3_errmsg(db));
        return NULL;
    }
    return db;
}

bool db_create_table(sqlite3 *db) {
    char *errMsg = 0;

    // Keyboard bindings table
    const char *sql_key =
        "CREATE TABLE IF NOT EXISTS keybind("
        "gba_id INTEGER PRIMARY KEY, "
        "key_name VARCHAR(30));";
    int rc = sqlite3_exec(db, sql_key, 0, 0, &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Could not create keybind table in DB: %s\n", errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    // Controller bindings table
    const char *sql_ctrl =
        "CREATE TABLE IF NOT EXISTS controller_bind("
        "gba_id INTEGER PRIMARY KEY, "
        "bind_name VARCHAR(30), "
        "sdl_button INTEGER NOT NULL);";
    rc = sqlite3_exec(db, sql_ctrl, 0, 0, &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Could not create controller_bind table in DB: %s\n", errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool db_insert(const char *button, sqlite3 *db, int gba_id) {
    if (!db || !button || gba_id < 0 || gba_id >= GBA_BIND_COUNT) return false;

    const char *sql = "INSERT OR REPLACE INTO keybind(gba_id, key_name) VALUES (?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, gba_id);
    sqlite3_bind_text(stmt, 2, button, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Could not write to DB: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

// Saves or updates a single controller button mapping in SQLite
bool db_save_controller_bind(sqlite3 *db, int gba_id, int sdl_button) {
    if (!db || gba_id < 0 || gba_id >= GBA_BIND_COUNT) return false;

    const char *sql =
        "INSERT OR REPLACE INTO controller_bind(gba_id, bind_name, sdl_button) "
        "VALUES (?, ?, ?);";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(stmt, 1, gba_id);
    sqlite3_bind_text(stmt, 2, pad_bind_keys[gba_id], -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, sdl_button);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Could not write controller bind to DB: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

// Saves all 10 controller mappings to the SQLite database
static void save_controller_mapping(sqlite3 *db) {
    if (!db) return;

    for (int i = 0; i < GBA_BIND_COUNT; i++) {
        db_save_controller_bind(db, i, pad_mappings[i]);
    }
    printf("[DB] Controller mappings saved to mydatabase.db\n");
}

// Loads controller mappings from SQLite (or seeds defaults on first run)
static void load_controller_mapping(sqlite3 *db) {
    if (!db) return;

    const char *sql = "SELECT gba_id, sdl_button FROM controller_bind;";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] Failed to load controller mappings: %s\n", sqlite3_errmsg(db));
        return;
    }

    int loaded_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int gba_id = sqlite3_column_int(stmt, 0);
        int sdl_button = sqlite3_column_int(stmt, 1);

        if (gba_id >= 0 && gba_id < GBA_BIND_COUNT) {
            pad_mappings[gba_id] = sdl_button;
            loaded_count++;
        }
    }

    sqlite3_finalize(stmt);

    if (loaded_count == 0) {
        printf("[DB] No saved controller mappings found. Saving defaults to mydatabase.db...\n");
        save_controller_mapping(db);
    } else {
        printf("[DB] Loaded %d controller mappings from mydatabase.db\n", loaded_count);
    }
}

// Saves all 10 keyboard mappings to the SQLite keybind table
static void save_keyboard_mapping(sqlite3 *db) {
    if (!db) return;

    for (int i = 0; i < GBA_BIND_COUNT; i++) {
        const char *key_name = SDL_GetKeyName(key_mappings[i]);
        db_insert(key_name, db, i);
    }
    printf("[DB] Keyboard mappings saved to mydatabase.db\n");
}

// Loads keyboard mappings from SQLite (or seeds defaults on first run)
static void load_keyboard_mapping(sqlite3 *db) {
    if (!db) return;

    const char *sql = "SELECT gba_id, key_name FROM keybind;";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] Failed to load keyboard mappings: %s\n", sqlite3_errmsg(db));
        return;
    }

    int loaded_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int gba_id = sqlite3_column_int(stmt, 0);
        const unsigned char *key_text = sqlite3_column_text(stmt, 1);

        if (gba_id >= 0 && gba_id < GBA_BIND_COUNT && key_text) {
            SDL_Keycode kc = SDL_GetKeyFromName((const char *)key_text);
            if (kc != SDLK_UNKNOWN) {
                key_mappings[gba_id] = kc;
                loaded_count++;
            }
        }
    }

    sqlite3_finalize(stmt);

    if (loaded_count == 0) {
        printf("[DB] No saved keyboard mappings found. Saving defaults to mydatabase.db...\n");
        save_keyboard_mapping(db);
    } else {
        printf("[DB] Loaded %d keyboard mappings from mydatabase.db\n", loaded_count);
    }
}

int main(int argc, char *argv[]) {
    // Setting up sqlite3 db
    sqlite3 *db = setupDB();
    if (!db) {
        return 1;
    }

    if (!db_create_table(db)) {
        sqlite3_close(db);
        return 1;
    }

    if (argc < 2) {
        printf("====================================================\n");
        printf("  GMU GBA - Game Boy Advance Emulator (In C!)       \n");
        printf("====================================================\n");
        printf("Usage: %s <rom.gba> [bios.bin]\n\n", argv[0]);
        printf("Default Controls (customizable via Controller / Keyboard menus):\n");
        printf("  D-Pad:  Arrow keys\n");
        printf("  A / B:  Z / X\n");
        printf("  L / R:  Q / E\n");
        printf("  Start:  Enter\n");
        printf("  Select: Left Shift\n");
        printf("  Quit:   Escape\n");
        sqlite3_close(db);
        return 1;
    }

    const char *rom_path = argv[1];
    const char *bios_path = (argc >= 3) ? argv[2] : "gba_bios.bin";

    arm_init();
    bios_init();

    if (!load_rom(rom_path)) {
        arm_uninit();
        sqlite3_close(db);
        return 1;
    }

    bool has_bios = load_bios(bios_path);
    key_input.w = 0x03ff; // All buttons unpressed (active-low)
    int_halt = false;

    if (has_bios) {
        arm_reset();
    } else {
        printf("No BIOS found; booting directly via High-Level Emulation (HLE).\n");
        bios_direct_boot();
    }

    // Load controller & keyboard mappings from SQLite database
    load_controller_mapping(db);
    load_keyboard_mapping(db);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        arm_uninit();
        sqlite3_close(db);
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

                // If currently in Remapping Mode grab input and store in DB
                if (is_remapping && remap_target_index >= 0) {
                    pad_mappings[remap_target_index] = btn;
                    printf("[Remap] GBA %s is now mapped to controller button %d!\n",
                           pad_bind_labels[remap_target_index], btn);

                    // Save updated binding directly to mydatabase.db
                    db_save_controller_bind(db, remap_target_index, btn);

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
                SDL_Keycode sym = event.key.keysym.sym;

                // If currently in Keyboard Remapping Mode, capture the key and store in DB
                if (is_key_remapping && key_remap_target_index >= 0) {
                    if (event.key.repeat) continue;

                    if (sym == SDLK_ESCAPE) {
                        is_key_remapping = false;
                        key_remap_target_index = -1;
                        SDL_SetWindowTitle(window, "GMU GBA");
                        printf("[Remap] Keyboard remapping cancelled.\n");
                        continue;
                    }

                    key_mappings[key_remap_target_index] = sym;
                    const char *key_name = SDL_GetKeyName(sym);
                    printf("[Remap] GBA %s is now mapped to key '%s'!\n",
                           pad_bind_labels[key_remap_target_index], key_name);

                    // Save updated keybinding directly to mydatabase.db
                    db_insert(key_name, db, key_remap_target_index);

                    // Reset title and state
                    SDL_SetWindowTitle(window, "GMU GBA");
                    is_key_remapping = false;
                    key_remap_target_index = -1;
                    continue;
                }

                // Normal Gameplay Keyboard Mapping
                if      (sym == key_mappings[GBA_BIND_A])      key_input.w &= ~BTN_A;
                else if (sym == key_mappings[GBA_BIND_B])      key_input.w &= ~BTN_B;
                else if (sym == key_mappings[GBA_BIND_L])      key_input.w &= ~BTN_LT;
                else if (sym == key_mappings[GBA_BIND_R])      key_input.w &= ~BTN_RT;
                else if (sym == key_mappings[GBA_BIND_START])  key_input.w &= ~BTN_STA;
                else if (sym == key_mappings[GBA_BIND_SELECT]) key_input.w &= ~BTN_SEL;
                else if (sym == key_mappings[GBA_BIND_UP])     key_input.w &= ~BTN_U;
                else if (sym == key_mappings[GBA_BIND_DOWN])   key_input.w &= ~BTN_D;
                else if (sym == key_mappings[GBA_BIND_LEFT])   key_input.w &= ~BTN_L;
                else if (sym == key_mappings[GBA_BIND_RIGHT])  key_input.w &= ~BTN_R;
                else if (sym == SDLK_ESCAPE) {
                    if (is_remapping) {
                        is_remapping = false;
                        remap_target_index = -1;
                        SDL_SetWindowTitle(window, "GMU GBA");
                        printf("[Remap] Remapping cancelled.\n");
                    } else {
                        running = false;
                    }
                }
            } else if (event.type == SDL_KEYUP) {
                if (is_key_remapping) continue;

                SDL_Keycode sym = event.key.keysym.sym;
                if      (sym == key_mappings[GBA_BIND_A])      key_input.w |= BTN_A;
                else if (sym == key_mappings[GBA_BIND_B])      key_input.w |= BTN_B;
                else if (sym == key_mappings[GBA_BIND_L])      key_input.w |= BTN_LT;
                else if (sym == key_mappings[GBA_BIND_R])      key_input.w |= BTN_RT;
                else if (sym == key_mappings[GBA_BIND_START])  key_input.w |= BTN_STA;
                else if (sym == key_mappings[GBA_BIND_SELECT]) key_input.w |= BTN_SEL;
                else if (sym == key_mappings[GBA_BIND_UP])     key_input.w |= BTN_U;
                else if (sym == key_mappings[GBA_BIND_DOWN])   key_input.w |= BTN_D;
                else if (sym == key_mappings[GBA_BIND_LEFT])   key_input.w |= BTN_L;
                else if (sym == key_mappings[GBA_BIND_RIGHT])  key_input.w |= BTN_R;
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

                        case ID_KEY_REMAP_A:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_A);
                            break;

                        case ID_KEY_REMAP_B:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_B);
                            break;

                        case ID_KEY_REMAP_L:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_L);
                            break;

                        case ID_KEY_REMAP_R:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_R);
                            break;

                        case ID_KEY_REMAP_START:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_START);
                            break;

                        case ID_KEY_REMAP_SELECT:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_SELECT);
                            break;

                        case ID_KEY_REMAP_UP:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_UP);
                            break;

                        case ID_KEY_REMAP_DOWN:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_DOWN);
                            break;

                        case ID_KEY_REMAP_LEFT:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_LEFT);
                            break;

                        case ID_KEY_REMAP_RIGHT:
                            start_remap_key(window, event.syswm.msg->msg.win.hwnd, GBA_BIND_RIGHT);
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

    sqlite3_close(db);

    arm_uninit();
    return 0;
}
