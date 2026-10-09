#include <borealis/extern/nanovg/nanovg_agc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "agc/evo_agc_runtime.h"
#include "agc/evo_agc_transient_ring.h"
#include "agc/evo_agc_writer.h"
#include "agc/evo_direct_mem.h"

namespace
{
struct Vertex
{
    float x, y;
    uint32_t color;
    float u, v;
};
struct Texture
{
    int id;
    int type;
    int width;
    int height;
    int flags;
    uint32_t pitch;
    void* raw;
    uint8_t* pixels;
    uint32_t descriptor[EVO_AGC_COMBINED_DESCRIPTOR_DWORDS];
};
struct State
{
    int width;
    int height;
    int next_id;
    Texture* white;
    std::vector<Texture*> textures;
    std::vector<Texture*> retired_textures;
};

static void releaseTexture(Texture* texture)
{
    if (!texture)
        return;
    evo_direct_mem_free(texture->raw);
    delete texture;
}

static void reapRetired(State* state)
{
    if (!state || state->retired_textures.empty())
        return;

    /* A deleted texture may still be referenced by a submitted DCB. Wait for
     * every frame fence before returning its direct-memory pages to the pool;
     * otherwise the next SVG/cache allocation can reuse GPU-visible pages while
     * the previous draw is still reading them. */
    evo_agc_runtime_wait_idle(250);
    for (Texture* texture : state->retired_textures)
        releaseTexture(texture);
    state->retired_textures.clear();
}

static uint32_t color(NVGcolor c)
{
    const auto byte = [](float v)
    { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const float a = std::clamp(c.a, 0.0f, 1.0f);
    return byte(c.r * a) | (byte(c.g * a) << 8) | (byte(c.b * a) << 16) | (byte(a) << 24);
}

static uint32_t scaleColor(uint32_t packed, float coverage)
{
    const auto scale = [coverage](uint32_t channel)
    {
        return static_cast<uint32_t>(static_cast<float>(channel) * coverage + 0.5f);
    };
    return scale(packed & 0xffu) | (scale((packed >> 8) & 0xffu) << 8) | (scale((packed >> 16) & 0xffu) << 16) | (scale((packed >> 24) & 0xffu) << 24);
}

static uint32_t paintColor(const NVGpaint* paint, float x, float y)
{
    if (!paint)
        return 0;

    const float dx = paint->innerColor.r - paint->outerColor.r;
    const float dy = paint->innerColor.g - paint->outerColor.g;
    const float dz = paint->innerColor.b - paint->outerColor.b;
    const float da = paint->innerColor.a - paint->outerColor.a;
    if (std::abs(dx) < 0.0001f && std::abs(dy) < 0.0001f && std::abs(dz) < 0.0001f && std::abs(da) < 0.0001f)
        return color(paint->innerColor);

    /* NanoVG's GL backend evaluates the paint in inverse-transform space;
     * using paint->xform directly collapses vertical_linear overlays to a
     * nearly uniform colour after the AGC path receives screen-space vertices. */
    float invxform[6];
    if (!nvgTransformInverse(invxform, paint->xform))
        return color(paint->innerColor);
    const float px       = x * invxform[0] + y * invxform[2] + invxform[4];
    const float py       = x * invxform[1] + y * invxform[3] + invxform[5];
    const float radius   = std::max(0.0f, paint->radius);
    const float ex       = std::max(0.0001f, paint->extent[0] - radius);
    const float ey       = std::max(0.0001f, paint->extent[1] - radius);
    const float qx       = std::abs(px) - ex;
    const float qy       = std::abs(py) - ey;
    const float outside  = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
    const float distance = std::min(std::max(qx, qy), 0.0f) + outside - radius;
    const float feather  = std::max(0.0001f, paint->feather);
    const float factor   = std::clamp((distance + feather * 0.5f) / feather, 0.0f, 1.0f);

    NVGcolor result;
    result.r = paint->innerColor.r + (paint->outerColor.r - paint->innerColor.r) * factor;
    result.g = paint->innerColor.g + (paint->outerColor.g - paint->innerColor.g) * factor;
    result.b = paint->innerColor.b + (paint->outerColor.b - paint->innerColor.b) * factor;
    result.a = paint->innerColor.a + (paint->outerColor.a - paint->innerColor.a) * factor;
    return color(result);
}

static Texture* findTexture(State* state, int id)
{
    for (Texture* texture : state->textures)
        if (texture->id == id)
            return texture;
    return state->white;
}

static constexpr size_t textureSourceBpp(const Texture* texture)
{
    return texture->type == NVG_TEXTURE_ALPHA ? 1u : 4u;
}

static constexpr size_t textureGpuBpp(const Texture* texture)
{
    return texture->type == NVG_TEXTURE_ALPHA ? 1u : 4u;
}

static inline uint8_t premultiply(uint8_t value, uint8_t alpha)
{
    return static_cast<uint8_t>((static_cast<unsigned>(value) * alpha + 127u) / 255u);
}

static int uploadTexture(Texture* texture, const unsigned char* data)
{
    const size_t source_bpp = textureSourceBpp(texture);
    const size_t row        = static_cast<size_t>(texture->width) * source_bpp;
    for (int y = 0; y < texture->height; ++y)
    {
        uint8_t* dst = texture->pixels + static_cast<size_t>(y) * texture->pitch;
        if (!data)
        {
            std::memset(dst, 0, texture->pitch);
        }
        else if (texture->type == NVG_TEXTURE_ALPHA)
        {
            std::memcpy(dst, data + static_cast<size_t>(y) * row, row);
        }
        else if ((texture->flags & NVG_IMAGE_PREMULTIPLIED) != 0)
        {
            std::memcpy(dst, data + static_cast<size_t>(y) * row, row);
        }
        else
        {
            const uint8_t* src = data + static_cast<size_t>(y) * row;
            for (int x = 0; x < texture->width; ++x)
            {
                const size_t offset = static_cast<size_t>(x) * 4u;
                dst[offset + 0]     = premultiply(src[offset + 0], src[offset + 3]);
                dst[offset + 1]     = premultiply(src[offset + 1], src[offset + 3]);
                dst[offset + 2]     = premultiply(src[offset + 2], src[offset + 3]);
                dst[offset + 3]     = src[offset + 3];
            }
        }
    }
    evo_agc_runtime_cache_flush(texture->pixels, static_cast<size_t>(texture->pitch) * texture->height);
    const int rc = texture->type == NVG_TEXTURE_ALPHA
        ? evo_agc_build_tsharp_r8(texture->descriptor,
              reinterpret_cast<uint64_t>(texture->pixels),
              texture->width, texture->height, texture->pitch)
        : evo_agc_build_tsharp_rgba8(texture->descriptor,
              reinterpret_cast<uint64_t>(texture->pixels),
              texture->width, texture->height, texture->pitch);
    if (rc != 0)
        return 0;
    return evo_agc_build_ssharp(texture->descriptor + EVO_AGC_TSHARP_DWORDS, 1, (texture->flags & NVG_IMAGE_NEAREST) == 0) == 0;
}

static Texture* createTexture(State* state, int type, int width, int height, int flags, const unsigned char* data)
{
    if (width <= 0 || height <= 0)
        return nullptr;
    reapRetired(state);
    auto* texture      = new Texture {};
    texture->id        = state->next_id++;
    texture->type      = type;
    texture->width     = width;
    texture->height    = height;
    texture->flags     = flags;
    texture->pitch     = (static_cast<uint32_t>(width) * textureGpuBpp(texture) + 255u) & ~255u;
    const size_t bytes = static_cast<size_t>(texture->pitch) * height;
    texture->raw       = evo_direct_mem_alloc(bytes + 255u);
    if (!texture->raw)
    {
        delete texture;
        return nullptr;
    }
    texture->pixels = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(texture->raw) + 255u) & ~uintptr_t(255u));
    if (!uploadTexture(texture, data))
    {
        evo_direct_mem_free(texture->raw);
        delete texture;
        return nullptr;
    }
    state->textures.push_back(texture);
    return texture;
}

