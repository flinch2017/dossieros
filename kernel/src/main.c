#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

// Set the base revision to 6, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// Halt and catch fire function.
static void hcf(void) {
    for (;;) {
#if defined (__x86_64__)
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}



/* Read a byte from an x86 I/O port. */
static inline uint8_t inb(uint16_t port) {
    uint8_t value;

    asm volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

/* Write a byte to an x86 I/O port. */
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

/* Wait until the PS/2 controller can accept a command or data byte. */
static bool ps2_wait_input_clear(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if ((inb(0x64) & 0x02) == 0) {
            return true;
        }
    }

    return false;
}

/* Enable the auxiliary device and activate mouse data reporting. */
static bool mouse_initialize(void) {
    /* Enable the PS/2 auxiliary (mouse) port. */
    if (!ps2_wait_input_clear()) return false;
    outb(0x64, 0xA8);

    /* Ask the mouse to use its default settings. */
    if (!ps2_wait_input_clear()) return false;
    outb(0x64, 0xD4);

    if (!ps2_wait_input_clear()) return false;
    outb(0x60, 0xF6);

    /* Wait for the mouse acknowledgement. */
    bool acknowledged = false;

    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(0x64);

        if ((status & 0x21) == 0x21) {
            if (inb(0x60) == 0xFA) {
                acknowledged = true;
                break;
            }
        }
    }

    if (!acknowledged) return false;

    /* Enable mouse data reporting. */
    if (!ps2_wait_input_clear()) return false;
    outb(0x64, 0xD4);

    if (!ps2_wait_input_clear()) return false;
    outb(0x60, 0xF4);

    acknowledged = false;

    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(0x64);

        if ((status & 0x21) == 0x21) {
            if (inb(0x60) == 0xFA) {
                acknowledged = true;
                break;
            }
        }
    }

    return acknowledged;
}

/* Read a mouse byte only when the controller reports auxiliary data. */
static bool mouse_read_byte(uint8_t *value) {
    uint8_t status = inb(0x64);

    if ((status & 0x21) != 0x21) {
        return false;
    }

    *value = inb(0x60);
    return true;
}

/* Read keyboard data without consuming mouse bytes. */
static bool keyboard_read_scancode(uint8_t *scancode) {
    uint8_t status = inb(0x64);

    if ((status & 0x01) == 0 || (status & 0x20) != 0) {
        return false;
    }

    *scancode = inb(0x60);
    return true;
}





// Scale an 8-bit colour channel value to the size the framebuffer gives the
// channel and move it into place within a pixel.
static uint32_t fb_channel(uint8_t value, uint8_t mask_size, uint8_t mask_shift) {
    uint64_t max = ((uint64_t)1 << mask_size) - 1;
    return (uint32_t)((value * max / 255) << mask_shift);
}

// Build a pixel from 8-bit red, green and blue values following the channel
// layout of the framebuffer.
static uint32_t fb_pixel(struct limine_framebuffer *fb, uint8_t red, uint8_t green, uint8_t blue) {
    return fb_channel(red, fb->red_mask_size, fb->red_mask_shift)
         | fb_channel(green, fb->green_mask_size, fb->green_mask_shift)
         | fb_channel(blue, fb->blue_mask_size, fb->blue_mask_shift);
}


static void fb_pattern(struct limine_framebuffer *fb) {
    volatile uint32_t *fb_ptr = fb->address;

    size_t center_x = fb->width * 3 / 4;
    size_t center_y = fb->height / 3;

    for (size_t y = 0; y < fb->height; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            // Dark blue background gradient.
            uint32_t red = 5;
            uint32_t green = 12 + (y * 10 / fb->height);
            uint32_t blue = 32 + (y * 35 / fb->height);

            // Calculate distance from the cyan glow.
            size_t dx = x > center_x ? x - center_x : center_x - x;
            size_t dy = y > center_y ? y - center_y : center_y - y;

            uint32_t nx = dx * 255 / (fb->width / 2 + 1);
            uint32_t ny = dy * 255 / (fb->height / 2 + 1);

            if (nx > 255) nx = 255;
            if (ny > 255) ny = 255;

            int glow = 255 - (int)((nx * nx + ny * ny) / 510);
            if (glow < 0) glow = 0;

            // Blend a subtle cyan glow into the background.
            green += glow * 45 / 255;
            blue += glow * 85 / 255;
            red += glow * 10 / 255;

            fb_ptr[y * (fb->pitch / 4) + x] =
                fb_pixel(fb, (uint8_t)red,
                         (uint8_t)green, (uint8_t)blue);
        }
    }
}


/* Minimal 5x7 uppercase bitmap font for DossierOS. */
static const uint8_t font[26][7] = {
    {14,17,17,31,17,17,17}, // A
    {30,17,17,30,17,17,30}, // B
    {14,17,16,16,16,17,14}, // C
    {30,17,17,17,17,17,30}, // D
    {31,16,16,30,16,16,31}, // E
    {31,16,16,30,16,16,16}, // F
    {14,17,16,23,17,17,15}, // G
    {17,17,17,31,17,17,17}, // H
    {14,4,4,4,4,4,14},      // I
    {7,2,2,2,18,18,12},     // J
    {17,18,20,24,20,18,17}, // K
    {16,16,16,16,16,16,31}, // L
    {17,27,21,21,17,17,17}, // M
    {17,25,21,19,17,17,17}, // N
    {14,17,17,17,17,17,14}, // O
    {30,17,17,30,16,16,16}, // P
    {14,17,17,17,21,18,13}, // Q
    {30,17,17,30,20,18,17}, // R
    {15,16,16,14,1,1,30},   // S
    {31,4,4,4,4,4,4},        // T
    {17,17,17,17,17,17,14}, // U
    {17,17,17,17,17,10,4},  // V
    {17,17,17,21,21,21,10}, // W
    {17,17,10,4,10,17,17},  // X
    {17,17,10,4,4,4,4},     // Y
    {31,1,2,4,8,16,31}      // Z
};

/* Draw one uppercase character, scaling each bitmap pixel. */
static void fb_draw_char(
    struct limine_framebuffer *fb,
    size_t x, size_t y, char c, size_t scale,
    uint32_t color
) {
    if (scale == 0) return;

    if (c < 'A' || c > 'Z') return;

    const uint8_t *glyph = font[c - 'A'];
    volatile uint32_t *pixels = fb->address;
    size_t stride = fb->pitch / 4;

    for (size_t row = 0; row < 7; row++) {
        for (size_t col = 0; col < 5; col++) {
            if ((glyph[row] & (1u << (4 - col))) == 0)
                continue;

            for (size_t sy = 0; sy < scale; sy++) {
                for (size_t sx = 0; sx < scale; sx++) {
                    size_t px = x + col * scale + sx;
                    size_t py = y + row * scale + sy;

                    if (px < fb->width && py < fb->height) {
                        pixels[py * stride + px] = color;
                    }
                }
            }
        }
    }
}

