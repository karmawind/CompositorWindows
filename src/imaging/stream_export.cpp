#include "export_temporary_file.h"
#include <propvarutil.h>
#include <array>
#include <cmath>
#include <sstream>

namespace compositor::imaging {
using Microsoft::WRL::ComPtr;
namespace {
using detail::Apartment;
using detail::cancelled;
using detail::factory;
using detail::fitPreview;
using detail::Handle;
using detail::ok;
using detail::regularFile;
using detail::TemporaryFile;
RgbaImage jpegPreview(IWICImagingFactory* imaging, const std::filesystem::path& path, UINT requestedWidth, UINT requestedHeight,
                      std::size_t availableBytes, std::size_t& peakBytes, const std::function<bool()>& check) {
    cancelled(check);
    ComPtr<IWICBitmapDecoder> decoder;
    ok(imaging->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "打开已编码的 JPEG 预览");
    ComPtr<IWICBitmapFrameDecode> frame; ok(decoder->GetFrame(0, &frame), "读取已编码的 JPEG 预览帧");
    ComPtr<IWICBitmapSourceTransform> scaled;
    ok(frame.As(&scaled), "JPEG 编解码器不支持有界的原生预览降采样");
    UINT width = requestedWidth, height = requestedHeight;
    ok(scaled->GetClosestSize(&width, &height), "选择有界 JPEG 解码尺寸");
    GUID format = GUID_WICPixelFormat32bppBGRA;
    ok(scaled->GetClosestPixelFormat(&format), "选择有界 JPEG 解码格式");
    UINT channels = format == GUID_WICPixelFormat32bppBGRA ? 4 : format == GUID_WICPixelFormat24bppBGR ? 3 : 0;
    if (!channels || !width || !height || width > 30000 || height > 30000) throw std::runtime_error("不支持的受限 JPEG 预览格式");
    const auto bytes = std::uint64_t(width) * height * channels;
    const auto outputBytes = std::uint64_t(requestedWidth) * requestedHeight * 4;
    // The native JPEG decoder supports downscaling by up to eight. A 100MP
    // source therefore needs at most 1,562,500 intermediate pixels.
    if (std::uint64_t(width) * height > 2000000 || bytes * 2 + outputBytes > availableBytes || bytes > UINT_MAX)
        throw std::runtime_error("JPEG 预览超出有界解码预算");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes));
    ok(scaled->CopyPixels(nullptr, width, height, &format, WICBitmapTransformRotate0, width * channels,
                          UINT(bytes), pixels.data()), "解码有界编码 JPEG 像素");
    cancelled(check);
    ComPtr<IWICBitmap> bitmap;
    ok(imaging->CreateBitmapFromMemory(width, height, format, width * channels, UINT(bytes), pixels.data(), &bitmap), "创建小型 JPEG 预览位图");
    // Apply the same declared-profile normalization as normal import. Raw JPEG
    // decoder channels can differ by a byte from the normalized document space.
    UINT profileCount = 0; ok(frame->GetColorContexts(0, nullptr, &profileCount), "读取已编码的 JPEG 配置文件数量");
    if (profileCount != 1) throw std::runtime_error("编码后的 JPEG 缺少 sRGB 配置文件");
    ComPtr<IWICColorContext> profile, srgb;
    ok(imaging->CreateColorContext(&profile), "创建编码 JPEG 配置文件上下文");
    IWICColorContext* contexts[]{profile.Get()}; ok(frame->GetColorContexts(1, contexts, &profileCount), "读取已编码的 JPEG 配置文件");
    ok(imaging->CreateColorContext(&srgb), "创建预览 sRGB 上下文"); ok(srgb->InitializeFromExifColorSpace(1), "初始化预览 sRGB 上下文");
    ComPtr<IWICColorTransform> normalized; ok(imaging->CreateColorTransformer(&normalized), "创建 JPEG 预览配置文件变换");
    ok(normalized->Initialize(bitmap.Get(), profile.Get(), srgb.Get(), GUID_WICPixelFormat32bppRGBA), "规范化已编码 JPEG 预览的配置文件");
    ComPtr<IWICBitmapScaler> scaler; ok(imaging->CreateBitmapScaler(&scaler), "创建 JPEG 预览缩放器");
    ok(scaler->Initialize(normalized.Get(), requestedWidth, requestedHeight, WICBitmapInterpolationModeFant), "缩放已编码的 JPEG 预览");
    ComPtr<IWICFormatConverter> converter; ok(imaging->CreateFormatConverter(&converter), "创建 JPEG 预览转换器");
    ok(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "将 JPEG 预览转换为 RGBA");
    RgbaImage result{requestedWidth, requestedHeight, std::size_t(requestedWidth) * 4, std::vector<std::uint8_t>(static_cast<std::size_t>(outputBytes))};
    ok(converter->CopyPixels(nullptr, UINT(result.stride), UINT(result.pixels.size()), result.pixels.data()), "读取已编码的 JPEG 预览");
    peakBytes = std::max(peakBytes, static_cast<std::size_t>(bytes * 2 + outputBytes));
    cancelled(check);
    return result;
}
void writeProfile(IWICImagingFactory* imaging, IWICBitmapFrameEncode* frame, ImageFormat format) {
    if (format == ImageFormat::Png) {
        ComPtr<IWICMetadataQueryWriter> metadata; ok(frame->GetMetadataQueryWriter(&metadata), "获取 PNG 导出元数据");
        PROPVARIANT value; PropVariantInit(&value); value.vt = VT_UI1; value.bVal = 0;
        ok(metadata->SetMetadataByName(L"/sRGB/RenderingIntent", &value), "写入 PNG sRGB 声明");
    } else {
        wchar_t system[MAX_PATH]{};
        if (!GetSystemDirectoryW(system, MAX_PATH)) throw std::runtime_error("无法找到 Windows sRGB 配置文件");
        auto path = std::filesystem::path(system) / L"spool/drivers/color/sRGB Color Space Profile.icm";
        ComPtr<IWICColorContext> color; ok(imaging->CreateColorContext(&color), "创建导出颜色上下文");
        ok(color->InitializeFromFilename(path.c_str()), "加载 Windows sRGB 配置文件");
        IWICColorContext* colors[]{color.Get()}; ok(frame->SetColorContexts(1, colors), "导出时嵌入 sRGB 配置文件");
    }
}
}

