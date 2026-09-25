/*
    Copyright 2021 natinusala

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/platforms/sdl/sdl_video.hpp>

#if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
#include <cstdio>
extern "C" void wiliwili_boot_log(const char*);
/* Application hook: the engine drains one queued texture upload per frame so a
 * long list of covers cannot block the render loop. */
extern "C" void wiliwili_drain_image_uploads(void) __attribute__((weak));
/* Frame phase marks for the WILIWILI_TRACE build (native_shims.c); no-ops
 * otherwise. */
extern "C" void wiliwili_trace_mark(int slot);
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifdef BOREALIS_USE_OPENGL
#ifdef __PSV__
#include <GLES2/gl2.h>
extern "C"
{
#include <gpu_es4/psp2_pvr_hint.h>
#include <psp2/kernel/modulemgr.h>
}
#define NANOVG_GLES2_IMPLEMENTATION
#elif defined(PS4)
#include <orbis/Pigletv2VSH.h>
#define NANOVG_GLES2_IMPLEMENTATION
#else
#include <glad/glad.h>
#ifdef USE_GLES2
#define NANOVG_GLES2_IMPLEMENTATION
#elif USE_GLES3
#define NANOVG_GLES3_IMPLEMENTATION
#elif USE_GL2
#define NANOVG_GL2_IMPLEMENTATION
#else
#define NANOVG_GL3_IMPLEMENTATION
#endif
#endif
#include <nanovg_gl.h>
#elif defined(BOREALIS_USE_D3D11)
#include <nanovg_d3d11.h>

#include <borealis/platforms/driver/d3d11.hpp>
std::unique_ptr<brls::D3D11Context> D3D11_CONTEXT;
#endif

namespace brls
{

static double scaleFactor = 1.0;

static void sdlWindowFramebufferSizeCallback(SDL_Window* window, int width, int height)
{
    if (!width || !height)
        return;

    int fWidth, fHeight;
#ifdef BOREALIS_USE_OPENGL
    SDL_GL_GetDrawableSize(window, &fWidth, &fHeight);
    scaleFactor = fWidth * 1.0 / width;
#if defined(ANDROID)
    // On Android, doing this is to ensure that glViewport is called from the main thread
    brls::sync([fWidth, fHeight]()
        { glViewport(0, 0, fWidth, fHeight); });
#else
    glViewport(0, 0, fWidth, fHeight);
#endif
#elif defined(BOREALIS_USE_D3D11)
    fWidth      = width;
    fHeight     = height;
    scaleFactor = D3D11_CONTEXT->getScaleFactor();
    D3D11_CONTEXT->onFramebufferSize(fWidth, fHeight);
#endif

    Application::onWindowResized(fWidth, fHeight);

    if (!VideoContext::FULLSCREEN)
    {
        VideoContext::sizeW = width;
        VideoContext::sizeH = height;
    }
}

static void sdlWindowPositionCallback(SDL_Window* window, int windowXPos, int windowYPos)
{
    Application::onWindowReposition(windowXPos, windowYPos);

    if (!VideoContext::FULLSCREEN)
    {
        VideoContext::posX = (float)windowXPos;
        VideoContext::posY = (float)windowYPos;
    }
}

static int sdlWindowEventWatcher(void* data, SDL_Event* event)
{
    if (event->type == SDL_WINDOWEVENT)
    {
        SDL_Window* win = SDL_GetWindowFromID(event->window.windowID);
        switch (event->window.event)
        {
            case SDL_WINDOWEVENT_RESIZED:
                if (win == (SDL_Window*)data)
                {
                    sdlWindowFramebufferSizeCallback(win,
                        event->window.data1,
                        event->window.data2);
                }
                break;
            case SDL_WINDOWEVENT_MOVED:
                if (win == (SDL_Window*)data)
                {
                    sdlWindowPositionCallback(win,
                        event->window.data1,
                        event->window.data2);
                }
                break;
        }
    }
    return 0;
}

SDLVideoContext::SDLVideoContext(std::string windowTitle, uint32_t windowWidth, uint32_t windowHeight, float windowXPos, float windowYPos)
{
#ifdef __PSV__
#define MAX_PATH 256
    /// Huge thanks to SonicMastr for his kindness help and contribution in psv homebrew.

    windowWidth  = 960;
    windowHeight = 544;

    /* Disable Back Touchpad to prevent "misclicks" */
    SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);

    /* We need to use some custom hints */
    SDL_setenv("VITA_PVR_SKIP_INIT", "yeet", 1);

    PVRSRV_PSP2_APPHINT hint;
    char target_path[MAX_PATH];
    const char* default_path = "app0:module";