/* Draw uppercase text with spacing between characters. */
static void fb_draw_text(
    struct limine_framebuffer *fb,
    size_t x, size_t y, const char *text,
    size_t scale, uint8_t red, uint8_t green, uint8_t blue
) {
    uint32_t color = fb_pixel(fb, red, green, blue);
    size_t start_x = x;

    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == ' ') {
            x += 4 * scale;
        } else {
            fb_draw_char(fb, x, y, text[i], scale, color);
            x += 6 * scale;
        }

        if (x < start_x) return;
    }
}

/* Draw a filled rectangle, clipped to the framebuffer boundaries. */
static void fb_draw_rect(
    struct limine_framebuffer *fb,
    size_t x, size_t y, size_t width, size_t height,
    uint8_t red, uint8_t green, uint8_t blue
) {
    volatile uint32_t *pixels = fb->address;
    size_t stride = fb->pitch / 4;
    uint32_t color = fb_pixel(fb, red, green, blue);

    for (size_t dy = 0; dy < height; dy++) {
        size_t py = y + dy;
        if (py >= fb->height) break;

        for (size_t dx = 0; dx < width; dx++) {
            size_t px = x + dx;
            if (px >= fb->width) break;

            pixels[py * stride + px] = color;
        }
    }
}

/* Draw the first non-interactive DossierOS taskbar. */


/* Draw the DossierOS desktop taskbar. */
static void fb_draw_taskbar(struct limine_framebuffer *fb) {
    const size_t taskbar_height = 48;

    size_t taskbar_y = fb->height > taskbar_height
        ? fb->height - taskbar_height
        : 0;

    /* Taskbar background and top divider. */
    fb_draw_rect(
        fb, 0, taskbar_y,
        fb->width, taskbar_height,
        12, 20, 35
    );

    fb_draw_rect(
        fb, 0, taskbar_y,
        fb->width, 1,
        45, 105, 125
    );

    /* Start button. */
    fb_draw_rect(
        fb, 12, taskbar_y + 8,
        78, 32,
        28, 48, 65
    );

    fb_draw_rect(
        fb, 12, taskbar_y + 8,
        78, 1,
        65, 170, 185
    );

    /* Cyan mark beside the Start label. */
    fb_draw_rect(
        fb, 21, taskbar_y + 17,
        12, 12,
        75, 195, 205
    );

    fb_draw_text(
        fb, 39, taskbar_y + 20,
        "START", 1,
        232, 240, 248
    );

    /* System status at the right. */
    size_t status_x = fb->width > 110
        ? fb->width - 110
        : 4;

    fb_draw_rect(
        fb, status_x, taskbar_y + 21,
        6, 6,
        85, 200, 205
    );

    fb_draw_text(
        fb, status_x + 12, taskbar_y + 20,
        "SYSTEM READY", 1,
        145, 180, 198
    );
}



/* Draw buttons for open applications on the taskbar. */
static void fb_draw_app_buttons(
    struct limine_framebuffer *fb,
    bool terminal_open,
    bool terminal_minimized,
    bool file_manager_open,
    bool file_manager_minimized,
    bool file_manager_on_top
) {
    size_t taskbar_y = fb->height > 48
        ? fb->height - 48
        : 0;

    size_t button_x = 102;
    size_t button_y = taskbar_y + 8;
    size_t button_width = 112;
    size_t button_height = 32;

    if (terminal_open) {
        fb_draw_rect(
            fb, button_x, button_y,
            button_width, button_height,
            (!terminal_minimized && !file_manager_on_top)
                ? 45 : 25,
            (!terminal_minimized && !file_manager_on_top)
                ? 85 : 42,
            (!terminal_minimized && !file_manager_on_top)
                ? 105 : 60
        );

        fb_draw_text(
            fb, button_x + 9, button_y + 12,
            "TERMINAL", 1,
            terminal_minimized ? 135 : 220,
            terminal_minimized ? 155 : 235,
            terminal_minimized ? 165 : 245
        );

        button_x += button_width + 6;
    }

    if (file_manager_open) {
        fb_draw_rect(
            fb, button_x, button_y,
            button_width, button_height,
            (!file_manager_minimized && file_manager_on_top)
                ? 45 : 25,
            (!file_manager_minimized && file_manager_on_top)
                ? 85 : 42,
            (!file_manager_minimized && file_manager_on_top)
                ? 105 : 60
        );

        fb_draw_text(
            fb, button_x + 9, button_y + 12,
            "FILES", 1,
            file_manager_minimized ? 135 : 220,
            file_manager_minimized ? 155 : 235,
            file_manager_minimized ? 165 : 245
        );
    }
}



/* Draw a desktop icon with a simple custom symbol. */
static void fb_draw_desktop_icon(
    struct limine_framebuffer *fb,
    size_t x,
    size_t y,
    const char *label,
    uint8_t type
) {
    /* Icon background. */
    fb_draw_rect(
        fb, x, y, 48, 42,
        24, 39, 56
    );

    /* Muted cyan icon border. */
    fb_draw_rect(fb, x, y, 48, 1, 54, 125, 145);
    fb_draw_rect(fb, x, y + 41, 48, 1, 54, 125, 145);
    fb_draw_rect(fb, x, y, 1, 42, 54, 125, 145);
    fb_draw_rect(fb, x + 47, y, 1, 42, 54, 125, 145);

    if (type == 0) {
        /* Terminal: screen and command lines. */
        fb_draw_rect(fb, x + 8, y + 7, 32, 25, 9, 17, 29);
        fb_draw_rect(fb, x + 13, y + 14, 3, 3, 90, 210, 220);
        fb_draw_rect(fb, x + 18, y + 15, 13, 1, 160, 190, 205);
        fb_draw_rect(fb, x + 13, y + 21, 3, 3, 90, 210, 220);
        fb_draw_rect(fb, x + 18, y + 22, 9, 1, 160, 190, 205);
    } else if (type == 1) {
        /* Files: a simple folder shape. */
        fb_draw_rect(fb, x + 8, y + 13, 32, 19, 74, 145, 165);
        fb_draw_rect(fb, x + 10, y + 10, 13, 5, 74, 145, 165);
        fb_draw_rect(fb, x + 11, y + 18, 26, 10, 98, 179, 190);
    } else {
        /* Settings: a simple gear-like symbol. */
        fb_draw_rect(fb, x + 19, y + 8, 10, 26, 100, 178, 190);
        fb_draw_rect(fb, x + 11, y + 16, 26, 10, 100, 178, 190);
        fb_draw_rect(fb, x + 17, y + 14, 14, 14, 24, 39, 56);
        fb_draw_rect(fb, x + 21, y + 18, 6, 6, 100, 178, 190);
    }

    /* Text labels use the existing 5x7 bitmap font. */
    size_t label_width = 0;
    while (label[label_width] != '\0') {
        label_width++;
    }
    label_width *= 6;

    fb_draw_text(
        fb,
        x + (48 > label_width ? (48 - label_width) / 2 : 0),
        y + 48,
        label, 1,
        220, 231, 240
    );
}