enum DrawTopology
{
    DRAW_LIST,
    DRAW_STRIP,
    DRAW_FAN
};

static bool imageUv(const Texture* texture, const NVGpaint* paint, float x, float y, float* u, float* v)
{
    if (!texture || !paint || !u || !v || paint->extent[0] == 0.0f || paint->extent[1] == 0.0f)
        return false;

    float invxform[6];
    if ((texture->flags & NVG_IMAGE_FLIPY) != 0)
    {
        float m1[6], m2[6];
        nvgTransformTranslate(m1, 0.0f, paint->extent[1] * 0.5f);
        nvgTransformMultiply(m1, paint->xform);
        nvgTransformScale(m2, 1.0f, -1.0f);
        nvgTransformMultiply(m2, m1);
        nvgTransformTranslate(m1, 0.0f, -paint->extent[1] * 0.5f);
        nvgTransformMultiply(m1, m2);
        nvgTransformInverse(invxform, m1);
    }
    else
    {
        nvgTransformInverse(invxform, paint->xform);
    }

    const float imageX = x * invxform[0] + y * invxform[2] + invxform[4];
    const float imageY = x * invxform[1] + y * invxform[3] + invxform[5];
    *u                 = imageX / paint->extent[0];
    *v                 = imageY / paint->extent[1];
    return true;
}

