// Host display backend: 320x320 RGB565 software framebuffer -> SDL2
// window. The framebuffer's push_rect() contract is a blocking
// sub-rectangle upload, which maps directly onto SDL_UpdateTexture.
//
// fbdev path: when SDL runs on its "evdev" (or "dummy") video driver
// (SDL_VIDEODRIVER=evdev), pixels go straight to /dev/fb0 (override with
// GC_FBDEV) and SDL is used for keyboard input only. This avoids EGL
// entirely, which matters on images (e.g. Calculinux) where Mesa has no
// DRI driver for the ili9488_picocalc DRM device, so KMSDRM cannot init.
//
// The window is a normal desktop citizen (X11/fluxbox multitasking);
// there is no panel bring-up, no RGB666 conversion, no backlight —
// those live in the firmware's SPI transport.

#include "platform/display.hpp"

#include <SDL2/SDL.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

namespace platform {

namespace {

SDL_Window* g_win = nullptr;
SDL_Renderer* g_ren = nullptr;
SDL_Texture* g_tex = nullptr;

// fbdev backend state (null/zero unless the fbdev path is active).
uint8_t* g_fb = nullptr;
int g_fb_bpp = 0;
int g_fb_stride = 0;
int g_fb_w = 0;
int g_fb_h = 0;
fb_var_screeninfo g_fb_var{};

constexpr int kScale = 2;  // 8x16 font is small at 1:1 on desktop panels

// Integer window scale that fits the desktop. The device panel is
// 320x320, exactly the native screen — a 2x window there gets scaled
// back down by the WM anyway, and any non-integer size the WM lands on
// (fluxbox remembers geometry) feeds the 320x320 logical image through
// the renderer's resampler. That resampler must be nearest-neighbor
// (see the hint below), but the window itself should still fit, so the
// common case is a clean 1:1 blit.
int window_scale() {
    SDL_DisplayMode dm{};
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w >= kScreenW && dm.h >= kScreenH) {
        const int fit_w = dm.w / kScreenW;
        const int fit_h = dm.h / kScreenH;
        const int fit = fit_w < fit_h ? fit_w : fit_h;
        return fit < kScale ? fit : kScale;
    }
    return 1;
}

bool fb_wanted() {
    const char* drv = SDL_GetCurrentVideoDriver();
    return drv != nullptr && (SDL_strcmp(drv, "evdev") == 0 || SDL_strcmp(drv, "dummy") == 0);
}

void fb_open() {
    const char* dev = SDL_getenv("GC_FBDEV");
    if (dev == nullptr) {
        dev = "/dev/fb0";
    }
    const int fd = open(dev, O_RDWR);
    if (fd < 0) {
        SDL_Log("fbdev: cannot open %s", dev);
        return;
    }
    fb_fix_screeninfo fix{};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &g_fb_var) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        SDL_Log("fbdev: ioctl failed on %s", dev);
        close(fd);
        return;
    }
    void* p = mmap(nullptr, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        SDL_Log("fbdev: mmap failed on %s", dev);
        return;
    }
    g_fb = static_cast<uint8_t*>(p);
    g_fb_bpp = static_cast<int>(g_fb_var.bits_per_pixel);
    g_fb_stride = static_cast<int>(fix.line_length);
    g_fb_w = static_cast<int>(g_fb_var.xres);
    g_fb_h = static_cast<int>(g_fb_var.yres);
    SDL_Log("fbdev: %s %dx%d %dbpp", dev, g_fb_w, g_fb_h, g_fb_bpp);
}

// Same contract as push_rect: px is the band's first row, stride is a
// full screen row. 16bpp is a straight copy; 32bpp expands RGB565.
void fb_push(int x, int y, int w, int h, const uint16_t* px) {
    if (x >= g_fb_w || y >= g_fb_h) {
        return;
    }
    const int cw = (x + w > g_fb_w) ? g_fb_w - x : w;
    const int ch = (y + h > g_fb_h) ? g_fb_h - y : h;
    for (int r = 0; r < ch; ++r) {
        const uint16_t* src = px + r * kScreenW;
        uint8_t* row = g_fb + (y + r) * g_fb_stride;
        if (g_fb_bpp == 16) {
            std::memcpy(row + x * 2, src, static_cast<size_t>(cw) * 2);
        } else if (g_fb_bpp == 32) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(row) + x;
            for (int c = 0; c < cw; ++c) {
                const uint16_t p = src[c];
                const uint32_t r5 = (p >> 11) & 0x1F;
                const uint32_t g6 = (p >> 5) & 0x3F;
                const uint32_t b5 = p & 0x1F;
                const uint32_t r8 = (r5 << 3) | (r5 >> 2);
                const uint32_t g8 = (g6 << 2) | (g6 >> 4);
                const uint32_t b8 = (b5 << 3) | (b5 >> 2);
                dst[c] = (r8 << g_fb_var.red.offset) | (g8 << g_fb_var.green.offset) |
                         (b8 << g_fb_var.blue.offset);
            }
        }
    }
}

}  // namespace

