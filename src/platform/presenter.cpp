#include "platform/presenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "enhanced/scene.h"
#include "graphics/composite.h"
#include "platform/scene_shaders.inc"
#include "ui/app_icon.h"

namespace vette {
namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 960;

SdlPtr<SDL_Texture> create_texture(SDL_Renderer* renderer, SDL_TextureAccess access, int w, int h,
                                   SDL_ScaleMode scale_mode) {
    SdlPtr<SDL_Texture> texture{SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, access, w, h)};
    if (!texture)
        throw_sdl_error("SDL_CreateTexture");
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture.get(), scale_mode);
    return texture;
}

// Windows takes the icon from the exe's resources, in every size it has; elsewhere the window gets it here.
void set_window_icon(SDL_Window* window) {
#ifndef SDL_PLATFORM_WINDOWS
    constexpr int kSize = 128;
    std::vector<std::uint32_t> pixels = ui::app_icon(kSize);
    SdlPtr<SDL_Surface> icon{SDL_CreateSurfaceFrom(kSize, kSize, SDL_PIXELFORMAT_ARGB8888, pixels.data(), kSize * 4)};
    if (!icon || !SDL_SetWindowIcon(window, icon.get()))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "No window icon: %s", SDL_GetError());
#else
    (void)window;
#endif
}

}  // namespace

// The Enhanced 3D view drawn with a depth buffer: SDL's GPU API, on the device of the "gpu" renderer, into
// a texture of that renderer (so it's shown like any other). Its pipeline tests SceneVertex::depth
// (larger is nearer, ties to the later triangle) against a buffer cleared to 0, and blends the scenes'
// colours premultiplied, so translucent faces work as before.
struct Presenter::Gpu {
    SDL_GPUDevice* device = nullptr;
    SDL_GPUShader* vertex_shader = nullptr;
    SDL_GPUShader* pixel_shader = nullptr;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;  // the pipelines' target format
    SDL_GPUGraphicsPipeline* scene = nullptr;  // depth tested and written, blended
    SDL_GPUGraphicsPipeline* fill = nullptr;   // the mirror's sky: written whatever the depth (0), opaque
    SDL_GPUTexture* depth = nullptr;
    int depth_w = 0, depth_h = 0;
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    SDL_GPUTransferBuffer* transfer = nullptr;
    Uint32 vertex_bytes = 0, index_bytes = 0, transfer_bytes = 0;  // their sizes
    SdlPtr<SDL_Texture> target;  // what it draws into, the renderer's
    int target_w = 0, target_h = 0;
    bool failed = false;  // set by a draw that couldn't be done: the depth pass is then dropped

    SDL_Texture* fail() {
        failed = true;
        return nullptr;
    }

    ~Gpu() {
        release_pipelines();
        if (vertex_shader)
            SDL_ReleaseGPUShader(device, vertex_shader);
        if (pixel_shader)
            SDL_ReleaseGPUShader(device, pixel_shader);
        if (depth)
            SDL_ReleaseGPUTexture(device, depth);
        if (vertices)
            SDL_ReleaseGPUBuffer(device, vertices);
        if (indices)
            SDL_ReleaseGPUBuffer(device, indices);
        if (transfer)
            SDL_ReleaseGPUTransferBuffer(device, transfer);
    }

    void release_pipelines() {
        if (scene)
            SDL_ReleaseGPUGraphicsPipeline(device, scene);
        if (fill)
            SDL_ReleaseGPUGraphicsPipeline(device, fill);
        scene = fill = nullptr;
        color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    }

