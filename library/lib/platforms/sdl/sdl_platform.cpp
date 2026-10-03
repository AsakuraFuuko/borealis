/*
    Copyright 2023 xfangfang

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

#include <strings.h>
#include <unistd.h> /* chdir */

#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/platforms/sdl/sdl_platform.hpp>
#include <string>

#if defined(PS5)
extern "C" int sceSystemServiceParamGetInt(int paramId, int* value);
#endif

namespace
{
static std::string localeFromSDL(const SDL_Locale* locale)
{
    if (!locale || !locale->language)
        return {};

    const std::string language = locale->language;
    const std::string country  = locale->country ? locale->country : "";
    if (language == "zh" || language == "zh-CN" || language == "zh-Hans" || language == "zh-Hant")
        return country == "TW" || country == "HK" || country == "MO" || language == "zh-Hant"
                   ? brls::LOCALE_ZH_HANT
                   : brls::LOCALE_ZH_HANS;
    if (language == "ja")
        return brls::LOCALE_JA;
    if (language == "ko")
        return brls::LOCALE_Ko;
    if (language == "it")
        return brls::LOCALE_IT;
    if (language == "en")
        return brls::LOCALE_EN_US;
    return {};
}

#if defined(PS5)
static std::string localeFromPS5System(void)
{
    /* SDL's PS5 branch has no locale backend and otherwise falls back to English.
     * SystemService uses the same language parameter values as the PS4 SDK. */
    constexpr int SYSTEM_PARAM_ID_LANG = 0;
    constexpr int LANG_JAPANESE       = 0;
    constexpr int LANG_ITALIAN        = 5;
    constexpr int LANG_KOREAN         = 9;
    constexpr int LANG_CHINESE_T      = 10;
    constexpr int LANG_CHINESE_S      = 11;

    int language = -1;
    if (sceSystemServiceParamGetInt(SYSTEM_PARAM_ID_LANG, &language) < 0)
        return {};
    switch (language)
    {
    case LANG_JAPANESE: return brls::LOCALE_JA;
    case LANG_ITALIAN: return brls::LOCALE_IT;
    case LANG_KOREAN: return brls::LOCALE_Ko;
    case LANG_CHINESE_T: return brls::LOCALE_ZH_HANT;
    case LANG_CHINESE_S: return brls::LOCALE_ZH_HANS;
    default: return brls::LOCALE_EN_US;
    }
}
#endif
} // namespace

#if defined(IOS) || defined(TVOS)
#include <sys/utsname.h>

static bool isIPad()
{
    struct utsname systemInfo;
    uname(&systemInfo);
    return strncmp(systemInfo.machine, "iPad", 4) == 0;
}
#endif

namespace brls
{

SDLPlatform::SDLPlatform()
{
#ifdef ANDROID
    // Enable Fullscreen on Android
    VideoContext::FULLSCREEN = true;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
#elif defined(IOS) || defined(TVOS)
    // Enable Fullscreen on iOS
    VideoContext::FULLSCREEN = true;
    if (!isIPad())
    {
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    }
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
#elif defined(__APPLE__)
    // Same behavior as GLFW, change to the app's Resources directory if run in a ".app" bundle
    // Or to the executable's directory if run from other ways
    char *base_path = SDL_GetBasePath();
    if (base_path)
    {
        chdir(base_path);
        SDL_free(base_path);
    }
#endif
#ifdef PS5
    VideoContext::FULLSCREEN = true;
#if defined(BOREALIS_USE_AGC)
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
#elif defined(PS5_NATIVE_APP) && !defined(WILIWILI_SOFTWARE_RENDER)
    // Hardware native titles use the ps5-opengl EGL bridge (ps5-g19).
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "ps5-g19");
#elif defined(WILIWILI_SOFTWARE_RENDER)
    // OSMesa titles render into SDL's surface and present through the upstream
    // PS5 VideoOut backend (ps5), not the EGL/AGC bridge.
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "ps5");
#else
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "ps5");
#endif
#endif

    // Init sdl
    if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) < 0)
    {
        Logger::error("sdl: failed to initialize");
        return;
    }

    // Platform impls
    this->audioPlayer = new NullAudioPlayer();

    // Resolve AUTO from the console first; PS5 SDL itself has no locale backend.
    if (Platform::APP_LOCALE_DEFAULT == LOCALE_AUTO)
    {
        std::string detectedLocale;
#if defined(PS5)
        detectedLocale = localeFromPS5System();
#endif
        if (detectedLocale.empty())
        {
            SDL_Locale* locales = SDL_GetPreferredLocales();
            if (locales != nullptr)
            {
                detectedLocale = localeFromSDL(locales);
                SDL_free(locales);
            }
        }
        this->locale = detectedLocale.empty() ? LOCALE_EN_US : detectedLocale;
        brls::Logger::info("Set app locale: {}", this->locale);
    }
}

