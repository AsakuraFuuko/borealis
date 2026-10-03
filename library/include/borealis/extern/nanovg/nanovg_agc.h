#pragma once

#include <nanovg.h>

NVGcontext* nvgCreateAgc(int width, int height);
void nvgDeleteAgc(NVGcontext* context);