    /* Load Modules */
    sceKernelLoadStartModule("vs0:sys/external/libfios2.suprx", 0, NULL, 0, NULL, NULL);
    sceKernelLoadStartModule("vs0:sys/external/libc.suprx", 0, NULL, 0, NULL, NULL);
    snprintf(target_path, MAX_PATH, "%s/%s", default_path, "libgpu_es4_ext.suprx");
    sceKernelLoadStartModule(target_path, 0, NULL, 0, NULL, NULL);
    snprintf(target_path, MAX_PATH, "%s/%s", default_path, "libIMGEGL.suprx");
    sceKernelLoadStartModule(target_path, 0, NULL, 0, NULL, NULL);

    /* Set PVR Hints */
    PVRSRVInitializeAppHint(&hint);
    snprintf(hint.szGLES1, MAX_PATH, "%s/%s", default_path, "libGLESv1_CM.suprx");
    snprintf(hint.szGLES2, MAX_PATH, "%s/%s", default_path, "libGLESv2.suprx");
    snprintf(hint.szWindowSystem, MAX_PATH, "%s/%s", default_path, "libpvrPSP2_WSEGL.suprx");

    hint.ui32SwTexOpCleanupDelay = 32000; // Set to 32 milliseconds to prevent a pool of unfreed memory
    PVRSRVCreateVirtualAppHint(&hint);
#endif

#if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
    wiliwili_boot_log("sdl: SDL_Init(VIDEO) enter");
#endif
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
#if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
        {
            char line[192];
            std::snprintf(line, sizeof(line), "sdl: SDL_Init failed: %s", SDL_GetError());
            wiliwili_boot_log(line);
        }
#endif
        fatal(std::string("sdl: failed to initialize: ") + SDL_GetError());
    }
#if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
    {
        char line[192];
        std::snprintf(line, sizeof(line), "sdl: video driver=%s",
                      SDL_GetCurrentVideoDriver() != nullptr
                          ? SDL_GetCurrentVideoDriver()
                          : "(none)");
        wiliwili_boot_log(line);
    }
#endif

    // Create window
#if defined(PS5_NATIVE_APP)
    // The ps5-opengl SDL2 bridge owns exactly one fixed-size EGL surface and
    // rejects resize and high-DPI window flags.
    Uint32 windowFlags = SDL_WINDOW_SHOWN;
#else
    Uint32 windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
#ifdef BOREALIS_USE_OPENGL
#if defined(PS5_NATIVE_APP)
    // The ps5-opengl SDL2 bridge selects its EGL configuration from the GL
    // attributes that are set before the window is created, and it presents a
    // double buffered surface: without SDL_GL_DOUBLEBUFFER the draw buffer is
    // GL_FRONT, so every frame is drawn into a buffer that is never scanned out
    // and the display stays black. The SDK's own SDL sample sets exactly this
    // set of attributes.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#elif defined(__SWITCH__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#elif defined(__PSV__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#elif defined(USE_GLES2)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#elif defined(USE_GLES3)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#elif defined(USE_GL2)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#if defined(PS5_NATIVE_APP)
    // The ps5-opengl bridge derives its EGL configuration from these sizes.
    // Requesting any explicit channel layout (565 as the generic branch does,
    // or 8888) makes it pick a configuration whose channel order does not match
    // the scanout - red and blue come out swapped - so the choice is left to the
    // driver, exactly as the SDK's own SDL sample does.
#else
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
#endif
#if defined(USE_EGL)
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#endif
    windowFlags |= SDL_WINDOW_OPENGL;
#endif
    if (VideoContext::FULLSCREEN)
    {
#if defined(PS5_NATIVE_APP)
        // The bridge presents fullscreen through EGL and rejects window-level
        // fullscreen/resize flags.
#elif defined(__WINRT__)
        windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    }
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");

    if (std::isnan(windowXPos) || std::isnan(windowYPos))
    {
    #if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
    wiliwili_boot_log("sdl: SDL_CreateWindow enter");
#endif
    this->window = SDL_CreateWindow(windowTitle.c_str(),
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            windowWidth,
            windowHeight,
            windowFlags);
    }
    else
    {
        this->window = SDL_CreateWindow(windowTitle.c_str(),
            windowXPos > 0 ? windowXPos : SDL_WINDOWPOS_UNDEFINED,
            windowYPos > 0 ? windowYPos : SDL_WINDOWPOS_UNDEFINED,
            windowWidth,
            windowHeight,
            windowFlags);
    }

#if defined(PS5_NATIVE_APP)
    if (this->window)
    {
        int realisedWidth = 0, realisedHeight = 0;
        SDL_GetWindowSize(this->window, &realisedWidth, &realisedHeight);
        char realised[64];
        std::snprintf(realised, sizeof(realised), "sdl: window %dx%d", realisedWidth, realisedHeight);
        wiliwili_boot_log(realised);
    }
