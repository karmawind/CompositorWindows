#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "wic_codec.h"
#include "win32_file_path.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <propvarutil.h>
#include <cmath>
#include <sstream>

namespace compositor::imaging {
using Microsoft::WRL::ComPtr;
namespace {
void ok(HRESULT hr,const char* what) { if(FAILED(hr)) { std::ostringstream s; s<<what<<" (HRESULT 0x"<<std::hex<<static_cast<unsigned long>(hr)<<")"; throw std::runtime_error(s.str()); } }
struct Apartment { HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED); Apartment(){if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE)ok(hr,"初始化 COM");} ~Apartment(){if(SUCCEEDED(hr))CoUninitialize();} };
ComPtr<IWICImagingFactory> factory(){ComPtr<IWICImagingFactory> f;ok(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"创建 WIC 工厂");return f;}
std::uint16_t orientation(IWICBitmapFrameDecode* frame){
    ComPtr<IWICMetadataQueryReader> r;if(FAILED(frame->GetMetadataQueryReader(&r)))return 1;
    for(auto query:{L"/app1/ifd/{ushort=274}",L"/ifd/{ushort=274}"}){PROPVARIANT v;PropVariantInit(&v);if(SUCCEEDED(r->GetMetadataByName(query,&v))){auto n=v.vt==VT_UI2?v.uiVal:1;PropVariantClear(&v);if(n>=1&&n<=8)return static_cast<std::uint16_t>(n);}else PropVariantClear(&v);}return 1;
}
ComPtr<IWICColorContext> srgb(IWICImagingFactory* f){ComPtr<IWICColorContext> c;ok(f->CreateColorContext(&c),"创建 sRGB 上下文");ok(c->InitializeFromExifColorSpace(1),"初始化 sRGB 上下文");return c;}
ComPtr<IWICBitmapSource> convert(IWICImagingFactory* f,IWICBitmapSource* source,REFWICPixelFormatGUID format){ComPtr<IWICFormatConverter> c;ok(f->CreateFormatConverter(&c),"创建转换器");ok(c->Initialize(source,format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"转换像素格式");ComPtr<IWICBitmapSource> out;ok(c.As(&out),"查询位图源");return out;}
void premultiply(std::vector<std::uint8_t>& p){for(std::size_t i=0;i<p.size();i+=4)for(int c=0;c<3;++c)p[i+c]=std::uint8_t((unsigned(p[i+c])*p[i+3]+127)/255);}
}
DecodedImage WicCodec::decode(const std::filesystem::path& filename,const ImportOptions& options){
    const auto path=win32FilePath(filename);
    checkCancelled(options);Apartment apartment;auto f=factory();ComPtr<IWICBitmapDecoder> decoder;
    ok(f->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder),"打开图像");
    GUID container{};ok(decoder->GetContainerFormat(&container),"读取图像类型");
    if(container!=GUID_ContainerFormatPng&&container!=GUID_ContainerFormatJpeg&&container!=GUID_ContainerFormatTiff)throw std::runtime_error("选择 PNG、JPEG、TIFF 或随附的 HEIC");
    ComPtr<IWICBitmapFrameDecode> frame;ok(decoder->GetFrame(0,&frame),"解码第一帧");UINT w=0,h=0;ok(frame->GetSize(&w,&h),"读取尺寸");
    auto bytes=checkedBytes(w,h,4,options);if(bytes>options.maxWorkingBytes/3)throw std::runtime_error("图像超出临时分配预算");
    DecodedImage out;out.metadata.originalOrientation=orientation(frame.Get());out.metadata.decoder="Windows 图像处理组件";
    if(FAILED(frame->GetResolution(&out.metadata.dpiX,&out.metadata.dpiY))){out.metadata.dpiX=out.metadata.dpiY=72;}
    ComPtr<IWICBitmapSource> source;ok(frame.As(&source),"获取帧源");UINT count=0;HRESULT colorHr=frame->GetColorContexts(0,nullptr,&count);
    if(FAILED(colorHr)&&colorHr!=WINCODEC_ERR_UNSUPPORTEDOPERATION)ok(colorHr,"读取颜色上下文数量");
    if(count>16)throw std::runtime_error("嵌入的颜色上下文过多");
    if(count){
        std::vector<ComPtr<IWICColorContext>> contexts(count);std::vector<IWICColorContext*> raw(count);
        for(UINT i=0;i<count;++i){ok(f->CreateColorContext(&contexts[i]),"创建嵌入颜色上下文");raw[i]=contexts[i].Get();}
        ok(frame->GetColorContexts(count,raw.data(),&count),"读取嵌入的颜色上下文");
        auto dest=srgb(f.Get());ComPtr<IWICColorTransform> transform;ok(f->CreateColorTransformer(&transform),"创建颜色变换");
        ok(transform->Initialize(source.Get(),contexts[0].Get(),dest.Get(),GUID_WICPixelFormat32bppRGBA),"将嵌入配置文件规范化为 sRGB");
        ok(transform.As(&source),"获取归一化源");out.metadata.profileStatus=ProfileStatus::ConvertedEmbeddedProfile;
    }else{
        GUID format{};ok(frame->GetPixelFormat(&format),"读取像素格式");
        if(format==GUID_WICPixelFormat32bppCMYK || format==GUID_WICPixelFormat64bppCMYK)throw std::runtime_error("无配置文件的 CMYK 不能假定为 sRGB");
        source=convert(f.Get(),source.Get(),GUID_WICPixelFormat32bppRGBA);
        out.metadata.profileStatus=ProfileStatus::AssumedSrgbNoProfile;
    }
    checkCancelled(options);std::vector<std::uint8_t> rgba(bytes);ok(source->CopyPixels(nullptr,w*4,static_cast<UINT>(bytes),rgba.data()),"读取 RGBA 像素");
    const auto o=options.applyExifOrientation?out.metadata.originalOrientation:1;const bool transpose=o>=5;out.image.width=transpose?h:w;out.image.height=transpose?w:h;out.image.stride=std::size_t(out.image.width)*4;out.image.pixels.resize(bytes);
    for(UINT y=0;y<h;++y){checkCancelled(options);for(UINT x=0;x<w;++x){UINT dx=x,dy=y;
        switch(o){case 2:dx=w-1-x;break;case 3:dx=w-1-x;dy=h-1-y;break;case 4:dy=h-1-y;break;case 5:dx=y;dy=x;break;case 6:dx=h-1-y;dy=x;break;case 7:dx=h-1-y;dy=w-1-x;break;case 8:dx=y;dy=w-1-x;break;default:break;}
        std::copy_n(rgba.data()+(std::size_t(y)*w+x)*4,4,out.image.pixels.data()+std::size_t(dy)*out.image.stride+dx*4);
    }}
    if(transpose)std::swap(out.metadata.dpiX,out.metadata.dpiY);premultiply(out.image.pixels);return out;
}
void WicCodec::normalizeStraightRgba(std::span<std::uint8_t> rgba,std::uint32_t w,std::uint32_t h,std::span<const std::uint8_t> icc){
    if(rgba.size()!=checkedBytes(w,h,4)||icc.empty()||icc.size()>16*1024*1024)throw std::runtime_error("无效的 ICC 转换输入");
    Apartment apartment;auto f=factory();ComPtr<IWICBitmap> bitmap;ok(f->CreateBitmapFromMemory(w,h,GUID_WICPixelFormat32bppRGBA,w*4,static_cast<UINT>(rgba.size()),rgba.data(),&bitmap),"创建 ICC 源位图");
    ComPtr<IWICColorContext> context;ok(f->CreateColorContext(&context),"创建 ICC 上下文");ok(context->InitializeFromMemory(const_cast<BYTE*>(icc.data()),static_cast<UINT>(icc.size())),"读取 ICC 配置文件");
    auto dest=srgb(f.Get());ComPtr<IWICColorTransform> transform;ok(f->CreateColorTransformer(&transform),"创建 ICC 变换");ok(transform->Initialize(bitmap.Get(),context.Get(),dest.Get(),GUID_WICPixelFormat32bppRGBA),"将 HEIC ICC 规范化为 sRGB");
    std::vector<std::uint8_t> copy(rgba.size());ok(transform->CopyPixels(nullptr,w*4,static_cast<UINT>(copy.size()),copy.data()),"读取 ICC 转换后的像素");std::copy(copy.begin(),copy.end(),rgba.begin());
}
void WicCodec::encode(const std::filesystem::path& filename,const RgbaImage& image,const ExportOptions& options){
    if(options.format==ImageFormat::Exr)throw std::runtime_error("OpenEXR 需要 HDR 文档导出器");
    const auto path=win32FilePath(filename);
    validate(image);if(!std::isfinite(options.dpi)||options.dpi<=0||!std::isfinite(options.jpegQuality)||options.format==ImageFormat::Pdf)throw std::runtime_error("无效的导出设置");
    auto cancelled=[&]{if(options.cancelled&&options.cancelled())throw std::runtime_error("图像导出已取消");};cancelled();Apartment apartment;auto f=factory();
    GUID id{};ok(CoCreateGuid(&id),"创建输出标识符");wchar_t guid[40]{};StringFromGUID2(id,guid,40);auto temp=path;temp+=std::wstring(L".")+guid+L".tmp";
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);}}cleanup{temp};
    {
    ComPtr<IWICStream> stream;ok(f->CreateStream(&stream),"创建导出流");ok(stream->InitializeFromFilename(temp.c_str(),GENERIC_WRITE),"打开临时导出文件");
    const GUID& container=options.format==ImageFormat::Jpeg?GUID_ContainerFormatJpeg:options.format==ImageFormat::Tiff?GUID_ContainerFormatTiff:GUID_ContainerFormatPng;
    ComPtr<IWICBitmapEncoder> encoder;ok(f->CreateEncoder(container,nullptr,&encoder),"创建编码器");ok(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"初始化编码器");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;ok(encoder->CreateNewFrame(&frame,&props),"创建导出帧");
    if(options.format==ImageFormat::Jpeg){PROPBAG2 p{};p.pstrName=const_cast<LPOLESTR>(L"ImageQuality");VARIANT v;VariantInit(&v);v.vt=VT_R4;v.fltVal=static_cast<float>(std::clamp(options.jpegQuality,0.0,1.0));ok(props->Write(1,&p,&v),"设置 JPEG 质量");}
    ok(frame->Initialize(props.Get()),"初始化帧");ok(frame->SetSize(image.width,image.height),"设置导出尺寸");ok(frame->SetResolution(options.dpi,options.dpi),"Set DPI");
    if(options.format!=ImageFormat::Png){
        wchar_t system[MAX_PATH]{};if(!GetSystemDirectoryW(system,MAX_PATH))throw std::runtime_error("无法找到 Windows 颜色配置文件");
        auto profile=std::filesystem::path(system)/L"spool/drivers/color/sRGB Color Space Profile.icm";
        ComPtr<IWICColorContext> color;ok(f->CreateColorContext(&color),"创建导出配置文件");ok(color->InitializeFromFilename(profile.c_str()),"加载 Windows sRGB 配置文件");IWICColorContext* colors[]={color.Get()};ok(frame->SetColorContexts(1,colors),"设置导出 sRGB 配置文件");
    }
    if(options.format==ImageFormat::Png){ComPtr<IWICMetadataQueryWriter> metadata;ok(frame->GetMetadataQueryWriter(&metadata),"获取 PNG 元数据");PROPVARIANT v;PropVariantInit(&v);v.vt=VT_UI1;v.bVal=0;ok(metadata->SetMetadataByName(L"/sRGB/RenderingIntent",&v),"写入 PNG sRGB 声明");}
    const auto bytes=checkedBytes(image.width,image.height,4);std::vector<std::uint8_t> straight(bytes);
    const std::uint8_t matte[]={options.matteR,options.matteG,options.matteB};
    for(std::uint32_t y=0;y<image.height;++y){cancelled();for(std::uint32_t x=0;x<image.width;++x){const auto* p=&image.pixels[y*image.stride+x*4];auto* q=&straight[(std::size_t(y)*image.width+x)*4];
        for(int c=0;c<3;++c)q[c]=options.format==ImageFormat::Jpeg?std::uint8_t(std::min(255U,unsigned(p[c])+(unsigned(matte[c])*(255-p[3])+127)/255)):p[3]?std::uint8_t(std::min(255U,(unsigned(p[c])*255+p[3]/2)/p[3])):0;
        q[3]=options.format==ImageFormat::Jpeg?255:p[3];
    }}
    ComPtr<IWICBitmap> bitmap;ok(f->CreateBitmapFromMemory(image.width,image.height,GUID_WICPixelFormat32bppRGBA,image.width*4,static_cast<UINT>(bytes),straight.data(),&bitmap),"创建导出位图");
    GUID format=options.format==ImageFormat::Jpeg?GUID_WICPixelFormat24bppBGR:GUID_WICPixelFormat32bppBGRA;ok(frame->SetPixelFormat(&format),"协商编码像素格式");auto source=convert(f.Get(),bitmap.Get(),format);ok(frame->WriteSource(source.Get(),nullptr),"编码像素");cancelled();ok(frame->Commit(),"提交导出帧");ok(encoder->Commit(),"提交导出容器");
    }cancelled();if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("原子导出替换失败");
}
}