void SDLPlatform::createWindow(std::string windowTitle, uint32_t windowWidth, uint32_t windowHeight, float windowXPos, float windowYPos)
{
#if defined(BOREALIS_USE_AGC)
    windowWidth  = 1920;
    windowHeight = 1080;
    Application::setWindowSize(windowWidth, windowHeight);
    this->videoContext = new AgcVideoContext(windowWidth, windowHeight);
    this->inputManager = new SDLInputManager(nullptr);
#elif defined(PS5_NATIVE_APP)
    // The ps5-opengl SDL2 bridge owns exactly one fixed 1920x1080 EGL surface,
    // so the window and the UI geometry share that size.
    windowWidth  = 1920;
    windowHeight = 1080;
#endif
#if !defined(BOREALIS_USE_AGC)
    this->videoContext = new SDLVideoContext(windowTitle, windowWidth, windowHeight, windowXPos, windowYPos);
    this->inputManager = new SDLInputManager(this->videoContext->getSDLWindow());
#endif
    this->imeManager = new SDLImeManager(&this->otherEvent);
}

void SDLPlatform::restoreWindow()
{
#if !defined(BOREALIS_USE_AGC)
    SDL_RestoreWindow(this->videoContext->getSDLWindow());
#endif
}

void SDLPlatform::setWindowAlwaysOnTop(bool enable)
{
#if !defined(BOREALIS_USE_AGC)
    SDL_SetWindowAlwaysOnTop(this->videoContext->getSDLWindow(), enable ? SDL_TRUE : SDL_FALSE);
#else
    (void)enable;
#endif
}

void SDLPlatform::setWindowSize(uint32_t windowWidth, uint32_t windowHeight)
{
#if !defined(BOREALIS_USE_AGC)
    if (windowWidth > 0 && windowHeight > 0) {
        SDL_SetWindowSize(this->videoContext->getSDLWindow(), windowWidth, windowHeight);
    }
#else
    (void)windowWidth;
    (void)windowHeight;
#endif
}

void SDLPlatform::setWindowSizeLimits(uint32_t windowMinWidth, uint32_t windowMinHeight, uint32_t windowMaxWidth, uint32_t windowMaxHeight)
{
#if !defined(BOREALIS_USE_AGC)
    if (windowMinWidth > 0 && windowMinHeight > 0)
        SDL_SetWindowMinimumSize(this->videoContext->getSDLWindow(), windowMinWidth, windowMinHeight);
    if ((windowMaxWidth > 0 && windowMaxHeight > 0) && (windowMaxWidth > windowMinWidth && windowMaxHeight > windowMinHeight))
        SDL_SetWindowMaximumSize(this->videoContext->getSDLWindow(), windowMaxWidth, windowMaxHeight);
#else
    (void)windowMinWidth;
    (void)windowMinHeight;
    (void)windowMaxWidth;
    (void)windowMaxHeight;
#endif
}

void SDLPlatform::setWindowPosition(int windowXPos, int windowYPos)
{
#if !defined(BOREALIS_USE_AGC)
    SDL_SetWindowPosition(this->videoContext->getSDLWindow(), windowXPos, windowYPos);
#else
    (void)windowXPos;
    (void)windowYPos;
#endif
}