#endif
    if (!this->window)
    {
        fatal(std::string("sdl: failed to create window: ") + SDL_GetError());
    }
#ifdef BOREALIS_USE_OPENGL
#if defined(PS5_NATIVE_APP)
    // The ps5-opengl bridge validates the requested attributes at creation
    // time, so they are set immediately before the context is created.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    wiliwili_boot_log("sdl: requesting 3.3 core context");
#endif
    // Configure window
#if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
    wiliwili_boot_log("sdl: SDL_GL_CreateContext enter");
#endif
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context)
    {
        fatal(std::string("sdl: failed to create OpenGL context: ") + SDL_GetError());
    }
#if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
    wiliwili_boot_log("sdl: SDL_GL_MakeCurrent enter");
#endif
    if (SDL_GL_MakeCurrent(window, context) != 0)
    {
        fatal(std::string("sdl: failed to make OpenGL context current: ") + SDL_GetError());
    }
#if defined(PS5_NATIVE_APP)
    /* Present on the vertical blank: without it the scanout alternates between
     * a fresh and a half-written buffer, which reads as constant refreshing. */
    if (SDL_GL_SetSwapInterval(1) != 0)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "sdl: swap interval 1 refused: %s", SDL_GetError());
        wiliwili_boot_log(line);
    }
#endif
#endif
    SDL_AddEventWatch(sdlWindowEventWatcher, window);
#ifdef BOREALIS_USE_OPENGL
#if !defined(__PSV__) && !defined(PS4)
    // Load OpenGL routines using glad
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        fatal(std::string("sdl: failed to load OpenGL functions: ") + SDL_GetError());
    }
#endif

    Logger::info("sdl: GL Vendor: {}", (const char*)glGetString(GL_VENDOR));
    Logger::info("sdl: GL Renderer: {}", (const char*)glGetString(GL_RENDERER));
    Logger::info("sdl: GL Version: {}", (const char*)glGetString(GL_VERSION));
#if defined(PS5_NATIVE_APP)
    /* The title has no console output, so the GL identity is recorded in the
     * boot log: it is the first thing to check when nothing reaches the screen. */
    {
        char identity[192];
        std::snprintf(identity, sizeof(identity), "sdl: gl vendor=%s renderer=%s version=%s",
                      (const char*)glGetString(GL_VENDOR),
                      (const char*)glGetString(GL_RENDERER),
                      (const char*)glGetString(GL_VERSION));
        wiliwili_boot_log(identity);
    }
#endif

    // Initialize nanovg
#ifdef __PSV__
    this->nvgContext = nvgCreateGLES2(0);