static bool draw(State* state, const NVGpaint* paint, const NVGscissor* scissor,
    const NVGvertex* source, int count, DrawTopology topology, float strokeMult)
{
    if (count < 3 || count > 65535)
        return false;
    const int index_count = topology == DRAW_LIST ? count : (count - 2) * 3;
    auto* cb              = evo_agc_runtime_get_current_cb();
    auto* ring            = evo_agc_runtime_get_transient_ring();
    if (!cb || !ring)
        return false;
    const uint32_t slot = evo_agc_runtime_get_current_slot();
    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);
    int width = state->width, height = state->height;
    if (scissor && scissor->extent[0] >= 0.0f && scissor->extent[1] >= 0.0f)
    {
        /* NVG stores the scissor centre in transformed coordinates but keeps
         * extent in local coordinates. The old code subtracted the raw extent,
         * so the 1.5x 1280->1920 UI scale clipped every view to a smaller centre
         * rectangle. Transform both rectangle axes into an axis-aligned bound. */
        const float half_x = std::abs(scissor->extent[0] * scissor->xform[0]) + std::abs(scissor->extent[1] * scissor->xform[2]);
        const float half_y = std::abs(scissor->extent[0] * scissor->xform[1]) + std::abs(scissor->extent[1] * scissor->xform[3]);
        const int left     = std::max(0, static_cast<int>(std::floor(scissor->xform[4] - half_x)));
        const int top      = std::max(0, static_cast<int>(std::floor(scissor->xform[5] - half_y)));
        const int right    = std::min(width, static_cast<int>(std::ceil(scissor->xform[4] + half_x)));
        const int bottom   = std::min(height, static_cast<int>(std::ceil(scissor->xform[5] + half_y)));
        evo_agc_runtime_set_scissor(left, top, std::max(0, right - left), std::max(0, bottom - top));
    }
    else
    {
        evo_agc_runtime_set_scissor(0, 0, width, height);
    }
    evo_agc_transient_slice_t constants, constant_desc, vertices, vertex_desc, indices, texture_desc;
    if (evo_agc_transient_ring_alloc(ring, slot, 128, 16, &constants) != 0 || evo_agc_transient_ring_alloc(ring, slot, 16, 16, &constant_desc) != 0 || evo_agc_transient_ring_alloc(ring, slot, static_cast<size_t>(count) * sizeof(Vertex), 16, &vertices) != 0 || evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vertex_desc) != 0 || evo_agc_transient_ring_alloc(ring, slot, static_cast<size_t>(index_count) * sizeof(uint16_t), 16, &indices) != 0 || evo_agc_transient_ring_alloc(ring, slot, EVO_AGC_COMBINED_DESCRIPTOR_DWORDS * sizeof(uint32_t), 16, &texture_desc) != 0)
    {
        evo_agc_runtime_note_drop(0);
        return false;
    }
    Texture* texture = findTexture(state, paint->image);
    std::memset(constants.cpu, 0, 128);
    auto* projection = static_cast<float*>(constants.cpu);
    projection[0]    = 2.0f / static_cast<float>(width);
    projection[5]    = -2.0f / static_cast<float>(height);
    projection[10] = projection[15] = 1.0f;
    projection[12]                  = -1.0f;
    projection[13]                  = 1.0f;
    auto* dst                       = static_cast<Vertex*>(vertices.cpu);
    const bool gradient             = paint->image == 0;
    const bool imageFill            = topology != DRAW_LIST && paint->image != 0 && texture != state->white;
    for (int i = 0; i < count; ++i)
    {
        const uint32_t tint = gradient ? paintColor(paint, source[i].x, source[i].y)
                                       : color(paint->innerColor);
        dst[i]              = Vertex { source[i].x, source[i].y, tint, source[i].u, source[i].v };
        if (imageFill)
            imageUv(texture, paint, source[i].x, source[i].y, &dst[i].u, &dst[i].v);
        if (strokeMult > 0.0f)
        {
            const float coverage = std::clamp((1.0f - std::abs(source[i].u * 2.0f - 1.0f)) * strokeMult, 0.0f, 1.0f) * std::clamp(source[i].v, 0.0f, 1.0f);
            dst[i].color         = scaleColor(dst[i].color, coverage);
        }
    }
    auto* index = static_cast<uint16_t*>(indices.cpu);
    if (topology == DRAW_LIST)
    {
        for (int i = 0; i < count; ++i)
            index[i] = static_cast<uint16_t>(i);
    }
    else
    {
        /* NanoVG supplies fill paths as triangle fans and stroke fringes as
         * triangle strips, while the AGC UI pipeline is compiled as
         * TRIANGLE_LIST. Expand each primitive explicitly. */
        for (int i = 0; i < count - 2; ++i)
        {
            const int base = i * 3;
            if (topology == DRAW_FAN)
            {
                index[base + 0] = 0;
                index[base + 1] = static_cast<uint16_t>(i + 1);
                index[base + 2] = static_cast<uint16_t>(i + 2);
            }
            else if ((i & 1) == 0)
            {
                index[base + 0] = static_cast<uint16_t>(i);
                index[base + 1] = static_cast<uint16_t>(i + 1);
                index[base + 2] = static_cast<uint16_t>(i + 2);
            }
            else
            {
                index[base + 0] = static_cast<uint16_t>(i + 1);
                index[base + 1] = static_cast<uint16_t>(i);
                index[base + 2] = static_cast<uint16_t>(i + 2);
            }
        }
    }
    evo_agc_build_constant_vsharp(static_cast<uint32_t*>(constant_desc.cpu), constants.gpu_addr, 128);
    evo_agc_build_vsharp(static_cast<uint32_t*>(vertex_desc.cpu), vertices.gpu_addr, sizeof(Vertex), count);
    std::memcpy(texture_desc.cpu, texture->descriptor, EVO_AGC_COMBINED_DESCRIPTOR_DWORDS * sizeof(uint32_t));
    const auto layout = evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI);
    uint32_t vs[16] = {}, ps[16] = {};
    if (layout.vs_const_table_dword < 0 || layout.vs_vertex_table_dword < 0 || layout.ps_texture_table_dword < 0)
        return false;
    vs[layout.vs_const_table_dword]   = static_cast<uint32_t>(constant_desc.gpu_addr);
    vs[layout.vs_vertex_table_dword]  = static_cast<uint32_t>(vertex_desc.gpu_addr);
    ps[layout.ps_texture_table_dword] = static_cast<uint32_t>(texture_desc.gpu_addr);
    evo_agc_writer_set_user_data_gs(cb, vs, layout.vs_count);
    evo_agc_writer_set_user_data_ps(cb, ps, layout.ps_count);
    if (evo_agc_writer_draw_index(cb, static_cast<uint32_t>(index_count), static_cast<const uint16_t*>(indices.cpu)) != 0)
        return false;
    evo_agc_runtime_note_draw();
    evo_agc_runtime_note_ui_drawn();
    return true;
}

