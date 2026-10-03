#include <borealis/extern/nanovg/agc/evo_agc_runtime.h>
#include <borealis/extern/nanovg/agc/evo_direct_mem.h>
#include <borealis/extern/nanovg/nanovg_agc.h>

#include <borealis/platforms/agc/agc_video.hpp>

extern "C" void wiliwili_frame_phase_clear(void);
namespace brls
{
AgcVideoContext::AgcVideoContext(uint32_t width, uint32_t height)
    : width_(width)
    , height_(height)
{
    evo_direct_mem_init(EVO_DIRECT_MEM_POOL_BYTES);
    if (evo_agc_runtime_init(static_cast<int>(width_), static_cast<int>(height_), 0) == 0)
        context_ = nvgCreateAgc(static_cast<int>(width_), static_cast<int>(height_));
}

AgcVideoContext::~AgcVideoContext()
{
    nvgDeleteAgc(context_);
    context_ = nullptr;
    evo_agc_runtime_shutdown();
    evo_direct_mem_shutdown();
}

NVGcontext* AgcVideoContext::getNVGContext() { return context_; }
void AgcVideoContext::clear(NVGcolor color)
{
    const auto byte = [](float value) -> uint32_t
    {
        if (value <= 0.0f)
            return 0;
        if (value >= 1.0f)
            return 255;
        return static_cast<uint32_t>(value * 255.0f + 0.5f);
    };
    const uint32_t r = byte(color.r);
    const uint32_t g = byte(color.g);
    const uint32_t b = byte(color.b);
    const uint32_t a = byte(color.a);
    evo_agc_runtime_clear_color(r | (g << 8) | (b << 16) | (a << 24));
    wiliwili_frame_phase_clear();
}
void AgcVideoContext::beginFrame() { evo_agc_runtime_frame_begin(); }
void AgcVideoContext::endFrame() { evo_agc_runtime_present(); }
void AgcVideoContext::setSwapInterval(int interval) { VideoContext::swapInterval = interval; }
void AgcVideoContext::resetState() { }
double AgcVideoContext::getScaleFactor() { return 1.0; }
void AgcVideoContext::fullScreen(bool) { }
}