#elif PS4
    // Same as GLES2, but with pre-compiled shaders, so the flags must be "NVG_STENCIL_STROKES | NVG_ANTIALIAS" now
    this->nvgContext = nvgCreateGLES2(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#elif USE_GLES2
    this->nvgContext = nvgCreateGLES2(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#elif USE_GLES3
    this->nvgContext = nvgCreateGLES3(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#elif USE_GL2
    this->nvgContext = nvgCreateGL2(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#else
    #if defined(PS5_NATIVE_APP)
    // The native GL driver charges a full draw call per nanovg path, and
    // antialiasing adds a fringe path to every shape: it is disabled here so a
    // frame issues roughly half as many calls.
    this->nvgContext = nvgCreateGL3(0);
#else
this->nvgContext = nvgCreateGL3(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#endif
#endif
#elif defined(BOREALIS_USE_D3D11)
    Logger::info("sdl: USE_D3D11");
    D3D11_CONTEXT    = std::make_unique<D3D11Context>(this->window, windowWidth, windowHeight);
    this->nvgContext = nvgCreateD3D11(D3D11_CONTEXT->getDevice(), NVG_ANTIALIAS | NVG_STENCIL_STROKES);
#endif
    if (!this->nvgContext)
    {
        brls::fatal("sdl: unable to init nanovg");
    }

    setSwapInterval(VideoContext::swapInterval);

    // Setup window state
    int width, height;
    SDL_GetWindowSize(window, &width, &height);

    int fWidth, fHeight;
#ifdef BOREALIS_USE_OPENGL
    SDL_GL_GetDrawableSize(window, &fWidth, &fHeight);
    scaleFactor = fWidth * 1.0 / width;
    Application::setWindowSize(fWidth, fHeight);
    glViewport(0, 0, fWidth, fHeight);
#elif defined(BOREALIS_USE_D3D11)
    scaleFactor      = D3D11_CONTEXT->getScaleFactor();
    fWidth           = width;
    fHeight          = height;
    Application::setWindowSize(fWidth, fHeight);
    D3D11_CONTEXT->onFramebufferSize(fWidth, fHeight);
#endif

    int xPos, yPos;
    SDL_GetWindowPosition(window, &xPos, &yPos);
    Application::setWindowPosition(xPos, yPos);

    if (!VideoContext::FULLSCREEN)
    {
        VideoContext::sizeW = width;
        VideoContext::sizeH = height;
        VideoContext::posX  = (float)xPos;
        VideoContext::posY  = (float)yPos;
    }
}

void SDLVideoContext::beginFrame()
{
#if defined(PS5_NATIVE_APP) || defined(WILIWILI_SOFTWARE_RENDER)
    wiliwili_trace_mark(0);
#endif
#ifdef BOREALIS_USE_OPENGL
    /* The default draw buffer matches what the bridge flushes on swap; drawing
     * into both buffers would rasterise every frame twice. */
#endif
#if defined(BOREALIS_USE_D3D11)
    D3D11_CONTEXT->beginFrame();
#endif
}

void SDLVideoContext::endFrame()
{
#ifdef BOREALIS_USE_OPENGL
#if defined(PS5_NATIVE_APP)
    /* Pace before presenting. The bridge keeps a two deep flip pipeline and
     * ignores the requested swap interval, so presenting faster than the panel
     * fills that pipeline and the fourth present blocks forever. */
    {
        static std::chrono::steady_clock::time_point next_frame =
            std::chrono::steady_clock::now();
        next_frame += std::chrono::microseconds(16667);
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (next_frame > now)
        {
            std::this_thread::sleep_for(next_frame - now);
        }
        else
        {
            next_frame = now;
        }
    }
    if (wiliwili_drain_image_uploads != nullptr)
    {
        wiliwili_drain_image_uploads();
    }
    wiliwili_trace_mark(2);
    /* Submit the frame without waiting: glFinish() serialises the whole
     * pipeline every frame and costs more than it buys here. */
    glFlush();
    SDL_GL_SwapWindow(this->window);
    wiliwili_trace_mark(3);
    return;
#endif
    SDL_GL_SwapWindow(this->window);
#elif defined(BOREALIS_USE_D3D11)
    D3D11_CONTEXT->endFrame();
#endif
}

void SDLVideoContext::setSwapInterval(int interval)
{
    VideoContext::swapInterval = interval;
#ifdef BOREALIS_USE_OPENGL
    SDL_GL_SetSwapInterval(interval);
#elif defined(BOREALIS_USE_D3D11)
    D3D11_CONTEXT->setSwapInterval(interval);
#endif
}

void SDLVideoContext::clear(NVGcolor color)
{
#ifdef BOREALIS_USE_OPENGL
#if defined(PS5_NATIVE_APP) && !defined(WILIWILI_SOFTWARE_RENDER)
    // Same reason as the nanovg output swizzle: the ps5-opengl scanout is
    // displayed BGRA, while the software renderer's OSMesa buffer is presented
    // by the ps5 video driver, which takes plain RGBA.
    glClearColor(color.b, color.g, color.r, color.a);
#else
    glClearColor(
        color.r,
        color.g,
        color.b,
        color.a);
#endif

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
#elif defined(BOREALIS_USE_D3D11)
    D3D11_CONTEXT->clear(nvgRGBAf(
        color.r,
        color.g,
        color.b,
        color.a));
#endif
}

void SDLVideoContext::resetState()
{
#ifdef BOREALIS_USE_OPENGL
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
#endif
}

double SDLVideoContext::getScaleFactor()
{
    return scaleFactor;
}

SDLVideoContext::~SDLVideoContext()
{
    try
    {
        if (this->nvgContext)
        {
#ifdef BOREALIS_USE_OPENGL
#ifdef USE_GLES2
            nvgDeleteGLES2(this->nvgContext);
#elif USE_GLES3
            nvgDeleteGLES3(this->nvgContext);
#elif USE_GL2
            nvgDeleteGL2(this->nvgContext);
#else
            nvgDeleteGL3(this->nvgContext);
#endif
#elif defined(BOREALIS_USE_D3D11)
            nvgDeleteD3D11(this->nvgContext);
            D3D11_CONTEXT = nullptr;
#endif
        }
    }
    catch (...)
    {
        Logger::error("Cannot delete nvg Context");
    }
    SDL_DestroyWindow(this->window);
    SDL_Quit();
}

NVGcontext* SDLVideoContext::getNVGContext()
{
    return this->nvgContext;
}

SDL_Window* SDLVideoContext::getSDLWindow()
{
    return this->window;
}

void SDLVideoContext::fullScreen(bool fs)
{
#ifdef __WINRT__
    // win32 会很模糊，而且点击事件貌似也错位了，只给 winrt 使用。
    static unsigned int flag = SDL_WINDOW_FULLSCREEN;
#else
    static unsigned int flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    SDL_SetWindowFullscreen(this->window, fs ? flag : 0);
}

} // namespace brls