void SDLPlatform::setWindowState(uint32_t windowWidth, uint32_t windowHeight, int windowXPos, int windowYPos)
{
#if !defined(BOREALIS_USE_AGC)
    if (windowWidth > 0 && windowHeight > 0)
    {
        SDL_Window* win = this->videoContext->getSDLWindow();
        SDL_RestoreWindow(win);
        SDL_SetWindowSize(win, windowWidth, windowHeight);
        SDL_SetWindowPosition(win, windowXPos, windowYPos);
    }
#else
    (void)windowWidth;
    (void)windowHeight;
    (void)windowXPos;
    (void)windowYPos;
#endif
}

void SDLPlatform::disableScreenDimming(bool disable, const std::string& reason, const std::string& app)
{
    if (disable)
    {
        SDL_DisableScreenSaver();
    }
    else
    {
        SDL_EnableScreenSaver();
    }
}

bool SDLPlatform::isScreenDimmingDisabled()
{
    return !SDL_IsScreenSaverEnabled();
}

void SDLPlatform::pasteToClipboard(const std::string& text)
{
    SDL_SetClipboardText(text.c_str());
}

std::string SDLPlatform::pasteFromClipboard()
{
    char *str = SDL_GetClipboardText();
    if (!str)
        return "";
    return std::string{str};
}

std::string SDLPlatform::getName()
{
    return "SDL";
}

bool SDLPlatform::processEvent(SDL_Event* event)
{
    if (event->type == SDL_QUIT)
    {
        return false;
    }
    else if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP)
    {
        auto* manager = this->inputManager;
        if (manager)
            manager->updateKeyboardState(event->key);
    }
    else if (event->type == SDL_MOUSEMOTION)
    {
        auto* manager = this->inputManager;
        if (manager)
            manager->updateMouseMotion(event->motion);
    }
    else if (event->type == SDL_MOUSEWHEEL)
    {
        auto* manager = this->inputManager;
        if (manager)
            manager->updateMouseWheel(event->wheel);
    }
    else if (event->type == SDL_CONTROLLERSENSORUPDATE)
    {
        auto* manager = this->inputManager;
        if (manager)
            manager->updateControllerSensorsUpdate(event->csensor);
    }
#if defined(IOS) || defined(ANDROID)
    else if (event->type == SDL_APP_WILLENTERBACKGROUND)
    {
        brls::Application::getWindowFocusChangedEvent()->fire(false);
    }
    else if (event->type == SDL_APP_WILLENTERFOREGROUND)
    {
        brls::Application::getWindowFocusChangedEvent()->fire(true);

    }
#endif
    else if (event->type != SDL_POLLSENTINEL)
    {
        // 其它没有处理的事件
        this->otherEvent.fire(event);
    }
    brls::Application::setActiveEvent(true);
    return true;
}

bool SDLPlatform::mainLoopIteration()
{
    SDL_Event event;
    bool hasEvent = false;
    while (SDL_PollEvent(&event))
    {
        if (!processEvent(&event))
        {
            return false;
        }
        hasEvent = true;
    }
    if (!hasEvent && !Application::hasActiveEvent())
    {
        if (SDL_WaitEventTimeout(&event, (int)(brls::Application::getDeactivatedFrameTime() * 1000))
            && !processEvent(&event))
        {
            return false;
        }
    }
    return true;
}

AudioPlayer* SDLPlatform::getAudioPlayer()
{
    return this->audioPlayer;
}

VideoContext* SDLPlatform::getVideoContext()
{
    return this->videoContext;
}

InputManager* SDLPlatform::getInputManager()
{
    return this->inputManager;
}

ImeManager* SDLPlatform::getImeManager() {
    return this->imeManager;
}

SDLPlatform::~SDLPlatform()
{
    delete this->audioPlayer;
    delete this->inputManager;
    delete this->imeManager;
    delete this->videoContext;
}

} // namespace brls
