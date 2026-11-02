#include "DisplayProfile.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <icm.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wrl/client.h>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace compositor::platform {
using Microsoft::WRL::ComPtr;
namespace {
void checked(HRESULT hr,const char* operation){if(FAILED(hr)){std::ostringstream s;s<<operation<<" failed (0x"<<std::hex<<unsigned(hr)<<')';throw std::runtime_error(s.str());}}
std::uint32_t be32(std::span<const std::uint8_t> b,size_t p){return (std::uint32_t(b[p])<<24)|(std::uint32_t(b[p+1])<<16)|(std::uint32_t(b[p+2])<<8)|b[p+3];}
void validateIcc(std::span<const std::uint8_t> bytes){
    if(bytes.size()<132||bytes.size()>16*1024*1024)throw std::runtime_error("显示 ICC 大小无效");
    const auto length=be32(bytes,0),count=be32(bytes,128);
    if(length!=bytes.size()||std::memcmp(bytes.data()+36,"acsp",4)||std::memcmp(bytes.data()+12,"mntr",4)||std::memcmp(bytes.data()+16,"RGB ",4))throw std::runtime_error("显示配置文件必须是完整的 RGB 显示器 ICC");
    if((std::memcmp(bytes.data()+20,"XYZ ",4)&&std::memcmp(bytes.data()+20,"Lab ",4))||count>4096||132ULL+std::uint64_t(count)*12>length)throw std::runtime_error("显示 ICC 标记表或连接空间无效");
    std::set<std::uint32_t> signatures;
    for(std::uint32_t i=0;i<count;++i){const size_t p=132+size_t(i)*12;const auto start=be32(bytes,p+4),size=be32(bytes,p+8);if(!signatures.insert(be32(bytes,p)).second||start%4||start<132ULL+std::uint64_t(count)*12||size<8||std::uint64_t(start)+size>length)throw std::runtime_error("显示 ICC 标记位于配置文件之外");}
    PROFILE profile{PROFILE_MEMBUFFER,const_cast<std::uint8_t*>(bytes.data()),DWORD(bytes.size())};
    HPROFILE handle=OpenColorProfileW(&profile,PROFILE_READ,FILE_SHARE_READ,OPEN_EXISTING);
    if(!handle)throw std::runtime_error("Windows 拒绝了该显示器 ICC 配置文件");
    BOOL valid=FALSE;const BOOL success=IsColorProfileValid(handle,&valid);CloseColorProfile(handle);
    if(!success||!valid)throw std::runtime_error("Windows 发现无效的显示器 ICC 配置文件");
}
std::string hash(std::span<const std::uint8_t> bytes){
    BCRYPT_ALG_HANDLE algorithm{};if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("无法初始化配置文件 SHA256");
    std::uint8_t digest[32]{};const auto status=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),digest,32);BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0)throw std::runtime_error("无法计算显示配置文件哈希");
    constexpr char hex[]="0123456789abcdef";std::string result;for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}return result;
}
imaging::RgbaImage copyPixels(const imaging::RgbaImage& source){imaging::RgbaImage out{source.width,source.height,size_t(source.width)*4,std::vector<std::uint8_t>(size_t(source.width)*source.height*4)};for(std::uint32_t y=0;y<source.height;++y)std::memcpy(out.pixels.data()+y*out.stride,source.pixels.data()+y*source.stride,out.stride);return out;}
}
DisplayProfile loadDisplayProfile(const std::filesystem::path& path){
    DisplayProfile result;result.path=path;
    try{
        std::ifstream file(path,std::ios::binary|std::ios::ate);const auto length=file.tellg();
        if(!file||length<132||length>16*1024*1024)throw std::runtime_error("显示 ICC 缺失或超出 16 MiB 配置文件预算");
        result.icc.resize(size_t(length));file.seekg(0);if(!file.read(reinterpret_cast<char*>(result.icc.data()),length))throw std::runtime_error("无法读取完整的显示 ICC");
        validateIcc(result.icc);result.sha256=hash(result.icc);result.kind=DisplayProfileKind::Icc;
    }catch(const std::exception& e){result.icc.clear();result.sha256.clear();result.diagnostic=std::string(e.what())+"; using sRGB presentation";}
    return result;
}
DisplayProfile discoverDisplayProfile(void* nativeWindow){
    DisplayProfile fallback;MONITORINFOEXW info{};info.cbSize=sizeof(info);
    const auto monitor=MonitorFromWindow(static_cast<HWND>(nativeWindow),MONITOR_DEFAULTTONEAREST);
    if(!monitor||!GetMonitorInfoW(monitor,&info)){fallback.diagnostic="无法识别显示器；将使用 sRGB 呈现";return fallback;}
    fallback.monitorDevice=info.szDevice;
    HDC dc=CreateDCW(L"DISPLAY",info.szDevice,nullptr,nullptr);
    if(!dc){fallback.diagnostic="无法读取显示设备上下文；将使用 sRGB 呈现";return fallback;}
    DWORD count=0;GetICMProfileW(dc,&count,nullptr);
    if(!count||count>32768){DeleteDC(dc);fallback.diagnostic="Windows 报告没有显示器配置文件；将使用 sRGB（这也是高级颜色的惯例）";return fallback;}
    std::vector<wchar_t> path(count+1);const auto success=GetICMProfileW(dc,&count,path.data());DeleteDC(dc);
    if(!success){fallback.diagnostic="无法检索显示配置文件；将使用 sRGB 呈现";return fallback;}
    auto result=loadDisplayProfile(std::filesystem::path(path.data()));result.monitorDevice=info.szDevice;return result;
}
struct DisplayColorConverter::Impl {
    ComPtr<ID3D11Device> d3d;ComPtr<ID2D1Device> device;ComPtr<ID2D1DeviceContext> context;ComPtr<ID2D1Factory1> factory;
    std::string adapter;
};
DisplayColorConverter::DisplayColorConverter(bool forceWarp):impl_(std::make_unique<Impl>()){
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL obtained{};
    auto create=[&](D3D_DRIVER_TYPE type){return D3D11CreateDevice(nullptr,type,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,1,D3D11_SDK_VERSION,&impl_->d3d,&obtained,nullptr);};
    auto hr=create(forceWarp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE);if(FAILED(hr)&&!forceWarp){impl_->d3d.Reset();hr=create(D3D_DRIVER_TYPE_WARP);}checked(hr,"创建显示颜色设备");
    ComPtr<IDXGIDevice> dxgi;checked(impl_->d3d.As(&dxgi),"读取显示 DXGI 设备");ComPtr<IDXGIAdapter> adapter;checked(dxgi->GetAdapter(&adapter),"读取颜色适配器");DXGI_ADAPTER_DESC description{};checked(adapter->GetDesc(&description),"读取颜色适配器名称");
    char name[512]{};WideCharToMultiByte(CP_UTF8,0,description.Description,-1,name,sizeof(name),nullptr,nullptr);impl_->adapter=name;
    checked(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,IID_PPV_ARGS(&impl_->factory)),"创建显示颜色工厂");
    checked(impl_->factory->CreateDevice(dxgi.Get(),&impl_->device),"创建显示 D2D 设备");checked(impl_->device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&impl_->context),"创建显示颜色上下文");
    impl_->context->SetDpi(96,96);
}
DisplayColorConverter::~DisplayColorConverter()=default;
std::string DisplayColorConverter::adapterName()const{return impl_->adapter;}
DisplayConversion DisplayColorConverter::convert(const imaging::RgbaImage& source,const DisplayProfile& profile,const DisplayConversionLimits& limits){
    if(!limits.tileSide||limits.tileSide>1024||source.width>30000||source.height>30000||std::uint64_t(source.width)*source.height*4>limits.maxOutputBytes)throw std::invalid_argument("显示转换超出其视口输出预算");
    imaging::validate(source);
    DisplayConversion result;result.pixels=copyPixels(source);result.profileHash=profile.sha256;
    if(profile.kind!=DisplayProfileKind::Icc){result.fallback=true;result.diagnostic=profile.diagnostic.empty()?"未提供显示器 ICC 配置；将使用 sRGB 呈现":profile.diagnostic;return result;}
    try{
        validateIcc(profile.icc);if(hash(profile.icc)!=profile.sha256)throw std::runtime_error("显示 ICC 字节不再与已验证的配置文件哈希匹配");
        auto* context=impl_->context.Get();ComPtr<ID2D1ColorContext> srgb,destination;
        checked(context->CreateColorContext(D2D1_COLOR_SPACE_SRGB,nullptr,0,&srgb),"创建规范 sRGB 上下文");checked(context->CreateColorContext(D2D1_COLOR_SPACE_CUSTOM,profile.icc.data(),UINT32(profile.icc.size()),&destination),"创建显示器 ICC 上下文");
        ComPtr<ID2D1Effect> effect;checked(context->CreateEffect(CLSID_D2D1ColorManagement,&effect),"创建显示颜色管理效果");
        checked(effect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT,srgb.Get()),"设置 sRGB 源配置文件");checked(effect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,destination.Get()),"设置显示器配置文件");
        checked(effect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_RENDERING_INTENT,D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC),"设置源渲染意图");checked(effect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_RENDERING_INTENT,D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC),"设置显示器渲染意图");
        checked(effect->SetValue(D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE,D2D1_COLORMANAGEMENT_ALPHA_MODE_PREMULTIPLIED),"Set presentation alpha mode");
        const auto format=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
        for(UINT y=0;y<source.height;y+=limits.tileSide)for(UINT x=0;x<source.width;x+=limits.tileSide){
            const UINT width=std::min(limits.tileSide,source.width-x),height=std::min(limits.tileSide,source.height-y);std::vector<std::uint8_t> bgra(size_t(width)*height*4);
            for(UINT row=0;row<height;++row)for(UINT col=0;col<width;++col){const auto* p=source.pixels.data()+(y+row)*source.stride+size_t(x+col)*4;auto* q=bgra.data()+(size_t(row)*width+col)*4;q[0]=p[2];q[1]=p[1];q[2]=p[0];q[3]=p[3];}
            const auto size=D2D1::SizeU(width,height);ComPtr<ID2D1Bitmap1> input,target,staging;
            checked(context->CreateBitmap(size,bgra.data(),width*4,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,format),&input),"创建颜色源图块");
            checked(context->CreateBitmap(size,nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,format),&target),"创建颜色目标图块");
            checked(context->CreateBitmap(size,nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,format),&staging),"创建颜色回读图块");
            effect->SetInput(0,input.Get());context->SetTarget(target.Get());context->BeginDraw();context->Clear(D2D1::ColorF(0,0));context->DrawImage(effect.Get(),D2D1::Point2F(0,0),D2D1::RectF(0,0,float(width),float(height)),D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,D2D1_COMPOSITE_MODE_SOURCE_COPY);const auto draw=context->EndDraw();context->SetTarget(nullptr);checked(draw,"转换显示器颜色图块");
            checked(staging->CopyFromBitmap(nullptr,target.Get(),nullptr),"复制显示颜色图块");D2D1_MAPPED_RECT mapping{};checked(staging->Map(D2D1_MAP_OPTIONS_READ,&mapping),"读取显示颜色图块");
            for(UINT row=0;row<height;++row)for(UINT col=0;col<width;++col){const auto* p=mapping.bits+size_t(row)*mapping.pitch+size_t(col)*4;auto* q=result.pixels.pixels.data()+(y+row)*result.pixels.stride+size_t(x+col)*4;const auto alpha=source.pixels[size_t(y+row)*source.stride+size_t(x+col)*4+3];q[0]=std::min(p[2],alpha);q[1]=std::min(p[1],alpha);q[2]=std::min(p[0],alpha);q[3]=alpha;}
            checked(staging->Unmap(),"取消映射显示颜色图块");
        }
        result.converted=true;
    }catch(const std::exception& e){impl_->context->SetTarget(nullptr);result.pixels=copyPixels(source);result.fallback=true;result.diagnostic=std::string(e.what())+"; using sRGB presentation";}
    return result;
}
}
