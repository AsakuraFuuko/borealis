#pragma once

#include <borealis/core/video.hpp>

namespace brls {
class AgcVideoContext final : public VideoContext {
  public:
    AgcVideoContext(uint32_t width, uint32_t height);
    ~AgcVideoContext() override;
    NVGcontext* getNVGContext() override;
    void clear(NVGcolor color) override;
    void beginFrame() override;
    void endFrame() override;
    void setSwapInterval(int interval) override;
    void resetState() override;
    double getScaleFactor() override;
    void fullScreen(bool fs) override;

  private:
    NVGcontext* context_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};
}