static bool sameVertex(const NVGvertex& a, const NVGvertex& b)
{
    return std::abs(a.x - b.x) < 0.00001f && std::abs(a.y - b.y) < 0.00001f && std::abs(a.u - b.u) < 0.00001f && std::abs(a.v - b.v) < 0.00001f;
}

static int clipTriangleU(const NVGvertex triangle[3], float boundary, bool keepGreater, NVGvertex output[6])
{
    int count   = 0;
    auto append = [&](const NVGvertex& vertex)
    {
        if (count == 0 || !sameVertex(output[count - 1], vertex))
            output[count++] = vertex;
    };
    NVGvertex previous  = triangle[2];
    bool previousInside = keepGreater ? previous.u >= boundary : previous.u <= boundary;
    for (int i = 0; i < 3; ++i)
    {
        const NVGvertex current  = triangle[i];
        const bool currentInside = keepGreater ? current.u >= boundary : current.u <= boundary;
        if (currentInside != previousInside)
        {
            const float amount = (boundary - previous.u) / (current.u - previous.u);
            append(NVGvertex {
                previous.x + (current.x - previous.x) * amount,
                previous.y + (current.y - previous.y) * amount,
                boundary,
                previous.v + (current.v - previous.v) * amount,
            });
        }
        if (currentInside)
            append(current);
        previous       = current;
        previousInside = currentInside;
    }
    if (count > 1 && sameVertex(output[0], output[count - 1]))
        --count;
    return count;
}