    // The shaders in a format the device takes, and a depth format; false if there's none.
    bool init() {
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
        SDL_GPUShaderCreateInfo vs{};
        SDL_GPUShaderCreateInfo ps{};
        if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
            vs.code = kSceneVertexDxil;
            vs.code_size = sizeof kSceneVertexDxil;
            ps.code = kScenePixelDxil;
            ps.code_size = sizeof kScenePixelDxil;
            vs.format = ps.format = SDL_GPU_SHADERFORMAT_DXIL;
            vs.entrypoint = "vs_main";
            ps.entrypoint = "ps_main";
        } else if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
            vs.code = kSceneVertexSpirv;
            vs.code_size = sizeof kSceneVertexSpirv;
            ps.code = kScenePixelSpirv;
            ps.code_size = sizeof kScenePixelSpirv;
            vs.format = ps.format = SDL_GPU_SHADERFORMAT_SPIRV;
            vs.entrypoint = ps.entrypoint = "main";
        } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
            vs.code = ps.code = reinterpret_cast<const Uint8*>(kSceneMetal);
            vs.code_size = ps.code_size = std::strlen(kSceneMetal);
            vs.format = ps.format = SDL_GPU_SHADERFORMAT_MSL;
            vs.entrypoint = "vs_main";
            ps.entrypoint = "fs_main";
        } else {
            SDL_SetError("no shader format for this GPU device");
            return false;
        }
        vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vs.num_uniform_buffers = 1;
        ps.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        vertex_shader = SDL_CreateGPUShader(device, &vs);
        pixel_shader = SDL_CreateGPUShader(device, &ps);
        if (!vertex_shader || !pixel_shader)
            return false;
        // Float depth: the far city's depths are around 4e-6, too fine for a 24-bit buffer.
        depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        if (!SDL_GPUTextureSupportsFormat(device, depth_format, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            SDL_SetError("no 32-bit float depth buffer");
            return false;
        }
        return true;
    }

    bool make_pipelines(SDL_GPUTextureFormat format) {
        release_pipelines();
        const SDL_GPUVertexBufferDescription buffer{0, sizeof(enhanced::SceneVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const SDL_GPUVertexAttribute attributes[] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(enhanced::SceneVertex, x)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(enhanced::SceneVertex, r)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(enhanced::SceneVertex, depth)},
        };
        SDL_GPUColorTargetDescription target_desc{};
        target_desc.format = format;
        SDL_GPUColorTargetBlendState& blend = target_desc.blend_state;
        blend.enable_blend = true;
        blend.src_color_blendfactor = blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_color_blendfactor = blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex_shader;
        info.fragment_shader = pixel_shader;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 3;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.target_info.color_target_descriptions = &target_desc;
        info.target_info.num_color_targets = 1;
        info.target_info.depth_stencil_format = depth_format;
        info.target_info.has_depth_stencil_target = true;
        scene = SDL_CreateGPUGraphicsPipeline(device, &info);
        blend.enable_blend = false;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
        fill = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!scene || !fill) {
            release_pipelines();
            return false;
        }
        color_format = format;
        return true;
    }

    // A buffer at least `bytes` big (doubling, so it settles quickly).
    template <class Buffer, class Create>
    bool reserve(Buffer*& buffer, Uint32& size, Uint32 bytes, Create create) {
        if (buffer && size >= bytes)
            return true;
        Uint32 n = std::max<Uint32>(size, 1u << 16);
        while (n < bytes)
            n *= 2;
        Buffer* fresh = create(n);
        if (!fresh)
            return false;
        if (buffer) {
            if constexpr (std::is_same_v<Buffer, SDL_GPUTransferBuffer>)
                SDL_ReleaseGPUTransferBuffer(device, buffer);
            else
                SDL_ReleaseGPUBuffer(device, buffer);
        }
        buffer = fresh;
        size = n;
        return true;
    }
};

Presenter::Presenter(const char* title) {
    SDL_Window* window = SDL_CreateWindow(title, kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
        throw_sdl_error("SDL_CreateWindow");
    window_.reset(window);
    set_window_icon(window);

    // SDL's GPU renderer, for the depth buffer; without one that can (or with SDL_RENDER_DRIVER set), the
    // platform's default renderer.
    SDL_Renderer* renderer = nullptr;
    if (!SDL_GetHint(SDL_HINT_RENDER_DRIVER)) {
        renderer = SDL_CreateRenderer(window, "gpu");
        if (renderer) {
            auto gpu = std::make_unique<Gpu>();
            gpu->device = static_cast<SDL_GPUDevice*>(
                SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
            if (gpu->device && gpu->init()) {
                gpu_ = std::move(gpu);
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "No depth buffer: %s", SDL_GetError());
                gpu.reset();
                SDL_DestroyRenderer(renderer);
                renderer = nullptr;
            }
        }
    }
    if (!renderer)
        renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
        throw_sdl_error("SDL_CreateRenderer");
    renderer_.reset(renderer);

    if (!SDL_SetRenderVSync(renderer, 1))
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "VSync unavailable: %s", SDL_GetError());
    if (gpu_) {
        SDL_Log("Renderer: %s (%s), with a depth buffer", SDL_GetRendererName(renderer), SDL_GetGPUDeviceDriver(gpu_->device));
    } else {
        SDL_Log("Renderer: %s", SDL_GetRendererName(renderer));
    }
}

// The members' order destroys the depth pass (and its texture) before the renderer.
Presenter::~Presenter() = default;