static bool fb_point_in_rect(
    int32_t x,
    int32_t y,
    size_t rect_x,
    size_t rect_y,
    size_t rect_width,
    size_t rect_height
);

static bool terminal_minimize_button_hit(
    struct limine_framebuffer *fb,
    size_t win_x,
    size_t win_y,
    int32_t mouse_x,
    int32_t mouse_y
) {
    size_t win_width = fb->width > 520
        ? 480
        : fb->width - 32;

    return fb_point_in_rect(
        mouse_x,
        mouse_y,
        win_x + win_width - 49,
        win_y + 6,
        18,
        18
    );
}

static bool file_manager_minimize_button_hit(
    struct limine_framebuffer *fb,
    size_t win_x,
    size_t win_y,
    int32_t mouse_x,
    int32_t mouse_y
) {
    size_t win_width = fb->width > 460
        ? 420
        : fb->width - 32;

    return fb_point_in_rect(
        mouse_x,
        mouse_y,
        win_x + win_width - 49,
        win_y + 6,
        18,
        18
    );
}


/* Draw the DossierOS desktop workspace. */
static void fb_draw_desktop(struct limine_framebuffer *fb) {
    /* Paint the existing background first. */
    fb_pattern(fb);

    /* Desktop heading. */
    fb_draw_text(
        fb, 24, 18,
        "DOSSIEROS DESKTOP", 2,
        220, 231, 240
    );

    /* Subtle line below the heading. */
    fb_draw_rect(
        fb, 24, 38,
        fb->width > 48 ? fb->width - 48 : fb->width,
        1,
        45, 85, 105
    );

    /* Desktop shortcuts down the left side. */
    fb_draw_desktop_icon(fb, 24, 58, "TERMINAL", 0);
    fb_draw_desktop_icon(fb, 24, 124, "FILES", 1);
    fb_draw_desktop_icon(fb, 24, 190, "SETTINGS", 2);

    /* A simple system information panel. */
    size_t panel_width = fb->width > 360 ? 300 : fb->width / 2;
    size_t panel_x = fb->width > panel_width
        ? (fb->width - panel_width) / 2
        : 0;
    size_t panel_y = fb->height / 3;

    fb_draw_rect(
        fb, panel_x, panel_y,
        panel_width, 88,
        18, 30, 46
    );

    fb_draw_rect(
        fb, panel_x, panel_y,
        panel_width, 2,
        65, 170, 185
    );

    fb_draw_text(
        fb, panel_x + 16, panel_y + 18,
        "SYSTEM STATUS", 2,
        110, 205, 215
    );

    fb_draw_text(
        fb, panel_x + 16, panel_y + 52,
        "SYSTEM READY", 1,
        220, 231, 240
    );

    /* Draw the taskbar last so it stays above the desktop. */
    fb_draw_taskbar(fb);
}

static void fb_draw_start_menu(struct limine_framebuffer *fb) {
    size_t menu_x = 12;
    size_t taskbar_y = fb->height > 48 ? fb->height - 48 : 0;
    size_t menu_y = taskbar_y > 190 ? taskbar_y - 190 : 0;
    size_t menu_width = fb->width > 244 ? 220 : fb->width - 24;
    size_t menu_height = 180;

    /* Menu background and top accent */
    fb_draw_rect(fb, menu_x, menu_y,
                 menu_width, menu_height,
                 18, 30, 46);

    fb_draw_rect(fb, menu_x, menu_y,
                 menu_width, 2,
                 65, 170, 185);

    /* Menu heading */
    fb_draw_text(fb, menu_x + 14, menu_y + 12,
                 "DOSSIEROS", 2,
                 220, 231, 240);

    fb_draw_rect(fb, menu_x + 12, menu_y + 38,
                 menu_width - 24, 1,
                 45, 85, 105);

    /* Menu items */
    fb_draw_text(fb, menu_x + 20, menu_y + 52,
                 "TERMINAL", 1,
                 220, 231, 240);

    fb_draw_text(fb, menu_x + 20, menu_y + 86,
                 "FILES", 1,
                 220, 231, 240);

    fb_draw_text(fb, menu_x + 20, menu_y + 120,
                 "SETTINGS", 1,
                 220, 231, 240);
}

static bool fb_point_in_rect(
    int32_t x, int32_t y,
    size_t rx, size_t ry,
    size_t width, size_t height
) {
    return x >= (int32_t)rx &&
           y >= (int32_t)ry &&
           x < (int32_t)(rx + width) &&
           y < (int32_t)(ry + height);
}

static bool terminal_close_button_hit(
    struct limine_framebuffer *fb,
    size_t win_x,
    size_t win_y,
    int32_t mouse_x,
    int32_t mouse_y
) {
    size_t win_width = fb->width > 520
        ? 480
        : fb->width - 32;

    return fb_point_in_rect(
        mouse_x,
        mouse_y,
        win_x + win_width - 29,
        win_y + 6,
        18,
        18
    );
}

static int fb_start_menu_item_at(
    struct limine_framebuffer *fb,
    int32_t x, int32_t y
) {
    size_t menu_x = 12;
    size_t taskbar_y = fb->height > 48 ? fb->height - 48 : 0;
    size_t menu_y = taskbar_y > 190 ? taskbar_y - 190 : 0;
    size_t menu_width = fb->width > 244 ? 220 : fb->width - 24;

    if (fb_point_in_rect(x, y, menu_x, menu_y + 48,
                         menu_width, 30)) {
        return 0; /* Terminal */
    }

    if (fb_point_in_rect(x, y, menu_x, menu_y + 82,
                         menu_width, 30)) {
        return 1; /* Files */
    }

    if (fb_point_in_rect(x, y, menu_x, menu_y + 116,
                         menu_width, 30)) {
        return 2; /* Settings */
    }

    return -1;
}