static std::vector<NVGvertex> splitStrokeCoverage(const NVGvertex* strip, int count)
{
    std::vector<NVGvertex> triangles;
    if (!strip || count < 3)
        return triangles;
    triangles.reserve(static_cast<size_t>(count - 2) * 6);
    for (int i = 0; i < count - 2; ++i)
    {
        NVGvertex triangle[3];
        if ((i & 1) == 0)
        {
            triangle[0] = strip[i];
            triangle[1] = strip[i + 1];
        }
        else
        {
            triangle[0] = strip[i + 1];
            triangle[1] = strip[i];
        }
        triangle[2]         = strip[i + 2];
        const float minimum = std::min({ triangle[0].u, triangle[1].u, triangle[2].u });
        const float maximum = std::max({ triangle[0].u, triangle[1].u, triangle[2].u });
        if (!(minimum < 0.5f && maximum > 0.5f))
        {
            triangles.insert(triangles.end(), triangle, triangle + 3);
            continue;
        }

        for (bool keepGreater : { false, true })
        {
            NVGvertex clipped[6];
            const int clippedCount = clipTriangleU(triangle, 0.5f, keepGreater, clipped);
            for (int j = 1; j + 1 < clippedCount; ++j)
            {
                if (triangles.size() + 3 > 65535)
                {
                    triangles.clear();
                    return triangles;
                }
                triangles.insert(triangles.end(), { clipped[0], clipped[j], clipped[j + 1] });
            }
        }
    }
    return triangles;
}