// Largest 4:3 rect that fits the output, centered: both EGA modes filled a 4:3 CRT.
SDL_FRect Presenter::fit() const {
    int out_w = 0;
    int out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_.get(), &out_w, &out_h);
    const int w = std::min(out_w, out_h * 4 / 3);
    const int h = std::min(out_h, out_w * 3 / 4);
    return {static_cast<float>(out_w - w) / 2, static_cast<float>(out_h - h) / 2, static_cast<float>(w),
            static_cast<float>(h)};
}

void Presenter::frame_scale(int frame_w, int frame_h, float& sx, float& sy) const {
    if (original_resolution_) {
        sx = sy = 1;
        return;
    }
    const SDL_FRect dst = fit();
    sx = dst.w / static_cast<float>(frame_w);
    sy = dst.h / static_cast<float>(frame_h);
}

// Smooth scaling is "sharp bilinear": nearest-neighbor upscale by whole factors close to the output
// size, then a linear filter for the remaining non-integer stretch, so only the edges between pixels
// blend. Sharp scaling uses nearest neighbour all the way: every pixel a solid block. With `transparency`, kTransparentPixel pixels are see-through
// (premultiplied alpha, so the linear filter doesn't darken the edges).
SDL_Texture* Presenter::upload(Layer& layer, const std::uint8_t* src_pixels, int w, int h,
                               const std::array<std::uint32_t, 16>& argb, bool transparency, const SDL_FRect& dst) {
    SDL_Renderer* renderer = renderer_.get();
    if (w != layer.w || h != layer.h) {  // the game switched video modes
        layer.frame = create_texture(renderer, SDL_TEXTUREACCESS_STREAMING, w, h, SDL_SCALEMODE_NEAREST);
        if (transparency)
            SDL_SetTextureBlendMode(layer.frame.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        layer.w = w;
        layer.h = h;
        layer.scale_x = layer.scale_y = 0;
    }

    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(layer.frame.get(), nullptr, &pixels, &pitch))
        throw_sdl_error("SDL_LockTexture");
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(pixels) + y * pitch);
        const std::uint8_t* src = src_pixels + y * w;
        for (int x = 0; x < w; ++x)
            row[x] = transparency && src[x] == kTransparentPixel ? 0 : argb[src[x] & 0x0F];
    }
    SDL_UnlockTexture(layer.frame.get());
    return enlarge(layer, transparency, dst);
}

