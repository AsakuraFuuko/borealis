/*
    Copyright 2021 natinusala
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

#include <unistd.h>

#include <borealis/core/application.hpp>
#include <borealis/core/assets.hpp>
#include <borealis/platforms/desktop/desktop_font.hpp>
#include <cstdlib>
#include <string>
#include <utility>
#ifdef USE_LIBROMFS
#include <romfs/romfs.hpp>
#endif

#define INTER_FONT_PATH BRLS_ASSET("font/switch_font.ttf")
#define INTER_ICON_PATH BRLS_ASSET("font/switch_icons.ttf")

#if defined(PS5)
extern "C" int wiliwili_read_file(const char* path, void** data, size_t* size);
#endif
namespace brls
{

const static std::vector<std::string> fontExts = {
    ".ttc",
    ".ttf",
    ".otf",
};

bool DesktopFontLoader::loadFontsExist(NVGcontext* vg, std::vector<std::string> fontPaths, std::string fontName, std::string fallbackFont)
{
    for (auto& fontPath : fontPaths)
    {
        for (auto& fontExt : fontExts)
        {
            std::string fullPath = fontPath + fontExt;
            if (access(fullPath.c_str(), F_OK) != -1)
            {
                this->loadFontFromFile(fontName, fullPath);
                if (!fallbackFont.empty())
                {
                    nvgAddFallbackFontId(vg, Application::getFont(fallbackFont), Application::getFont(fontName));
                }
                brls::Logger::info("Using {} font: {}", fontName, fullPath);
                return true;
            }
        }
    }
    return false;
}

bool DesktopFontLoader::loadFont(const std::string& name, const std::string& path)
{
#ifdef USE_LIBROMFS
    if (path.empty())
        return false;
    if (path.rfind("@res/", 0) == 0)
    {
        // font is inside the romfs
        try
        {
            auto& font = romfs::get(path.substr(5));
            if (font.valid() && Application::loadFontFromMemory(name, (void*)font.data(), font.size(), false))
                return true;
        }
        catch (...)
        {
        }
    }
    else
#endif
        if (access(path.c_str(), F_OK) != -1 && Application::loadFontFromFile(name, path))
    {
        return true;
    }

    return false;
}

static bool loadResourceFont(const std::string& name, const std::string& resourcePath)
{
#if defined(PS5)
    const char* root = std::getenv("WILIWILI_RES_DIR");
    if (root == nullptr || root[0] == '\0')
        root = "/app0/assets";
    const std::string path = std::string(root) + "/" + resourcePath;
    void* data             = nullptr;
    size_t size            = 0;
    return wiliwili_read_file(path.c_str(), &data, &size) == 0 && Application::loadFontFromMemory(name, data, size, true);
#elif defined(USE_LIBROMFS)
    try
    {
        const auto& font = romfs::get(resourcePath);
        if (font.valid() && Application::loadFontFromMemory(name, (void*)font.data(), font.size(), false))
            return true;
    }
    catch (...)
    {
    }
#else
    (void)name;
    (void)resourcePath;
#endif
    return false;
}

void DesktopFontLoader::loadFonts()
{
    NVGcontext* vg = brls::Application::getNVGContext();

    // Text font
    if (loadFont(FONT_REGULAR, USER_FONT_PATH))
    {
        // Using internal font as fallback
        if (loadFont("default", INTER_FONT_PATH))
        {
            nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont("default"));
        }
    }
    else
    {
        brls::Logger::warning("Cannot find custom font, (Searched at: {})", USER_FONT_PATH);
        brls::Logger::info("Trying to use internal font: {}", INTER_FONT_PATH);
        // PS5 title libc rejects fopen(/app0/assets/...), while open/read is
        // usable from the native shim. Keep the default font a real handle.
        if (!loadResourceFont(FONT_REGULAR, "font/switch_font.ttf"))
        {
            Logger::warning("Failed to load internal font, text may not be displayed");
        }
    }

    // The title has no system font. Register each shipped Noto face on the base
    // font; fontstash selects a fallback per missing Unicode glyph.
    const std::pair<const char*, const char*> fallbackFonts[] = {
        { "noto-kr", "font/noto-sans-kr.ttf" },
        { "noto-arabic", "font/noto-sans-arabic.ttf" },
        { "noto-thai", "font/noto-sans-thai.ttf" },
        { "noto-devanagari", "font/noto-sans-devanagari.ttf" },
        { "noto-myanmar", "font/noto-sans-myanmar.ttf" },
        { "noto-telugu", "font/noto-sans-telugu.ttf" },
        { "noto-tamil", "font/noto-sans-tamil.ttf" },
        { "noto-sinhala", "font/noto-sans-sinhala.ttf" },
        { "noto-hebrew", "font/noto-sans-hebrew.ttf" },
        { "noto-georgian", "font/noto-sans-georgian.ttf" },
        { "noto-armenian", "font/noto-sans-armenian.ttf" },
    };
    const int regularFont = Application::getFont(FONT_REGULAR);
    if (regularFont != FONT_INVALID)
    {
        for (const auto& [fontName, resourcePath] : fallbackFonts)
        {
            if (loadResourceFont(fontName, resourcePath))
            {
                const int fallbackFont = Application::getFont(fontName);
                if (!nvgAddFallbackFontId(vg, regularFont, fallbackFont))
                    Logger::warning("Could not attach font fallback: {}", fontName);
            }
            else
            {
                Logger::warning("Could not load font fallback: {}", resourcePath);
            }
        }
    }
    // Using system font as fallback
#if defined(__APPLE__) && !defined(IOS)
    std::vector<std::string> koreanFonts = {
        "/System/Library/Fonts/AppleSDGothicNeo",
    };
    std::vector<std::string> simplifiedChineseFonts;
    // {
    //     "/System/Library/Fonts/STHeiti Light", // 黑体
    //     "/System/Library/Fonts/Supplemental/Arial Unicode", // Arial Unicode
    //     "/System/Library/Fonts/Supplemental/Songti", // 宋体
    // };
#elif defined(_WIN32)
    std::string prefix = "C:\\Windows\\Fonts\\";
    char* winDir       = getenv("systemroot");
    if (winDir)
    {
        prefix = std::string { winDir } + "\\Fonts\\";
    }
    std::vector<std::string> koreanFonts = {
        prefix + "malgun",
    };
    std::vector<std::string> simplifiedChineseFonts = {
        prefix + "msyh",
    };
#elif defined(ANDROID)
    std::vector<std::string> koreanFonts;
    std::vector<std::string> simplifiedChineseFonts = {
        "/system/fonts/NotoSansCJK-Regular",
        "/system/fonts/DroidSansFallback",
        "/system/fonts/NotoSansSC-Regular",
        "/system/fonts/DroidSansChinese",
    };
#else
    std::vector<std::string> koreanFonts;
    std::vector<std::string> simplifiedChineseFonts;
#endif
    if (!simplifiedChineseFonts.empty())
    {
        loadFontsExist(vg, simplifiedChineseFonts, FONT_CHINESE_SIMPLIFIED, FONT_REGULAR);
    }
    if (!koreanFonts.empty())
    {
        loadFontsExist(vg, koreanFonts, FONT_KOREAN_REGULAR, FONT_REGULAR);
    }

    // Load Emoji
    if (loadFont(FONT_EMOJI, USER_EMOJI_PATH))
    {
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_EMOJI));
    }

    // bottom bar icons
    if (loadFont(FONT_SWITCH_ICONS, USER_ICON_PATH))
    {
        // User-provided icons
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_SWITCH_ICONS));
    }
    else
    {
        brls::Logger::warning("Cannot find custom icon, (Searched at: {})", USER_ICON_PATH);
        brls::Logger::info("Trying to use internal icon: {}", INTER_ICON_PATH);
        if (loadFont(FONT_SWITCH_ICONS, INTER_ICON_PATH))
        {
            // Internal icons
            nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_SWITCH_ICONS));
        }
        else
        {
            Logger::warning("Failed to load internal icons, bottom bar icons may not be displayed");
        }
    }

    // Material icons
    if (this->loadMaterialFromResources())
    {
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_MATERIAL_ICONS));
    }
    else
    {
        Logger::error("switch: could not load Material icons font from resources");
    }
}

} // namespace brls