static float cross(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

static bool rayPolygonIntersection(const NVGvertex* polygon, int count,
    float cx, float cy, float dx, float dy, NVGvertex* point)
{
    float best = std::numeric_limits<float>::infinity();
    for (int i = 0; i < count; ++i)
    {
        const NVGvertex& a      = polygon[i];
        const NVGvertex& b      = polygon[(i + 1) % count];
        const float ax          = a.x - cx;
        const float ay          = a.y - cy;
        const float ex          = b.x - a.x;
        const float ey          = b.y - a.y;
        const float denominator = cross(dx, dy, ex, ey);
        if (std::abs(denominator) < 0.000001f)
            continue;

        const float distance = cross(ax, ay, ex, ey) / denominator;
        const float edge     = cross(ax, ay, dx, dy) / denominator;
        if (distance >= 0.0f && edge >= -0.00001f && edge <= 1.00001f && distance < best)
            best = distance;
    }
    if (!std::isfinite(best))
        return false;

    point->x = cx + dx * best;
    point->y = cy + dy * best;
    point->u = 0.5f;
    point->v = 1.0f;
    return true;
}

/* NanoVG GL uses stencil to subtract a hole from a fill. The focus shadow is a
 * convex rectangle with a convex rounded-rectangle hole; tessellating that ring
 * on CPU avoids filling the selected item itself on AGC, where stencil testing
 * is not reliable yet. */
static bool drawConvexRing(State* state, const NVGpaint* paint, const NVGscissor* scissor,
    const NVGpath& outer, const NVGpath& hole)
{
    if (!outer.fill || !hole.fill || outer.nfill < 3 || hole.nfill < 3 || outer.nfill + hole.nfill > 8192 || !outer.convex || !hole.convex)
        return false;

    float cx = 0.0f;
    float cy = 0.0f;
    std::vector<float> angles;
    angles.reserve(static_cast<size_t>(outer.nfill + hole.nfill));
    const auto addAngles = [&](const NVGpath& path)
    {
        for (int i = 0; i < path.nfill; ++i)
        {
            const float x = path.fill[i].x;
            const float y = path.fill[i].y;
            angles.push_back(std::atan2(y - cy, x - cx));
        }
    };
    for (int i = 0; i < hole.nfill; ++i)
    {
        cx += hole.fill[i].x;
        cy += hole.fill[i].y;
    }
    cx /= static_cast<float>(hole.nfill);
    cy /= static_cast<float>(hole.nfill);
    addAngles(outer);
    addAngles(hole);
    for (float& angle : angles)
        if (angle < 0.0f)
            angle += 6.2831853071795864769f;
    std::sort(angles.begin(), angles.end());
    angles.erase(std::unique(angles.begin(), angles.end(), [](float a, float b)
                     { return std::abs(a - b) < 0.00001f; }),
        angles.end());
    if (angles.size() > 1 && angles.front() + 6.2831853071795864769f - angles.back() < 0.00001f)
        angles.pop_back();
    if (angles.size() < 3 || angles.size() * 6 > 65535)
        return false;

    std::vector<NVGvertex> triangles;
    triangles.reserve(angles.size() * 6);
    for (size_t i = 0; i < angles.size(); ++i)
    {
        const float a0 = angles[i];
        const float a1 = i + 1 < angles.size() ? angles[i + 1] : angles[0] + 6.2831853071795864769f;
        NVGvertex inner0, inner1, outer0, outer1;
        if (!rayPolygonIntersection(hole.fill, hole.nfill, cx, cy, std::cos(a0), std::sin(a0), &inner0) || !rayPolygonIntersection(hole.fill, hole.nfill, cx, cy, std::cos(a1), std::sin(a1), &inner1) || !rayPolygonIntersection(outer.fill, outer.nfill, cx, cy, std::cos(a0), std::sin(a0), &outer0) || !rayPolygonIntersection(outer.fill, outer.nfill, cx, cy, std::cos(a1), std::sin(a1), &outer1) || a1 - a0 > 3.1415926535897932385f)
            return false;

        triangles.insert(triangles.end(), { inner0, outer0, outer1, inner0, outer1, inner1 });
    }

    return draw(state, paint, scissor, triangles.data(), static_cast<int>(triangles.size()), DRAW_LIST, 0.0f);
}

static int create(void* uptr)
{
    auto* state  = static_cast<State*>(uptr);
    state->white = createTexture(state, NVG_TEXTURE_RGBA, 1, 1, NVG_IMAGE_PREMULTIPLIED, reinterpret_cast<const unsigned char*>("\xff\xff\xff\xff"));
    return state->white != nullptr;
}
static int createTextureCallback(void* uptr, int type, int w, int h, int flags, const unsigned char* data)
{
    auto* t = createTexture(static_cast<State*>(uptr), type, w, h, flags, data);
    return t ? t->id : 0;
}
static int deleteTexture(void* uptr, int id)
{
    auto* state = static_cast<State*>(uptr);
    for (auto it = state->textures.begin(); it != state->textures.end(); ++it)
        if ((*it)->id == id)
        {
            state->retired_textures.push_back(*it);
            state->textures.erase(it);
            return 1;
        }
    return 0;
}
static int updateTexture(void* uptr, int id, int x, int y, int w, int h, const unsigned char* data)
{
    auto* state = static_cast<State*>(uptr);
    Texture* t  = nullptr;
    for (Texture* candidate : state->textures)
    {
        if (candidate->id == id)
        {
            t = candidate;
            break;
        }
    }
    if (!t || !data || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > t->width || y + h > t->height)
        return 0;

    /* NanoVG sends the full atlas as `data`, together with the dirty rectangle.
     * The font atlas updates are usually small non-zero-origin rectangles; using
     * the rectangle width as the source stride shifts every row after the first.
     */
    const size_t bytes_per_pixel = textureSourceBpp(t);
    const size_t source_pitch    = static_cast<size_t>(t->width) * bytes_per_pixel;
    for (int row = 0; row < h; ++row)
    {
        uint8_t* dst       = t->pixels + static_cast<size_t>(y + row) * t->pitch;
        const uint8_t* src = data + static_cast<size_t>(y + row) * source_pitch;
        if (t->type != NVG_TEXTURE_ALPHA)
        {
            for (int col = 0; col < w; ++col)
            {
                const size_t offset = static_cast<size_t>(x + col) * 4u;
                const uint8_t alpha = src[offset + 3];
                dst[offset + 0]     = (t->flags & NVG_IMAGE_PREMULTIPLIED) ? src[offset + 0] : premultiply(src[offset + 0], alpha);
                dst[offset + 1]     = (t->flags & NVG_IMAGE_PREMULTIPLIED) ? src[offset + 1] : premultiply(src[offset + 1], alpha);
                dst[offset + 2]     = (t->flags & NVG_IMAGE_PREMULTIPLIED) ? src[offset + 2] : premultiply(src[offset + 2], alpha);
                dst[offset + 3]     = alpha;
            }
        }
        else
        {
            for (int col = 0; col < w; ++col)
            {
                const uint8_t coverage = src[x + col];
                dst[x + col]           = coverage;
            }
        }
    }
    evo_agc_runtime_cache_flush(t->pixels, static_cast<size_t>(t->pitch) * t->height);
    return 1;
}
static int textureSize(void* uptr, int id, int* w, int* h)
{
    auto* t = findTexture(static_cast<State*>(uptr), id);
    if (!t)
        return 0;
    *w = t->width;
    *h = t->height;
    return 1;
}
static void viewport(void* uptr, float w, float h, float)
{
    auto* state   = static_cast<State*>(uptr);
    state->width  = static_cast<int>(w);
    state->height = static_cast<int>(h);
}
static void fill(void* uptr, NVGpaint* p, NVGcompositeOperationState,
    NVGscissor* s, float fringe, const float*, const NVGpath* paths, int n)
{
    auto* state    = static_cast<State*>(uptr);
    int outerIndex = -1;
    int holeIndex  = -1;
    for (int i = 0; i < n; ++i)
    {
        if (paths[i].winding == NVG_HOLE)
            holeIndex = i;
        else
            outerIndex = i;
    }

    if (holeIndex >= 0)
    {
        /* The focus shadow is the one convex outer + convex hole fill used by
         * the UI. Triangulate its annulus instead of letting two triangle fans
         * paint over the hole; AGC's NanoVG path has no working stencil fill. */
        if (n != 2 || outerIndex < 0 || !drawConvexRing(state, p, s, paths[outerIndex], paths[holeIndex]))
            return;
        if (fringe > 0.0f)
            for (int i = 0; i < n; ++i)
                if (paths[i].nstroke > 0)
                    draw(state, p, s, paths[i].stroke, paths[i].nstroke, DRAW_STRIP, 1.0f);
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        draw(state, p, s, paths[i].fill, paths[i].nfill, DRAW_FAN, 0.0f);
        /* Convex paths can use NanoVG's fringe directly. Other concave paths
         * still need stencil-then-cover, which this backend does not implement. */
        if (paths[i].convex && fringe > 0.0f && paths[i].nstroke > 0)
            draw(state, p, s, paths[i].stroke, paths[i].nstroke, DRAW_STRIP, 1.0f);
    }
}
static void stroke(void* uptr, NVGpaint* p, NVGcompositeOperationState,
    NVGscissor* s, float fringe, float strokeWidth,
    const NVGpath* paths, int n)
{
    auto* state            = static_cast<State*>(uptr);
    const float strokeMult = fringe > 0.0f ? (strokeWidth * 0.5f + fringe * 0.5f) / fringe : 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const auto triangles = splitStrokeCoverage(paths[i].stroke, paths[i].nstroke);
        if (!triangles.empty())
            draw(state, p, s, triangles.data(), static_cast<int>(triangles.size()), DRAW_LIST, 0.0f);
        else
            draw(state, p, s, paths[i].stroke, paths[i].nstroke, DRAW_STRIP, strokeMult);
    }
}
static void triangles(void* uptr, NVGpaint* p, NVGcompositeOperationState, NVGscissor* s, const NVGvertex* v, int n, float)
{
    auto* state = static_cast<State*>(uptr);
    draw(state, p, s, v, n, DRAW_LIST, 0.0f);
}
static void flush(void* uptr) { reapRetired(static_cast<State*>(uptr)); }
static void cancel(void*) { }
static void destroy(void* uptr)
{
    auto* state = static_cast<State*>(uptr);
    /* nvgDeleteAgc runs before the platform releases the AGC runtime and its
     * direct-memory arena. Drain outstanding DCBs before freeing texture storage. */
    evo_agc_runtime_wait_idle(500);
    for (Texture* texture : state->textures)
        releaseTexture(texture);
    for (Texture* texture : state->retired_textures)
        releaseTexture(texture);
    delete state;
}
}

NVGcontext* nvgCreateAgc(int width, int height)
{
    auto* state = new State { width, height, 1, nullptr, {}, {} };
    NVGparams params {};
    params.userPtr = state;
    /* Generate fringe geometry here so convex fills and strokes can receive
     * per-vertex coverage from the AGC callback. */
    params.edgeAntiAlias        = 1;
    params.renderCreate         = create;
    params.renderCreateTexture  = createTextureCallback;
    params.renderDeleteTexture  = deleteTexture;
    params.renderUpdateTexture  = updateTexture;
    params.renderGetTextureSize = textureSize;
    params.renderViewport       = viewport;
    params.renderCancel         = cancel;
    params.renderFlush          = flush;
    params.renderFill           = fill;
    params.renderStroke         = stroke;
    params.renderTriangles      = triangles;
    params.renderDelete         = destroy;
    NVGcontext* context         = nvgCreateInternal(&params);
    if (!context)
    {
        destroy(state);
        return nullptr;
    }
    return context;
}

void nvgDeleteAgc(NVGcontext* context)
{
    if (context)
        nvgDeleteInternal(context);
}