void validateExportExtent(std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > maxExportSide || height > maxExportSide || std::uint64_t(width) * height > maxExportPixels)
        throw std::invalid_argument("图像导出支持最大 1 亿像素且单边不超过 30,000 像素的画布。");
}

StreamingExportResult encodeRowsAtomic(const std::filesystem::path& destination, std::uint32_t width, std::uint32_t height,
    const RgbaRows& rows, const ExportOptions& options, const StreamExportLimits& limits, const ExportProgress& progress) {
    validateExportExtent(width, height);
    if (!rows || !std::isfinite(options.dpi) || options.dpi <= 0 || !std::isfinite(options.jpegQuality) ||
        (options.format != ImageFormat::Png && options.format != ImageFormat::Jpeg) || !limits.stripeRows || limits.stripeRows > 256 ||
        limits.previewWidth > 1000 || limits.previewHeight > 1000 || limits.maxBufferBytes > 1024ULL * 1024 * 1024)
        throw std::invalid_argument("无效的流式图像导出设置");
    cancelled(options.cancelled);
    const auto [previewWidth, previewHeight] = fitPreview(width, height, limits.previewWidth, limits.previewHeight);
    const bool pngPreview = options.format == ImageFormat::Png && previewWidth && previewHeight;
    using Accumulator = std::array<std::uint64_t, 4>;
    const std::size_t previewPixels = std::size_t(previewWidth) * previewHeight;
    const std::size_t previewStorage = pngPreview ? previewPixels * (sizeof(Accumulator) + 4) + std::size_t(width) * sizeof(UINT) : 0;
    // Reserve 8MiB for the canonical renderer's bounded tile/adjustment buffers.
    constexpr std::size_t renderReserve = 8 * 1024 * 1024;
    const UINT channels = options.format == ImageFormat::Jpeg ? 3U : 4U;
    const std::size_t bytesPerRow = std::size_t(width) * (4 + channels);
    if (limits.maxBufferBytes <= previewStorage + renderReserve + bytesPerRow)
        throw std::invalid_argument("流式导出缓冲区预算过小");
    const auto stripeRows = UINT(std::min<std::size_t>({height, limits.stripeRows, (limits.maxBufferBytes - previewStorage - renderReserve) / bytesPerRow}));
    std::vector<std::uint8_t> rgba(std::size_t(width) * stripeRows * 4), encoded(std::size_t(width) * stripeRows * channels);
    std::vector<Accumulator> sums(pngPreview ? previewPixels : 0);
    std::vector<UINT> previewX(pngPreview ? width : 0);
    if (pngPreview) for (UINT x = 0; x < width; ++x) previewX[x] = UINT(std::uint64_t(x) * previewWidth / width);
    StreamingExportResult result; result.stripeRows = stripeRows;
    result.peakOwnedPixelBytes = rgba.size() + encoded.size() + previewStorage;
    Apartment apartment; auto imaging = factory(); TemporaryFile temporary(destination);
    {
        ComPtr<IWICStream> stream; ok(imaging->CreateStream(&stream), "创建流式导出文件");
        ok(stream->InitializeFromFilename(temporary.path.c_str(), GENERIC_WRITE), "打开流式导出文件");
        ComPtr<IWICBitmapEncoder> encoder;
        ok(imaging->CreateEncoder(options.format == ImageFormat::Png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, &encoder), "创建流式图像编码器");
        ok(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "初始化无图像缓存的扫描行编码器");
        ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> properties;
        ok(encoder->CreateNewFrame(&frame, &properties), "创建流式导出帧");
        if (options.format == ImageFormat::Jpeg) {
            PROPBAG2 property{}; property.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value; VariantInit(&value); value.vt = VT_R4; value.fltVal = float(std::clamp(options.jpegQuality, 0., 1.));
            ok(properties->Write(1, &property, &value), "设置 JPEG 导出质量");
        }
        ok(frame->Initialize(properties.Get()), "初始化流式导出帧");
        ok(frame->SetSize(width, height), "设置导出尺寸"); ok(frame->SetResolution(options.dpi, options.dpi), "设置导出分辨率");
        GUID format = options.format == ImageFormat::Png ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        const auto requested = format; ok(frame->SetPixelFormat(&format), "选择导出像素格式");
        if (format != requested) throw std::runtime_error("编码器更改了所需的导出像素格式");
        writeProfile(imaging.Get(), frame.Get(), options.format);
        const std::array<std::uint8_t, 3> matte{options.matteR, options.matteG, options.matteB};
        for (UINT y = 0; y < height; y += stripeRows) {
            cancelled(options.cancelled); const UINT count = std::min(stripeRows, height - y);
            rows(y, count, std::span(rgba).first(std::size_t(width) * count * 4), std::size_t(width) * 4);
            for (UINT row = 0; row < count; ++row) {
                cancelled(options.cancelled);
                for (UINT x = 0; x < width; ++x) {
                    const auto* p = rgba.data() + (std::size_t(row) * width + x) * 4;
                    auto* q = encoded.data() + (std::size_t(row) * width + x) * channels;
                    if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) throw std::runtime_error("导出行包含无效的预乘像素");
                    for (UINT c = 0; c < 3; ++c) q[2 - c] = channels == 3 ? std::uint8_t(std::min(255U, unsigned(p[c]) + (unsigned(matte[c]) * (255 - p[3]) + 127) / 255))
                        : p[3] ? std::uint8_t(std::min(255U, (unsigned(p[c]) * 255 + p[3] / 2) / p[3])) : 0;
                    if (channels == 4) q[3] = p[3];
                    if (pngPreview) {
                        auto& sum = sums[std::size_t(std::uint64_t(y + row) * previewHeight / height) * previewWidth + previewX[x]];
                        for (UINT c = 0; c < 3; ++c) sum[c] += (unsigned(q[2 - c]) * p[3] + 127) / 255;
                        sum[3] += p[3];
                    }
                }
            }
            ok(frame->WritePixels(count, width * channels, width * count * channels, encoded.data()), "编码图像扫描行");
            if (progress) progress(y + count, height);
        }
        cancelled(options.cancelled); ok(frame->Commit(), "提交流式图像帧"); ok(encoder->Commit(), "提交流式图像容器");
    }
    cancelled(options.cancelled);
    if (pngPreview) {
        result.preview = {previewWidth, previewHeight, std::size_t(previewWidth) * 4, std::vector<std::uint8_t>(previewPixels * 4)};
        auto boundary = [](UINT i, UINT source, UINT target) { return (std::uint64_t(i) * source + target - 1) / target; };
        for (UINT y = 0; y < previewHeight; ++y) for (UINT x = 0; x < previewWidth; ++x) {
            const auto count = (boundary(x + 1, width, previewWidth) - boundary(x, width, previewWidth)) * (boundary(y + 1, height, previewHeight) - boundary(y, height, previewHeight));
            for (UINT c = 0; c < 4; ++c) result.preview.pixels[(std::size_t(y) * previewWidth + x) * 4 + c] = std::uint8_t((sums[std::size_t(y) * previewWidth + x][c] + count / 2) / count);
        }
    } else if (previewWidth && previewHeight) {
        // Release stripe allocations before decoding the encoded JPEG thumbnail.
        std::vector<std::uint8_t>().swap(rgba); std::vector<std::uint8_t>().swap(encoded);
        result.preview = jpegPreview(imaging.Get(), temporary.path, previewWidth, previewHeight, limits.maxBufferBytes - renderReserve,
                                     result.peakOwnedPixelBytes, options.cancelled);
    }
    result.encodedBytes = std::filesystem::file_size(temporary.path);
    temporary.replace(destination, options.cancelled);
    return result;
}

void copyEncodedAtomic(const std::filesystem::path& source, const std::filesystem::path& destination, const std::function<bool()>& check) {
    cancelled(check); const auto inputPath=win32FilePath(source); TemporaryFile temporary(destination);
    {
        Handle input{CreateFileW(inputPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        Handle output{CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (input.value == INVALID_HANDLE_VALUE || output.value == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开要导出的编码图像");
        regularFile(input.value); regularFile(output.value);
        std::vector<std::uint8_t> buffer(1024 * 1024);
        for (;;) {
            cancelled(check); DWORD count = 0;
            if (!ReadFile(input.value, buffer.data(), DWORD(buffer.size()), &count, nullptr)) throw std::runtime_error("无法读取编码图像");
            if (!count) break;
            DWORD offset = 0;
            while (offset < count) { DWORD written = 0; if (!WriteFile(output.value, buffer.data() + offset, count - offset, &written, nullptr) || !written) throw std::runtime_error("无法写入编码图像"); offset += written; }
        }
        if (!FlushFileBuffers(output.value)) throw std::runtime_error("无法刷新导出的图像");
    }
    temporary.replace(destination, check);
}
}