// The layer's frame-sized texture, upscaled by whole factors for the final stretch (see upload()).
SDL_Texture* Presenter::enlarge(Layer& layer, bool transparency, const SDL_FRect& dst) {
    SDL_Renderer* renderer = renderer_.get();
    const int w = layer.w;
    const int h = layer.h;
    const int ix = std::max(1, static_cast<int>(dst.w) / w);
    const int iy = std::max(1, static_cast<int>(dst.h) / h);
    if (ix != layer.scale_x || iy != layer.scale_y) {
        layer.scaled = create_texture(renderer, SDL_TEXTUREACCESS_TARGET, w * ix, h * iy, SDL_SCALEMODE_LINEAR);
        if (transparency)
            SDL_SetTextureBlendMode(layer.scaled.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        layer.scale_x = ix;
        layer.scale_y = iy;
    }
    SDL_SetRenderTarget(renderer, layer.scaled.get());
    SDL_SetTextureBlendMode(layer.frame.get(), SDL_BLENDMODE_NONE);  // copied as is, alpha included
    SDL_RenderTexture(renderer, layer.frame.get(), nullptr, nullptr);
    if (transparency)
        SDL_SetTextureBlendMode(layer.frame.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetTextureScaleMode(layer.scaled.get(), smooth_ ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    return layer.scaled.get();
}

SDL_Texture* Presenter::upload(Layer& layer, const Framebuffer& fb, bool transparency, const SDL_FRect& dst) {
    std::array<std::uint32_t, 16> argb{};
    for (std::size_t i = 0; i < argb.size(); ++i) {
        const Rgb c = fb.palette[i];
        argb[i] = 0xFF000000u | std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b;
    }
    return upload(layer, fb.pixels.data(), fb.width, fb.height, argb, transparency, dst);
}

void Presenter::present(const Framebuffer& fb) {
    SDL_Renderer* renderer = renderer_.get();
    const SDL_FRect dst = fit();
    SDL_Texture* picture = upload(base_, fb, false, dst);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, picture, nullptr, &dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = fb.width;
    picture_h_ = fb.height;
}

// The Enhanced 3D view: the game's view without its world, then the scene's triangles (and the
// mirror's), from frame coordinates to output pixels, each clipped to its viewport. At the original
// resolution, all of it is drawn into a frame-sized texture first, which is then enlarged like a frame.
void Presenter::draw_scene(const Framebuffer& under, const enhanced::Scene& scene, const enhanced::Scene* inset,
                           const SDL_FRect& dst) {
    SDL_Renderer* renderer = renderer_.get();
    if (!original_resolution_) {
        draw_scene_layers(upload(base_, under, false, dst), under.width, under.height, scene, inset, dst);
        return;
    }
    const SDL_FRect frame{0, 0, static_cast<float>(under.width), static_cast<float>(under.height)};
    SDL_Texture* under_texture = upload(low_under_, under, false, frame);  // before the target changes
    if (under.width != low_scene_.w || under.height != low_scene_.h) {
        low_scene_.frame = create_texture(renderer, SDL_TEXTUREACCESS_TARGET, under.width, under.height,
                                          SDL_SCALEMODE_NEAREST);
        low_scene_.w = under.width;
        low_scene_.h = under.height;
        low_scene_.scale_x = low_scene_.scale_y = 0;
    }
    SDL_SetRenderTarget(renderer, low_scene_.frame.get());
    draw_scene_layers(under_texture, under.width, under.height, scene, inset, frame);
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_RenderTexture(renderer, enlarge(low_scene_, false, dst), nullptr, &dst);
}

void Presenter::draw_scene_layers(SDL_Texture* under, int frame_w, int frame_h, const enhanced::Scene& scene,
                                  const enhanced::Scene* inset, const SDL_FRect& dst) {
    SDL_Renderer* renderer = renderer_.get();
    SDL_RenderTexture(renderer, under, nullptr, &dst);
    if (depth_buffer()) {
        const int w = std::max(1, static_cast<int>(std::lround(dst.w)));
        const int h = std::max(1, static_cast<int>(std::lround(dst.h)));
        if (SDL_Texture* drawn = draw_depth_tested(scene, inset, frame_w, frame_h, w, h)) {
            SDL_RenderTexture(renderer, drawn, nullptr, &dst);
            ++depth_frames_;
            return;
        }
        if (gpu_->failed) {  // from now on the triangles in their order, as without a depth buffer
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Depth buffer dropped: %s", SDL_GetError());
            gpu_.reset();
        }
    }
    draw_triangles(scene, frame_w, frame_h, dst);
    if (inset) {
        // Nothing of the main view may show in the mirror: its viewport is filled first (with its sky,
        // the colour of its first triangles), then its scene goes on top.
        const float sx = dst.w / static_cast<float>(frame_w);
        const float sy = dst.h / static_cast<float>(frame_h);
        const float x0 = std::round(dst.x + static_cast<float>(inset->view_x0) * sx);
        const float y0 = std::round(dst.y + static_cast<float>(inset->view_y0) * sy);
        const SDL_FRect rect{x0, y0, std::round(dst.x + static_cast<float>(inset->view_x1) * sx) - x0,
                             std::round(dst.y + static_cast<float>(inset->view_y1) * sy) - y0};
        const enhanced::SceneVertex sky = inset->vertices.empty() ? enhanced::SceneVertex{0, 0, 0x55 / 255.0f, 1, 1, 1}
                                                                  : inset->vertices.front();
        SDL_SetRenderDrawColorFloat(renderer, sky.r, sky.g, sky.b, 1);
        SDL_RenderFillRect(renderer, &rect);
        draw_triangles(*inset, frame_w, frame_h, dst);
    }
}

void Presenter::draw_triangles(const enhanced::Scene& scene, int frame_w, int frame_h, const SDL_FRect& dst) {
    SDL_Renderer* renderer = renderer_.get();
    if (scene.indices.empty())
        return;
    const float sx = dst.w / static_cast<float>(frame_w);
    const float sy = dst.h / static_cast<float>(frame_h);
    scene_xy_.resize(scene.vertices.size() * 2);
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        scene_xy_[2 * i] = dst.x + scene.vertices[i].x * sx;
        scene_xy_[2 * i + 1] = dst.y + scene.vertices[i].y * sy;
    }
    const auto edge = [](float v) { return static_cast<int>(std::lround(v)); };
    const int x0 = edge(dst.x + static_cast<float>(scene.view_x0) * sx);
    const int y0 = edge(dst.y + static_cast<float>(scene.view_y0) * sy);
    const SDL_Rect clip{x0, y0, edge(dst.x + static_cast<float>(scene.view_x1) * sx) - x0,
                        edge(dst.y + static_cast<float>(scene.view_y1) * sy) - y0};
    SDL_SetRenderClipRect(renderer, &clip);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);  // screen-door faces are translucent
    SDL_RenderGeometryRaw(renderer, nullptr, scene_xy_.data(), static_cast<int>(2 * sizeof(float)),
                          reinterpret_cast<const SDL_FColor*>(&scene.vertices[0].r),
                          static_cast<int>(sizeof(enhanced::SceneVertex)), nullptr, 0,
                          static_cast<int>(scene.vertices.size()), scene.indices.data(),
                          static_cast<int>(scene.indices.size()), static_cast<int>(sizeof(std::int32_t)));
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderClipRect(renderer, nullptr);
}

// The scene (and the mirror's) into the depth pass's target, `w` x `h` pixels for the whole frame: the
// scene in its viewport; then the mirror's viewport filled with its sky (as draw_scene_layers() does),
// its depth reset, and its scene. Null if the GPU fails: the caller then draws the triangles in order.
SDL_Texture* Presenter::draw_depth_tested(const enhanced::Scene& scene, const enhanced::Scene* inset, int frame_w,
                                          int frame_h, int w, int h) {
    Gpu& g = *gpu_;
    SDL_GPUDevice* device = g.device;
    if (!g.target || g.target_w != w || g.target_h != h) {
        g.target = create_texture(renderer_.get(), SDL_TEXTUREACCESS_TARGET, w, h, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(g.target.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        g.target_w = w;
        g.target_h = h;
    }
    auto* color = static_cast<SDL_GPUTexture*>(
        SDL_GetPointerProperty(SDL_GetTextureProperties(g.target.get()), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    const SDL_GPUTextureFormat format = SDL_GetGPUTextureFormatFromPixelFormat(g.target->format);
    if (!color || (format != g.color_format && !g.make_pipelines(format)))
        return g.fail();
    if (!g.depth || g.depth_w != w || g.depth_h != h) {
        if (g.depth)
            SDL_ReleaseGPUTexture(device, g.depth);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = g.depth_format;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width = static_cast<Uint32>(w);
        info.height = static_cast<Uint32>(h);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        g.depth = SDL_CreateGPUTexture(device, &info);
        g.depth_w = g.depth ? w : 0;
        g.depth_h = g.depth ? h : 0;
        if (!g.depth)
            return g.fail();
    }

    // The vertices and indices: the scene's, the mirror's, and the mirror's sky (a quad over the frame).
    const size_t sv = scene.vertices.size(), si = scene.indices.size();
    const size_t mv = inset ? inset->vertices.size() : 0, mi = inset ? inset->indices.size() : 0;
    const float fw = static_cast<float>(frame_w), fh = static_cast<float>(frame_h);
    const enhanced::SceneVertex sky = inset && !inset->vertices.empty() ? inset->vertices.front()
                                                                        : enhanced::SceneVertex{0, 0, 0, 0, 0x55 / 255.0f, 1, 0};
    const enhanced::SceneVertex quad[4] = {{0, 0, sky.r, sky.g, sky.b, 1, 0},
                                           {fw, 0, sky.r, sky.g, sky.b, 1, 0},
                                           {fw, fh, sky.r, sky.g, sky.b, 1, 0},
                                           {0, fh, sky.r, sky.g, sky.b, 1, 0}};
    static constexpr std::int32_t kQuad[6] = {0, 1, 2, 0, 2, 3};
    const size_t nv = sv + mv + (inset ? 4 : 0), ni = si + mi + (inset ? 6 : 0);
    if (ni == 0)
        return nullptr;  // nothing to draw
    const auto vbytes = static_cast<Uint32>(nv * sizeof(enhanced::SceneVertex));
    const auto ibytes = static_cast<Uint32>(ni * sizeof(std::int32_t));
    const auto make_buffer = [device](SDL_GPUBufferUsageFlags usage) {
        return [device, usage](Uint32 size) {
            const SDL_GPUBufferCreateInfo info{usage, size, 0};
            return SDL_CreateGPUBuffer(device, &info);
        };
    };
    if (!g.reserve(g.vertices, g.vertex_bytes, vbytes, make_buffer(SDL_GPU_BUFFERUSAGE_VERTEX)) ||
        !g.reserve(g.indices, g.index_bytes, ibytes, make_buffer(SDL_GPU_BUFFERUSAGE_INDEX)) ||
        !g.reserve(g.transfer, g.transfer_bytes, vbytes + ibytes, [device](Uint32 size) {
            const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
            return SDL_CreateGPUTransferBuffer(device, &info);
        }))
        return g.fail();
    auto* bytes = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device, g.transfer, true));
    if (!bytes)
        return g.fail();
    auto* v = reinterpret_cast<enhanced::SceneVertex*>(bytes);
    std::memcpy(v, scene.vertices.data(), sv * sizeof *v);
    if (inset) {
        std::memcpy(v + sv, inset->vertices.data(), mv * sizeof *v);
        std::memcpy(v + sv + mv, quad, sizeof quad);
    }
    auto* ix = reinterpret_cast<std::int32_t*>(bytes + vbytes);
    std::memcpy(ix, scene.indices.data(), si * sizeof *ix);
    if (inset) {
        std::memcpy(ix + si, inset->indices.data(), mi * sizeof *ix);
        std::memcpy(ix + si + mi, kQuad, sizeof kQuad);
    }
    SDL_UnmapGPUTransferBuffer(device, g.transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    if (!cmd)
        return g.fail();
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation from_v{g.transfer, 0}, from_i{g.transfer, vbytes};
    const SDL_GPUBufferRegion to_v{g.vertices, 0, vbytes}, to_i{g.indices, 0, ibytes};
    SDL_UploadToGPUBuffer(copy, &from_v, &to_v, true);
    SDL_UploadToGPUBuffer(copy, &from_i, &to_i, true);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo target{};
    target.texture = color;
    target.clear_color = {0, 0, 0, 0};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = g.depth;
    depth.clear_depth = 0;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, &depth);
    const SDL_GPUBufferBinding vb{g.vertices, 0}, ib{g.indices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const float transform[4] = {2 / fw, -2 / fh, -1, 1};  // frame -> clip space, y up
    SDL_PushGPUVertexUniformData(cmd, 0, transform, sizeof transform);
    // A viewport in frame coordinates, in target pixels (rounded as draw_triangles() clips).
    const float sx = static_cast<float>(w) / fw, sy = static_cast<float>(h) / fh;
    const auto rect = [&](int x0, int y0, int x1, int y1) {
        const int l = std::clamp(static_cast<int>(std::lround(static_cast<float>(x0) * sx)), 0, w);
        const int t = std::clamp(static_cast<int>(std::lround(static_cast<float>(y0) * sy)), 0, h);
        const int r = std::clamp(static_cast<int>(std::lround(static_cast<float>(x1) * sx)), 0, w);
        const int b = std::clamp(static_cast<int>(std::lround(static_cast<float>(y1) * sy)), 0, h);
        return SDL_Rect{l, t, std::max(0, r - l), std::max(0, b - t)};
    };
    SDL_BindGPUGraphicsPipeline(pass, g.scene);
    if (si > 0) {
        const SDL_Rect clip = rect(scene.view_x0, scene.view_y0, scene.view_x1, scene.view_y1);
        SDL_SetGPUScissor(pass, &clip);
        SDL_DrawGPUIndexedPrimitives(pass, static_cast<Uint32>(si), 1, 0, 0, 0);
    }
    if (inset) {
        const SDL_Rect clip = rect(inset->view_x0, inset->view_y0, inset->view_x1, inset->view_y1);
        SDL_SetGPUScissor(pass, &clip);
        SDL_BindGPUGraphicsPipeline(pass, g.fill);
        SDL_DrawGPUIndexedPrimitives(pass, 6, 1, static_cast<Uint32>(si + mi), static_cast<Sint32>(sv + mv), 0);
        if (mi > 0) {
            SDL_BindGPUGraphicsPipeline(pass, g.scene);
            SDL_DrawGPUIndexedPrimitives(pass, static_cast<Uint32>(mi), 1, static_cast<Uint32>(si), static_cast<Sint32>(sv), 0);
        }
    }
    SDL_EndGPURenderPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(cmd))
        return g.fail();
    return g.target.get();
}

void Presenter::present(const Framebuffer& under, const enhanced::Scene& scene, const Framebuffer& over,
                        const enhanced::Scene* inset) {
    SDL_Renderer* renderer = renderer_.get();
    const SDL_FRect dst = fit();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    draw_scene(under, scene, inset, dst);
    SDL_RenderTexture(renderer, upload(over_, over, true, dst), nullptr, &dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = under.width;
    picture_h_ = under.height;
}

// The art's images are made once (they live as long as the Substitution); a texture each.
SDL_Texture* Presenter::art_texture(const graphics::Image& image) {
    SdlPtr<SDL_Texture>& t = art_[&image];
    if (!t) {
        t.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, image.width,
                                  image.height));
        if (!t)
            throw_sdl_error("SDL_CreateTexture");
        SDL_UpdateTexture(t.get(), nullptr, image.pixels.data(), image.width * 4);
        SDL_SetTextureBlendMode(t.get(), SDL_BLENDMODE_BLEND);
    }
    SDL_SetTextureScaleMode(t.get(), smooth_ ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    return t.get();
}

void Presenter::draw_composite(const graphics::Composite& c, const SDL_FRect& dst) {
    SDL_Renderer* renderer = renderer_.get();
    std::array<std::uint32_t, 16> argb{};
    for (std::size_t i = 0; i < argb.size(); ++i)
        argb[i] = 0xFF000000u | c.palette[i];
    const float sx = dst.w / static_cast<float>(c.frame_w);
    const float sy = dst.h / static_cast<float>(c.frame_h);
    const auto out = [&](const graphics::FRect& r) {
        return SDL_FRect{dst.x + r.x * sx, dst.y + r.y * sy, r.w * sx, r.h * sy};
    };

    if (c.base.empty()) {  // a full-screen replacement: its letterbox bars
        SDL_SetRenderDrawColor(renderer, static_cast<std::uint8_t>(c.backdrop >> 16),
                               static_cast<std::uint8_t>(c.backdrop >> 8), static_cast<std::uint8_t>(c.backdrop),
                               SDL_ALPHA_OPAQUE);
        SDL_RenderFillRect(renderer, &dst);
    } else {
        SDL_RenderTexture(renderer, upload(art_base_, c.base.data(), c.frame_w, c.frame_h, argb, true, dst), nullptr,
                          &dst);
    }
    for (const graphics::Composite::Layer& l : c.layers) {
        if (!l.image || l.image->empty())
            continue;
        const SDL_FRect src{static_cast<float>(l.src.x), static_cast<float>(l.src.y), static_cast<float>(l.src.w),
                            static_cast<float>(l.src.h)};
        const SDL_FRect to = out(l.dst);
        SDL_RenderTexture(renderer, art_texture(*l.image), &src, &to);
    }
    if (!c.over.empty())
        SDL_RenderTexture(renderer, upload(art_over_, c.over.data(), c.frame_w, c.frame_h, argb, true, dst), nullptr,
                          &dst);
    if (!c.pieces.empty() && !c.moved.empty()) {
        upload(art_moved_, c.moved.data(), c.frame_w, c.frame_h, argb, true, dst);
        for (const graphics::Composite::Piece& piece : c.pieces) {
            const SDL_FRect src{static_cast<float>(piece.src.x), static_cast<float>(piece.src.y),
                                static_cast<float>(piece.src.w), static_cast<float>(piece.src.h)};
            const SDL_FRect to = out(piece.dst);
            SDL_RenderTexture(renderer, art_moved_.frame.get(), &src, &to);  // sharp: the frame-sized texture
        }
    }
}

void Presenter::present(const graphics::Composite& composite) {
    SDL_Renderer* renderer = renderer_.get();
    const SDL_FRect dst = fit();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    draw_composite(composite, dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = composite.frame_w;
    picture_h_ = composite.frame_h;
}

void Presenter::present(const Framebuffer& under, const enhanced::Scene& scene, const graphics::Composite& composite,
                        const enhanced::Scene* inset) {
    SDL_Renderer* renderer = renderer_.get();
    const SDL_FRect dst = fit();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    draw_scene(under, scene, inset, dst);
    draw_composite(composite, dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = under.width;
    picture_h_ = under.height;
}

void Presenter::present(const ui::Canvas& canvas) {
    SDL_Renderer* renderer = renderer_.get();
    if (canvas.width != canvas_w_ || canvas.height != canvas_h_) {
        canvas_ = create_texture(renderer, SDL_TEXTUREACCESS_STREAMING, canvas.width, canvas.height,
                                 SDL_SCALEMODE_NEAREST);
        canvas_w_ = canvas.width;
        canvas_h_ = canvas.height;
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(canvas_.get(), nullptr, &pixels, &pitch))
        throw_sdl_error("SDL_LockTexture");
    for (int y = 0; y < canvas.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(pixels) + y * pitch);
        const std::uint32_t* src = canvas.pixels.data() + y * canvas.width;
        for (int x = 0; x < canvas.width; ++x)
            row[x] = 0xFF000000u | src[x];
    }
    SDL_UnlockTexture(canvas_.get());

    int out_w = 0;
    int out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
    const int w = canvas.width * canvas.scale;
    const int h = canvas.height * canvas.scale;
    const SDL_FRect dst{static_cast<float>((out_w - w) / 2), static_cast<float>((out_h - h) / 2), static_cast<float>(w),
                        static_cast<float>(h)};
    SDL_SetRenderDrawColor(renderer, static_cast<std::uint8_t>(canvas.background >> 16),
                           static_cast<std::uint8_t>(canvas.background >> 8), static_cast<std::uint8_t>(canvas.background),
                           SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, canvas_.get(), nullptr, &dst);
    finish_frame();
    picture_ = dst;
    picture_w_ = canvas.width;
    picture_h_ = canvas.height;
}

void Presenter::show_overlay(const ui::Canvas& canvas) {
    SDL_Renderer* renderer = renderer_.get();
    if (!overlay_ || canvas.width != overlay_w_ || canvas.height != overlay_h_) {
        overlay_ = create_texture(renderer, SDL_TEXTUREACCESS_STREAMING, canvas.width, canvas.height,
                                  SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(overlay_.get(), SDL_BLENDMODE_BLEND);
        overlay_w_ = canvas.width;
        overlay_h_ = canvas.height;
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(overlay_.get(), nullptr, &pixels, &pitch))
        throw_sdl_error("SDL_LockTexture");
    for (int y = 0; y < canvas.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(pixels) + y * pitch);
        const std::uint32_t* src = canvas.pixels.data() + y * canvas.width;
        for (int x = 0; x < canvas.width; ++x)
            row[x] = src[x] == canvas.background ? 0u : 0xFF000000u | src[x];
    }
    SDL_UnlockTexture(overlay_.get());
    overlay_scale_ = canvas.scale;
    overlay_on_ = true;
}

void Presenter::finish_frame() {
    SDL_Renderer* renderer = renderer_.get();
    if (overlay_on_ && overlay_) {
        int out_w = 0;
        int out_h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
        SDL_RenderFillRect(renderer, nullptr);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        const int w = overlay_w_ * overlay_scale_;
        const int h = overlay_h_ * overlay_scale_;
        const SDL_FRect dst{static_cast<float>((out_w - w) / 2), static_cast<float>((out_h - h) / 2),
                            static_cast<float>(w), static_cast<float>(h)};
        SDL_RenderTexture(renderer, overlay_.get(), nullptr, &dst);
    }
    if (!screenshot_.empty()) {
        SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
        if (!shot || !SDL_SaveBMP(shot, screenshot_.c_str()))
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "Screenshot %s: %s", screenshot_.c_str(), SDL_GetError());
        SDL_DestroySurface(shot);
        screenshot_.clear();
    }
    SDL_RenderPresent(renderer);
}

void Presenter::output_size(int& w, int& h) const { SDL_GetCurrentRenderOutputSize(renderer_.get(), &w, &h); }

void Presenter::toggle_fullscreen() { set_fullscreen(!fullscreen()); }

void Presenter::set_fullscreen(bool on) { SDL_SetWindowFullscreen(window_.get(), on); }

bool Presenter::fullscreen() const { return (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN) != 0; }

bool Presenter::window_to_frame(float wx, float wy, int& fx, int& fy) const {
    float rx = 0;
    float ry = 0;
    if (picture_w_ == 0 || picture_.w <= 0 || picture_.h <= 0 ||
        !SDL_RenderCoordinatesFromWindow(renderer_.get(), wx, wy, &rx, &ry))
        return false;
    const float u = (rx - picture_.x) / picture_.w;
    const float v = (ry - picture_.y) / picture_.h;
    if (u < 0 || u >= 1 || v < 0 || v >= 1)
        return false;
    fx = static_cast<int>(u * static_cast<float>(picture_w_));
    fy = static_cast<int>(v * static_cast<float>(picture_h_));
    return true;
}

bool Presenter::visible() const {
    return (SDL_GetWindowFlags(window_.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) == 0;
}

// SDL's cursor visibility only applies over SDL's own windows, and this app has just the one.
void Presenter::show_system_cursor(bool show) {
    if (show != system_cursor_shown_ && (show ? SDL_ShowCursor() : SDL_HideCursor()))
        system_cursor_shown_ = show;
}

}  // namespace vette