static void fb_draw_terminal(
    struct limine_framebuffer *fb,
    size_t win_x,
    size_t win_y,
    const char *input,
    const char *output
) {
    size_t win_width = fb->width > 520 ? 480 : fb->width - 32;
    size_t win_height = fb->height > 340 ? 280 : fb->height - 80;

    /* Keep the window within the framebuffer. */
    if (win_x + win_width > fb->width) {
        win_x = fb->width - win_width;
    }

    if (win_y + win_height > fb->height - 48) {
        win_y = fb->height > win_height + 48
            ? fb->height - win_height - 48
            : 0;
    }

    /* Window background */
    fb_draw_rect(fb, win_x, win_y,
                 win_width, win_height,
                 12, 20, 31);

    /* Window border and title bar */
    fb_draw_rect(fb, win_x, win_y,
                 win_width, 2,
                 65, 170, 185);

    fb_draw_rect(fb, win_x, win_y + 2,
                 win_width, 30,
                 25, 39, 55);

    fb_draw_text(fb, win_x + 12, win_y + 12,
                 "DOSSIEROS TERMINAL", 1,
                 220, 231, 240);
    
    /* Minimize button */
    fb_draw_rect(fb, win_x + win_width - 45,
                win_y + 10, 10, 10,
                75, 155, 175);

    fb_draw_rect(fb, win_x + win_width - 43,
                win_y + 14, 6, 2,
                230, 240, 245);

    /* Close button */
    fb_draw_rect(fb, win_x + win_width - 25,
                win_y + 10, 10, 10,
                180, 80, 85);


    /* Terminal content */
    fb_draw_text(fb, win_x + 14, win_y + 48,
                 "DOSSIEROS KERNEL CONSOLE", 1,
                 80, 195, 205);

    if (output[0] != '\0') {
        fb_draw_text(fb, win_x + 14, win_y + 76,
                     output, 1,
                     190, 205, 215);
    }

    fb_draw_text(fb, win_x + 14, win_y + 112,
                 "DOSSIEROS> ", 1,
                 80, 195, 205);

    fb_draw_text(fb, win_x + 102, win_y + 112,
                 input, 1,
                 220, 231, 240);
}

enum {
    FS_ROOT = 0,
    FS_DOCUMENTS,
    FS_PROJECTS,
    FS_SYSTEM
};

struct virtual_file {
    const char *name;
    bool is_directory;
};

static const struct virtual_file fs_root[] = {
    { "DOCUMENTS", true },
    { "PROJECTS", true },
    { "SYSTEM", true },
    { "README.TXT", false }
};

static const struct virtual_file fs_documents[] = {
    { "WELCOME.TXT", false },
    { "NOTES.TXT", false }
};

static const struct virtual_file fs_projects[] = {
    { "DOSSIEROS", true },
    { "KERNEL.C", false }
};

static const struct virtual_file fs_system[] = {
    { "KERNEL.BIN", false },
    { "CONFIG.SYS", false }
};

static const char *fs_directory_name(int directory) {
    switch (directory) {
        case FS_DOCUMENTS: return "DOCUMENTS";
        case FS_PROJECTS:  return "PROJECTS";
        case FS_SYSTEM:    return "SYSTEM";
        default:           return "ROOT";
    }
}

static const struct virtual_file *fs_entries(
    int directory,
    size_t *count
) {
    switch (directory) {
        case FS_DOCUMENTS:
            *count = sizeof(fs_documents) / sizeof(fs_documents[0]);
            return fs_documents;

        case FS_PROJECTS:
            *count = sizeof(fs_projects) / sizeof(fs_projects[0]);
            return fs_projects;

        case FS_SYSTEM:
            *count = sizeof(fs_system) / sizeof(fs_system[0]);
            return fs_system;

        default:
            *count = sizeof(fs_root) / sizeof(fs_root[0]);
            return fs_root;
    }
}

static void fb_draw_file_manager(
    struct limine_framebuffer *fb,
    size_t win_x,
    size_t win_y,
    int directory
) {
    size_t win_width = fb->width > 460 ? 420 : fb->width - 32;
    size_t win_height = fb->height > 320 ? 260 : fb->height - 80;

    if (win_x + win_width > fb->width) {
        win_x = fb->width - win_width;
    }

    if (win_y + win_height > fb->height - 48) {
        win_y = fb->height > win_height + 48
            ? fb->height - win_height - 48
            : 0;
    }

    /* Window background and border */
    fb_draw_rect(fb, win_x, win_y,
                 win_width, win_height,
                 12, 20, 31);

    fb_draw_rect(fb, win_x, win_y,
                 win_width, 2,
                 65, 170, 185);

    /* Title bar */
    fb_draw_rect(fb, win_x, win_y + 2,
                 win_width, 30,
                 25, 39, 55);

    fb_draw_text(fb, win_x + 12, win_y + 12,
                 "DOSSIEROS FILES", 1,
                 220, 231, 240);

    
    /* Minimize button */
    fb_draw_rect(fb, win_x + win_width - 45,
                win_y + 10, 10, 10,
                75, 155, 175);

    fb_draw_rect(fb, win_x + win_width - 43,
                win_y + 14, 6, 2,
                230, 240, 245);

    /* Close button */
    fb_draw_rect(fb, win_x + win_width - 25,
                win_y + 10, 10, 10,
                180, 80, 85);


    /* Current directory */
    fb_draw_text(fb, win_x + 14, win_y + 44,
                 "LOCATION:", 1,
                 80, 195, 205);

    fb_draw_text(fb, win_x + 105, win_y + 44,
                 fs_directory_name(directory), 1,
                 220, 231, 240);

    fb_draw_rect(fb, win_x + 12, win_y + 62,
                 win_width - 24, 1,
                 45, 85, 105);

    /* Back entry */
    fb_draw_text(fb, win_x + 18, win_y + 74,
                 "[..] BACK", 1,
                 110, 205, 215);

    /* Directory contents */
    size_t count = 0;
    const struct virtual_file *entries =
        fs_entries(directory, &count);

    size_t row_y = win_y + 100;

    for (size_t i = 0; i < count; i++) {
        if (row_y + 10 >= win_y + win_height) {
            break;
        }

        fb_draw_text(
            fb,
            win_x + 18,
            row_y,
            entries[i].is_directory ? "[DIR]" : "[FILE]",
            1,
            entries[i].is_directory ? 80 : 160,
            entries[i].is_directory ? 195 : 175,
            entries[i].is_directory ? 205 : 190
        );

        fb_draw_text(fb, win_x + 75, row_y,
                     entries[i].name, 1,
                     220, 231, 240);

        row_y += 24;
    }
}