void Display::init() {
    if (initialized_) {
        return;
    }
    // Nearest-neighbor scaling, set before anything creates a texture.
    // The 320x320 RGB565 image must never be bilinear-resampled: a
    // non-integer window size (WM fullscreen/resizes) then produces
    // alternating dim/bright columns — the "vertical interlace with
    // wrong colors" corruption — because adjacent texels get blended.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    // fbdev path: no SDL renderer at all. A window is still created so
    // SDL has a keyboard-focus target for evdev key events.
    if (fb_wanted()) {
        fb_open();
        if (g_fb == nullptr) {
            return;
        }
        g_win = SDL_CreateWindow("PicoCalc Graph", 0, 0, kScreenW, kScreenH, 0);
        if (g_win != nullptr) {
            SDL_RaiseWindow(g_win);
        }
        initialized_ = true;
        return;
    }

    // Force software-only window framebuffer under X11. Without this,
    // SDL_CreateWindowFramebuffer() (called during SDL_CreateRenderer)
    // tries to create an accelerated GLES renderer as a "texture
    // framebuffer" — see SDL_video.c SDL_CreateWindowTexture(). On
    // this device the GLES driver is emulated software (DRI3 error),
    // and the GLES renderer's ARGB8888 texture path corrupts the
    // 16-bit RGB565 rendering pipeline, producing alternating bright/
    // dark columns with wrong colors. KMSDRM does not have this
    // problem and must not disable the hint (it breaks the surface).
    if (SDL_GetCurrentVideoDriver() && SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    }

    const int scale = window_scale();
    // On a desktop that can't fit 2x (the device's own 320x320 panel),
    // go borderless-fullscreen: windowed, the WM titlebar pushes the
    // softkey bar past the bottom edge of the screen.
    const Uint32 fullscreen = (scale == 1) ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u;
    g_win = SDL_CreateWindow("PicoCalc Graph", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             kScreenW * scale, kScreenH * scale,
                             SDL_WINDOW_RESIZABLE | fullscreen);
    if (g_win == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }
    // Software renderer, pinned explicitly. The old SDL build had no GLES,
    // so asking for ACCELERATED silently fell back to software; with GLES
    // enabled (needed to compile in KMSDRM) the X11-EGL renderer gets
    // selected instead and its visual mismatches the window — vertical
    // interlace stripes and shifted colors. We rasterize RGB565 in
    // software anyway and 320x320 is trivial to blit, so software is the
    // right choice on every backend.
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (g_ren == nullptr) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(g_win);
        g_win = nullptr;
        return;
    }
    // Integer-ish scaling: renderer keeps 320x320 logical coordinates
    // regardless of the physical window size.
    SDL_RenderSetLogicalSize(g_ren, kScreenW, kScreenH);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, kScreenW,
                              kScreenH);
    if (g_tex == nullptr) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_DestroyRenderer(g_ren);
        SDL_DestroyWindow(g_win);
        g_ren = nullptr;
        g_win = nullptr;
        return;
    }
    initialized_ = true;
}

void Display::push_rect(int x, int y, int w, int h, const uint16_t* px) {
    if (!initialized_ || w <= 0 || h <= 0) {
        return;
    }
    if (g_fb != nullptr) {
        fb_push(x, y, w, h, px);
        return;
    }
    const SDL_Rect r{x, y, w, h};
    // px points at row (0) of the band; row stride is a full screen row.
    SDL_UpdateTexture(g_tex, &r, px, kScreenW * sizeof(uint16_t));
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}

void Display::push_rect_dma(int x, int y, int w, int h, const uint16_t* px) {
    push_rect(x, y, w, h, px);
}

void Display::set_backlight(uint8_t) {
    // No backlight on a window.
}

Display& display() {
    static Display instance;
    return instance;
}

}  // namespace platform
