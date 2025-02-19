//
// Created by Mike on 2022/1/7.
//

#include <limits>
#include <luisa-compute.h>
#include <base/film.h>
#include <base/pipeline.h>
#include <util/colorspace.h>

namespace luisa::render {

using namespace luisa::compute;

class ColorFilm final : public Film {

private:
    float _scale[3]{};
    float _clamp{};
    bool _warn_nan{};

public:
    ColorFilm(Scene *scene, const SceneNodeDesc *desc) noexcept
        : Film{scene, desc},
          _warn_nan{desc->property_bool_or_default("warn_nan", false)} {
        auto exposure = desc->property_float3_or_default(
            "exposure", lazy_construct([desc] {
                return make_float3(desc->property_float_or_default(
                    "exposure", 0.0f));
            }));
        _scale[0] = std::pow(2.0f, exposure.x);
        _scale[1] = std::pow(2.0f, exposure.y);
        _scale[2] = std::pow(2.0f, exposure.z);
        _clamp = std::max(1.f, desc->property_float_or_default("clamp", 256.f));
    }
    [[nodiscard]] auto scale() const noexcept { return make_float3(_scale[0], _scale[1], _scale[2]); }
    [[nodiscard]] float clamp() const noexcept override { return _clamp; }
    [[nodiscard]] auto warn_nan() const noexcept { return _warn_nan; }
    [[nodiscard]] luisa::unique_ptr<Instance> build(
        Pipeline &pipeline, CommandBuffer &command_buffer) const noexcept override;
    [[nodiscard]] luisa::string_view impl_type() const noexcept override { return LUISA_RENDER_PLUGIN_NAME; }
};

class ColorFilmInstance final : public Film::Instance {

private:
    mutable Buffer<float> _image;
    mutable Buffer<float4> _converted;
    std::shared_future<Shader1D<Buffer<float>>> _clear_image;
    std::shared_future<Shader1D<Buffer<float>, Buffer<float4>>> _convert_image;

private:
    void _check_prepared() const noexcept {
        LUISA_ASSERT(_image && _converted, "Film is not prepared.");
    }

public:
    ColorFilmInstance(Device &device, Pipeline &pipeline, const ColorFilm *film) noexcept;
    void prepare(CommandBuffer &command_buffer) noexcept override;
    void download(CommandBuffer &command_buffer, float4 *framebuffer) const noexcept override;
    [[nodiscard]] Film::Accumulation read(Expr<uint2> pixel) const noexcept override;
    void release() const noexcept override;
    void clear(CommandBuffer &command_buffer) noexcept override;

protected:
    void _accumulate(Expr<uint2> pixel, Expr<float3> rgb, Expr<float> effective_spp) const noexcept override;
};

ColorFilmInstance::ColorFilmInstance(Device &device, Pipeline &pipeline, const ColorFilm *film) noexcept
    : Film::Instance{pipeline, film} {
    _clear_image = device.compile_async<1>([](BufferFloat image) noexcept {
        image.write(dispatch_x() * 4u + 0u, 0.f);
        image.write(dispatch_x() * 4u + 1u, 0.f);
        image.write(dispatch_x() * 4u + 2u, 0.f);
        image.write(dispatch_x() * 4u + 3u, 0.f);
    });
    _convert_image = device.compile_async<1>([this](BufferFloat accum, BufferFloat4 output) noexcept {
        auto i = dispatch_x();
        auto c0 = accum.read(i * 4u + 0u);
        auto c1 = accum.read(i * 4u + 1u);
        auto c2 = accum.read(i * 4u + 2u);
        auto n = max(accum.read(i * 4u + 3u), 1.f);
        auto scale = (1.f / n) * node<ColorFilm>()->scale();
        output.write(i, make_float4(scale * make_float3(c0, c1, c2), 1.f));
    });
}

void ColorFilmInstance::download(CommandBuffer &command_buffer, float4 *framebuffer) const noexcept {
    _check_prepared();
    auto resolution = node()->resolution();
    auto pixel_count = resolution.x * resolution.y;
    command_buffer << _convert_image.get()(_image, _converted).dispatch(pixel_count)
                   << _converted.copy_to(framebuffer);
}

void ColorFilmInstance::_accumulate(Expr<uint2> pixel, Expr<float3> rgb, Expr<float> effective_spp) const noexcept {
    _check_prepared();
    auto pixel_id = pixel.y * node()->resolution().x + pixel.x;
    $if(!any(isnan(rgb) || isinf(rgb))) {
        auto threshold = node<ColorFilm>()->clamp() * max(effective_spp, 1.f);
        auto strength = max(max(max(rgb.x, rgb.y), rgb.z), 0.f);
        auto c = rgb * (threshold / max(strength, threshold));
        for (auto i = 0u; i < 3u; i++) {
            _image.atomic(pixel_id * 4u + i).fetch_add(c[i]);
        }
        _image.atomic(pixel_id * 4u + 3u).fetch_add(effective_spp);
    }
    $else {
        if (node<ColorFilm>()->warn_nan()) {
            _image.write(pixel_id * 4u + 0u, std::numeric_limits<float>::infinity());
            _image.write(pixel_id * 4u + 1u, 0.f);
            _image.write(pixel_id * 4u + 2u, 0.f);
            _image.write(pixel_id * 4u + 3u, 1.f);
        }
    };
}

void ColorFilmInstance::prepare(CommandBuffer &command_buffer) noexcept {
    auto resolution = node()->resolution();
    auto pixel_count = resolution.x * resolution.y;
    if (!_image) { _image = pipeline().device().create_buffer<float>(pixel_count * 4u); }
    if (!_converted) { _converted = pipeline().device().create_buffer<float4>(pixel_count); }
    clear(command_buffer);
}

void ColorFilmInstance::clear(CommandBuffer &command_buffer) noexcept {
    auto resolution = node()->resolution();
    auto pixel_count = resolution.x * resolution.y;
    command_buffer << _clear_image.get()(_image).dispatch(pixel_count);
}

Film::Accumulation ColorFilmInstance::read(Expr<uint2> pixel) const noexcept {
    _check_prepared();
    auto width = node()->resolution().x;
    auto i = pixel.y * width + pixel.x;
    auto c0 = as<float>(_image.read(i * 4u + 0u));
    auto c1 = as<float>(_image.read(i * 4u + 1u));
    auto c2 = as<float>(_image.read(i * 4u + 2u));
    auto n = _image.read(i * 4u + 3u);
    auto inv_n = (1.f / max(cast<float>(n), 1e-6f));
    auto scale = inv_n * node<ColorFilm>()->scale();
    return {.average = scale * make_float3(c0, c1, c2), .sample_count = n};
}

void ColorFilmInstance::release() const noexcept {
    _image = {};
    _converted = {};
}

luisa::unique_ptr<Film::Instance> ColorFilm::build(
    Pipeline &pipeline, CommandBuffer &command_buffer) const noexcept {
    return luisa::make_unique<ColorFilmInstance>(
        pipeline.device(), pipeline, this);
}

}// namespace luisa::render

LUISA_RENDER_MAKE_SCENE_NODE_PLUGIN(luisa::render::ColorFilm)