static int fs_item_at(
    struct limine_framebuffer *fb,
    size_t win_x,
    size_t win_y,
    int directory,
    int32_t mouse_x,
    int32_t mouse_y
) {
    size_t win_width = fb->width > 460 ? 420 : fb->width - 32;
    size_t win_height = fb->height > 320 ? 260 : fb->height - 80;

    if (!fb_point_in_rect(mouse_x, mouse_y,
                          win_x, win_y,
                          win_width, win_height)) {
        return -1;
    }

    /* Row zero is the Back entry. */
    if (fb_point_in_rect(mouse_x, mouse_y,
                         win_x + 10, win_y + 68,
                         win_width - 20, 22)) {
        return 0;
    }

    size_t count = 0;
    fs_entries(directory, &count);

    for (size_t i = 0; i < count; i++) {
        size_t row_y = win_y + 94 + i * 24;

        if (fb_point_in_rect(mouse_x, mouse_y,
                             win_x + 10, row_y,
                             win_width - 20, 22)) {
            return (int)i + 1;
        }
    }

    return -1;
}



static char keyboard_scancode_to_char(uint8_t scancode) {
    switch (scancode) {
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';

        case 0x10: return 'Q';
        case 0x11: return 'W';
        case 0x12: return 'E';
        case 0x13: return 'R';
        case 0x14: return 'T';
        case 0x15: return 'Y';
        case 0x16: return 'U';
        case 0x17: return 'I';
        case 0x18: return 'O';
        case 0x19: return 'P';

        case 0x1E: return 'A';
        case 0x1F: return 'S';
        case 0x20: return 'D';
        case 0x21: return 'F';
        case 0x22: return 'G';
        case 0x23: return 'H';
        case 0x24: return 'J';
        case 0x25: return 'K';
        case 0x26: return 'L';

        case 0x2C: return 'Z';
        case 0x2D: return 'X';
        case 0x2E: return 'C';
        case 0x2F: return 'V';
        case 0x30: return 'B';
        case 0x31: return 'N';
        case 0x32: return 'M';

        case 0x39: return ' ';
        default: return '\0';
    }
}

static bool text_equals(const char *a, const char *b) {
    size_t i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }

    return a[i] == b[i];
}

static bool text_starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return false;
        }
        i++;
    }

    return true;
}

static void text_copy(
    char *destination,
    const char *source,
    size_t capacity
) {
    if (capacity == 0) {
        return;
    }

    size_t i = 0;

    while (i + 1 < capacity && source[i] != '\0') {
        destination[i] = source[i];
        i++;
    }

    destination[i] = '\0';
}





static void terminal_execute(
    const char *input,
    char *output,
    size_t output_size
) {
    if (output_size == 0) {
        return;
    }

    if (text_equals(input, "HELP")) {
        text_copy(output, "HELP ABOUT CLEAR ECHO TEXT", output_size);
    } else if (text_equals(input, "ABOUT")) {
        text_copy(output, "DOSSIEROS C KERNEL CONSOLE", output_size);
    } else if (text_equals(input, "CLEAR")) {
        output[0] = '\0';
    } else if (text_starts_with(input, "ECHO ")) {
        text_copy(output, input + 5, output_size);
    } else if (input[0] == '\0') {
        output[0] = '\0';
    } else {
        text_copy(output, "UNKNOWN COMMAND - TYPE HELP", output_size);
    }
}

static void fb_draw_status_notice(
    struct limine_framebuffer *fb,
    const char *message
) {
    size_t panel_width = fb->width > 360 ? 300 : fb->width / 2;
    size_t panel_x = fb->width > panel_width
        ? (fb->width - panel_width) / 2
        : 0;
    size_t panel_y = fb->height / 3;

    fb_draw_text(fb, panel_x + 16, panel_y + 52,
                 message, 1,
                 220, 231, 240);
}



/* Draw a short keyboard status message above the taskbar. */
static void fb_draw_keyboard_status(
    struct limine_framebuffer *fb,
    const char *message
) {
    size_t y = fb->height > 90 ? fb->height - 90 : 0;

    /* Clear the status-message area. */
    fb_draw_rect(
        fb, 16, y, fb->width > 32 ? fb->width - 32 : fb->width,
        16, 5, 12, 32
    );

    fb_draw_text(
        fb, 18, y + 4, message, 1,
        145, 210, 220
    );
}




static bool cursor_visible = false;
static uint32_t cursor_saved_pixels[12 * 12];

static void fb_draw_cursor(
    struct limine_framebuffer *fb,
    size_t x,
    size_t y
) {
    const size_t cursor_width = 12;
    const size_t cursor_height = 12;

    static const uint16_t shape[12] = {
        0x001,
        0x003,
        0x007,
        0x00F,
        0x01F,
        0x03F,
        0x07F,
        0x0FF,
        0x1FF,
        0x03F,
        0x033,
        0x061
    };

    volatile uint32_t *pixels =
        (volatile uint32_t *)fb->address;

    size_t stride = fb->pitch / 4;

    /*
     * If the cursor is already visible, restore
     * the pixels that were underneath it.
     */
    if (cursor_visible) {
        for (size_t row = 0; row < cursor_height; row++) {
            for (size_t col = 0; col < cursor_width; col++) {
                size_t px = x + col;
                size_t py = y + row;

                if (px < fb->width && py < fb->height) {
                    pixels[py * stride + px] =
                        cursor_saved_pixels[
                            row * cursor_width + col
                        ];
                }
            }
        }

        cursor_visible = false;
        return;
    }

    /*
     * Save every pixel underneath the cursor
     * before drawing it.
     */
    for (size_t row = 0; row < cursor_height; row++) {
        for (size_t col = 0; col < cursor_width; col++) {
            size_t px = x + col;
            size_t py = y + row;

            uint32_t original = 0;

            if (px < fb->width && py < fb->height) {
                original = pixels[py * stride + px];
            }

            cursor_saved_pixels[
                row * cursor_width + col
            ] = original;
        }
    }

    /*
     * Draw the white cursor using the bitmap.
     */
    for (size_t row = 0; row < cursor_height; row++) {
        for (size_t col = 0; col < cursor_width; col++) {
            size_t px = x + col;
            size_t py = y + row;

            bool pixel_on =
                (shape[row] & (1u << col)) != 0;

            if (pixel_on &&
                px < fb->width &&
                py < fb->height) {
                pixels[py * stride + px] = 0x00FFFFFF;
            }
        }
    }

    cursor_visible = true;
}




// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.

void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        hcf();
    }

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    for (uint64_t i = 0;
         i < framebuffer_request.response->framebuffer_count;
         i++) {
        struct limine_framebuffer *framebuffer =
            framebuffer_request.response->framebuffers[i];

        if (framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB ||
            framebuffer->bpp != 32) {
            hcf();
        }

        fb_pattern(framebuffer);
        fb_draw_desktop(framebuffer);
    }

    struct limine_framebuffer *fb =
        framebuffer_request.response->framebuffers[0];

    if (!mouse_initialize()) {
        fb_draw_keyboard_status(fb, "MOUSE INIT FAILED");
    } else {
        fb_draw_keyboard_status(fb, "MOUSE READY");
    }

    int32_t cursor_x = (int32_t)(fb->width / 2);
    int32_t cursor_y = (int32_t)(fb->height / 2);

    fb_draw_cursor(fb, (size_t)cursor_x, (size_t)cursor_y);

    uint8_t packet[3];
    size_t packet_index = 0;

    bool start_menu_open = false;
    bool left_was_down = false;
    const char *notice = NULL;

    /* Terminal state */
    bool terminal_open = false;
    bool terminal_minimized = false;
    char terminal_input[64] = "";
    char terminal_output[96] = "TYPE HELP FOR COMMANDS";
    size_t terminal_input_length = 0;

    size_t terminal_x = 80;
    size_t terminal_y = 70;

    bool terminal_dragging = false;
    int32_t drag_offset_x = 0;
    int32_t drag_offset_y = 0;

    /* File Manager state */
    bool file_manager_open = false;
    bool file_manager_minimized = false;
    int file_manager_directory = FS_ROOT;

    size_t file_manager_x = 120;
    size_t file_manager_y = 100;

    bool file_manager_dragging = false;
    int32_t file_manager_drag_offset_x = 0;
    int32_t file_manager_drag_offset_y = 0;

    /* Window stacking: true means File Manager is in front. */
    bool file_manager_on_top = false;

    for (;;) {
        uint8_t byte;

        /* =========================
           MOUSE INPUT
           ========================= */
        if (mouse_read_byte(&byte)) {
            if (packet_index == 0 && (byte & 0x08) == 0) {
                continue;
            }

            packet[packet_index++] = byte;

            if (packet_index == 3) {
                packet_index = 0;

                if ((packet[0] & 0xC0) == 0) {
                    int32_t dx = (int8_t)packet[1];
                    int32_t dy = (int8_t)packet[2];

                    bool left_down = (packet[0] & 0x01) != 0;

                    /* Remove the cursor from its old position. */
                    fb_draw_cursor(
                        fb,
                        (size_t)cursor_x,
                        (size_t)cursor_y
                    );

                    /* Update and clamp cursor coordinates. */
                    cursor_x += dx;
                    cursor_y -= dy;

                    if (cursor_x < 0) {
                        cursor_x = 0;
                    }

                    if (cursor_y < 0) {
                        cursor_y = 0;
                    }

                    if (cursor_x >= (int32_t)fb->width) {
                        cursor_x = (int32_t)fb->width - 1;
                    }

                    if (cursor_y >= (int32_t)fb->height) {
                        cursor_y = (int32_t)fb->height - 1;
                    }

                    /* Move Terminal while dragging its title bar. */
                    if (terminal_dragging && left_down) {
                        int32_t new_x = cursor_x - drag_offset_x;
                        int32_t new_y = cursor_y - drag_offset_y;

                        size_t win_width = fb->width > 520
                            ? 480
                            : fb->width - 32;

                        size_t win_height = fb->height > 340
                            ? 280
                            : fb->height - 80;

                        if (new_x < 0) {
                            new_x = 0;
                        }

                        if (new_y < 0) {
                            new_y = 0;
                        }

                        if ((size_t)new_x + win_width > fb->width) {
                            new_x = (int32_t)(fb->width - win_width);
                        }

                        if ((size_t)new_y + win_height > fb->height - 48) {
                            new_y = fb->height > win_height + 48
                                ? (int32_t)(
                                    fb->height - win_height - 48
                                )
                                : 0;
                        }

                        terminal_x = (size_t)new_x;
                        terminal_y = (size_t)new_y;
                    }

                    
                    /* Move File Manager while dragging its title bar. */
                    if (file_manager_dragging && left_down) {
                        int32_t new_x =
                            cursor_x - file_manager_drag_offset_x;
                        int32_t new_y =
                            cursor_y - file_manager_drag_offset_y;

                        size_t win_width = fb->width > 460
                            ? 420
                            : fb->width - 32;

                        size_t win_height = fb->height > 320
                            ? 260
                            : fb->height - 80;

                        if (new_x < 0) {
                            new_x = 0;
                        }

                        if (new_y < 0) {
                            new_y = 0;
                        }

                        if ((size_t)new_x + win_width > fb->width) {
                            new_x = (int32_t)(fb->width - win_width);
                        }

                        if ((size_t)new_y + win_height > fb->height - 48) {
                            new_y = fb->height > win_height + 48
                                ? (int32_t)(fb->height - win_height - 48)
                                : 0;
                        }

                        file_manager_x = (size_t)new_x;
                        file_manager_y = (size_t)new_y;
                    }


                    /* Handle a new left mouse-button click. */
                    if (left_down && !left_was_down) {
                        size_t taskbar_y = fb->height > 48
                            ? fb->height - 48
                            : 0;

                        size_t menu_x = 12;
                        size_t menu_y = taskbar_y > 190
                            ? taskbar_y - 190
                            : 0;

                        size_t menu_width = fb->width > 244
                            ? 220
                            : fb->width - 24;

                        bool on_start = fb_point_in_rect(
                            cursor_x, cursor_y,
                            12, taskbar_y + 8, 78, 32
                        );

                        bool on_terminal_taskbar = fb_point_in_rect(
                            cursor_x, cursor_y,
                            102, taskbar_y + 8, 112, 32
                        );

                        bool on_files_taskbar = fb_point_in_rect(
                            cursor_x, cursor_y,
                            220, taskbar_y + 8, 112, 32
                        );

                        bool inside_menu = fb_point_in_rect(
                            cursor_x, cursor_y,
                            menu_x, menu_y, menu_width, 180
                        );

                        size_t fm_width = fb->width > 460
                            ? 420
                            : fb->width - 32;

                        size_t fm_height = fb->height > 320
                            ? 260
                            : fb->height - 80;

                        
                        
                        /* Determine which windows contain the click. */
                        size_t terminal_width = fb->width > 520
                            ? 480
                            : fb->width - 32;

                        size_t terminal_height = fb->height > 340
                            ? 280
                            : fb->height - 80;

                        bool click_on_terminal =
                            terminal_open &&
                            fb_point_in_rect(
                                cursor_x, cursor_y,
                                terminal_x, terminal_y,
                                terminal_width, terminal_height
                            );

                        bool click_on_file_manager =
                            file_manager_open &&
                            fb_point_in_rect(
                                cursor_x, cursor_y,
                                file_manager_x, file_manager_y,
                                fm_width, fm_height
                            );

                        /* Handle the frontmost window first. */
                        if (click_on_file_manager &&
                            (!click_on_terminal || file_manager_on_top)) {

                            file_manager_on_top = true;

                            /* Minimize File Manager. */
                            if (file_manager_minimize_button_hit(
                                    fb,
                                    file_manager_x,
                                    file_manager_y,
                                    cursor_x,
                                    cursor_y
                                )) {
                                file_manager_minimized = true;
                                file_manager_dragging = false;
                            }

                            /* Close File Manager. */
                            else if (fb_point_in_rect(
                                    cursor_x, cursor_y,
                                    file_manager_x + fm_width - 29,
                                    file_manager_y + 6,
                                    18, 18
                                )) {
                                file_manager_open = false;
                                file_manager_minimized = false;
                                file_manager_dragging = false;
                            }

                            /* Begin dragging File Manager. */
                            else if (fb_point_in_rect(
                                    cursor_x, cursor_y,
                                    file_manager_x, file_manager_y,
                                    fm_width, 32
                                )) {
                                file_manager_dragging = true;

                                file_manager_drag_offset_x =
                                    cursor_x - (int32_t)file_manager_x;

                                file_manager_drag_offset_y =
                                    cursor_y - (int32_t)file_manager_y;
                            }

                            /* Handle directory navigation. */
                            else {
                                int item = fs_item_at(
                                    fb,
                                    file_manager_x,
                                    file_manager_y,
                                    file_manager_directory,
                                    cursor_x,
                                    cursor_y
                                );

                                if (item == 0) {
                                    file_manager_directory = FS_ROOT;
                                } else if (item > 0) {
                                    size_t count = 0;

                                    const struct virtual_file *entries =
                                        fs_entries(file_manager_directory, &count);

                                    size_t index = (size_t)(item - 1);

                                    if (index < count &&
                                        entries[index].is_directory &&
                                        file_manager_directory == FS_ROOT) {

                                        if (index == 0) {
                                            file_manager_directory = FS_DOCUMENTS;
                                        } else if (index == 1) {
                                            file_manager_directory = FS_PROJECTS;
                                        } else if (index == 2) {
                                            file_manager_directory = FS_SYSTEM;
                                        }
                                    }
                                }
                            }
                        }

                        /* Handle Terminal if it is the selected window. */
                        else if (click_on_terminal) {

                            file_manager_on_top = false;

                            /* Minimize Terminal. */
                            if (terminal_minimize_button_hit(
                                    fb,
                                    terminal_x,
                                    terminal_y,
                                    cursor_x,
                                    cursor_y
                                )) {
                                terminal_minimized = true;
                                terminal_dragging = false;
                            }

                            /* Close Terminal. */
                            else if (terminal_close_button_hit(
                                    fb,
                                    terminal_x,
                                    terminal_y,
                                    cursor_x,
                                    cursor_y
                                )) {
                                terminal_open = false;
                                terminal_minimized = false;
                                terminal_dragging = false;
                                notice = NULL;
                            }

                            /* Begin dragging Terminal. */
                            else if (fb_point_in_rect(
                                    cursor_x, cursor_y,
                                    terminal_x, terminal_y,
                                    terminal_width, 32
                                )) {
                                terminal_dragging = true;

                                drag_offset_x =
                                    cursor_x - (int32_t)terminal_x;

                                drag_offset_y =
                                    cursor_y - (int32_t)terminal_y;
                            }
                        }

                        /* Taskbar: restore or focus Terminal. */
                        else if (on_terminal_taskbar && terminal_open) {
                            terminal_minimized = false;
                            file_manager_on_top = false;
                            start_menu_open = false;
                        }

                        /* Taskbar: restore or focus File Manager. */
                        else if (on_files_taskbar && file_manager_open) {
                            file_manager_minimized = false;
                            file_manager_on_top = true;
                            start_menu_open = false;
                        }

                        /* START button */
                        else if (on_start) {
                            start_menu_open = !start_menu_open;
                        }

                        /* START MENU selections */
                        else if (start_menu_open) {
                            int item = fb_start_menu_item_at(
                                fb, cursor_x, cursor_y
                            );

                            if (item == 0) {
                                /* Open Terminal */
                                terminal_open = true;
                                terminal_minimized = false;
                                file_manager_on_top = false;
                                terminal_x = 80;
                                terminal_y = 70;
                                terminal_input_length = 0;
                                terminal_input[0] = '\0';
                                notice = NULL;
                                start_menu_open = false;
                            } else if (item == 1) {
                                /* Open File Manager */
                                file_manager_open = true;
                                file_manager_minimized = false;
                                file_manager_on_top = true;
                                file_manager_directory = FS_ROOT;
                                file_manager_x = 120;
                                file_manager_y = 100;
                                notice = NULL;
                                start_menu_open = false;
                            } else if (item == 2) {
                                notice = "SETTINGS SELECTED";
                                start_menu_open = false;
                            } else if (!inside_menu) {
                                start_menu_open = false;
                            }
                        }

                        

                        /* Redraw the desktop and every open window. */
                        fb_draw_desktop(fb);

                        fb_draw_app_buttons(
                            fb,
                            terminal_open,
                            terminal_minimized,
                            file_manager_open,
                            file_manager_minimized,
                            file_manager_on_top
                        );

                        if (start_menu_open) {
                            fb_draw_start_menu(fb);
                        }

                        if (notice != NULL) {
                            fb_draw_status_notice(fb, notice);
                        }

                        
                        /* Draw the background window first. */
                        if (file_manager_on_top) {
                            if (terminal_open && !terminal_minimized) {
                                fb_draw_terminal(
                                    fb,
                                    terminal_x,
                                    terminal_y,
                                    terminal_input,
                                    terminal_output
                                );
                            }

                            if (file_manager_open && !file_manager_minimized) {
                                fb_draw_file_manager(
                                    fb,
                                    file_manager_x,
                                    file_manager_y,
                                    file_manager_directory
                                );
                            }
                        } else {
                            if (file_manager_open && !file_manager_minimized) {
                                fb_draw_file_manager(
                                    fb,
                                    file_manager_x,
                                    file_manager_y,
                                    file_manager_directory
                                );
                            }

                            if (terminal_open && !terminal_minimized) {
                                fb_draw_terminal(
                                    fb,
                                    terminal_x,
                                    terminal_y,
                                    terminal_input,
                                    terminal_output
                                );
                            }
                        }

                    }

                    /* Stop Terminal dragging after mouse release. */
                    if (!left_down) {
                        terminal_dragging = false;
                        file_manager_dragging = false;
                    }

                    /* Redraw windows while dragging Terminal. */
                    if (terminal_dragging && left_down) {
                        fb_draw_desktop(fb);

                        fb_draw_app_buttons(
                            fb,
                            terminal_open,
                            terminal_minimized,
                            file_manager_open,
                            file_manager_minimized,
                            file_manager_on_top
                        );

                        if (start_menu_open) {
                            fb_draw_start_menu(fb);
                        }

                        if (notice != NULL) {
                            fb_draw_status_notice(fb, notice);
                        }

                        if (terminal_open && !terminal_minimized) {
                            fb_draw_terminal(
                                fb,
                                terminal_x,
                                terminal_y,
                                terminal_input,
                                terminal_output
                            );
                        }

                        if (file_manager_open && !file_manager_minimized) {
                            fb_draw_file_manager(
                                fb,
                                file_manager_x,
                                file_manager_y,
                                file_manager_directory
                            );
                        }
                    }

                    if (file_manager_dragging && left_down) {
                        fb_draw_desktop(fb);

                        fb_draw_app_buttons(
                            fb,
                            terminal_open,
                            terminal_minimized,
                            file_manager_open,
                            file_manager_minimized,
                            file_manager_on_top
                        );

                        if (start_menu_open) {
                            fb_draw_start_menu(fb);
                        }

                        if (notice != NULL) {
                            fb_draw_status_notice(fb, notice);
                        }

                        if (terminal_open && !terminal_minimized) {
                            fb_draw_terminal(
                                fb,
                                terminal_x,
                                terminal_y,
                                terminal_input,
                                terminal_output
                            );
                        }

                        if (file_manager_open && !file_manager_minimized) {
                            fb_draw_file_manager(
                                fb,
                                file_manager_x,
                                file_manager_y,
                                file_manager_directory
                            );
                        }
                    }

                    

                    left_was_down = left_down;

                    /* Draw the cursor at its updated position. */
                    fb_draw_cursor(
                        fb,
                        (size_t)cursor_x,
                        (size_t)cursor_y
                    );
                }
            }
        }

        /* =========================
           KEYBOARD INPUT
           ========================= */
        uint8_t scancode;

        if (keyboard_read_scancode(&scancode)) {
            if (terminal_open && !terminal_minimized) {
                /* Ignore key-release scancodes. */
                if ((scancode & 0x80) == 0) {
                    if (scancode == 0x01) {
                        /* Escape closes Terminal. */
                        terminal_open = false;
                        terminal_dragging = false;

                        fb_draw_cursor(
                            fb,
                            (size_t)cursor_x,
                            (size_t)cursor_y
                        );

                        fb_draw_desktop(fb);

                        fb_draw_app_buttons(
                            fb,
                            terminal_open,
                            terminal_minimized,
                            file_manager_open,
                            file_manager_minimized,
                            file_manager_on_top
                        );

                        if (start_menu_open) {
                            fb_draw_start_menu(fb);
                        }

                        if (file_manager_open && !file_manager_minimized) {
                            fb_draw_file_manager(
                                fb,
                                file_manager_x,
                                file_manager_y,
                                file_manager_directory
                            );
                        }

                        fb_draw_cursor(
                            fb,
                            (size_t)cursor_x,
                            (size_t)cursor_y
                        );
                    } else if (scancode == 0x1C) {
                        /* Enter executes the Terminal command. */
                        terminal_input[terminal_input_length] = '\0';

                        terminal_execute(
                            terminal_input,
                            terminal_output,
                            sizeof(terminal_output)
                        );

                        terminal_input_length = 0;
                        terminal_input[0] = '\0';
                    } else if (scancode == 0x0E) {
                        /* Backspace removes the last character. */
                        if (terminal_input_length > 0) {
                            terminal_input_length--;

                            terminal_input[terminal_input_length] = '\0';
                        }
                    } else {
                        /* Convert the scancode into a character. */
                        char character =
                            keyboard_scancode_to_char(scancode);

                        if (character != '\0' &&
                            terminal_input_length <
                                sizeof(terminal_input) - 1) {
                            terminal_input[terminal_input_length++] =
                                character;

                            terminal_input[terminal_input_length] = '\0';
                        }
                    }
                }

                /* Refresh Terminal after keyboard input. */
                if (terminal_open && !terminal_minimized) {
                    fb_draw_cursor(
                        fb,
                        (size_t)cursor_x,
                        (size_t)cursor_y
                    );

                    fb_draw_desktop(fb);

                    fb_draw_app_buttons(
                        fb,
                        terminal_open,
                        terminal_minimized,
                        file_manager_open,
                        file_manager_minimized,
                        file_manager_on_top
                    );

                    if (start_menu_open) {
                        fb_draw_start_menu(fb);
                    }

                    if (notice != NULL) {
                        fb_draw_status_notice(fb, notice);
                    }

                    fb_draw_terminal(
                        fb,
                        terminal_x,
                        terminal_y,
                        terminal_input,
                        terminal_output
                    );

                    if (file_manager_open && !file_manager_minimized) {
                        fb_draw_file_manager(
                            fb,
                            file_manager_x,
                            file_manager_y,
                            file_manager_directory
                        );
                    }

                    fb_draw_cursor(
                        fb,
                        (size_t)cursor_x,
                        (size_t)cursor_y
                    );
                }
            } else {
                /* Keep the original desktop keyboard responses. */
                if (scancode == 0x1C) {
                    fb_draw_keyboard_status(fb, "ENTER PRESSED");
                } else if (scancode == 0x01) {
                    fb_draw_keyboard_status(fb, "ESC PRESSED");
                } else if (scancode == 0x39) {
                    fb_draw_keyboard_status(fb, "SPACE PRESSED");
                }
            }
        }

        asm volatile ("pause");
    }
}

