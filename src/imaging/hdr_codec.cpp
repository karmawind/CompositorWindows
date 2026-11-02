#include "hdr_codec.h"
#include <ImfIO.h>
#include <ImfInputFile.h>
#include <ImfTiledInputFile.h>
#include <ImfOutputFile.h>
#include <ImfHeader.h>
#include <ImfFrameBuffer.h>
#include <ImfChannelList.h>
#include <ImfVersion.h>
#include <ImfStandardAttributes.h>
#include <fstream>
#include <cmath>
#include <cstring>
#include <memory>
#include <cstdlib>
#include <cstddef>

namespace {
// stb_image's allocations are bounded even if a file changes after its info
// pass. Allocation callbacks never throw through the C decoder's cleanup.
thread_local size_t hdrStbRemaining=1200000000;
struct alignas(std::max_align_t) HdrAllocation {size_t bytes;};
void* hdrStbAllocate(size_t size){if(size>hdrStbRemaining||size>SIZE_MAX-sizeof(HdrAllocation))return nullptr;auto* block=static_cast<HdrAllocation*>(std::malloc(sizeof(HdrAllocation)+size));if(!block)return nullptr;block->bytes=size;hdrStbRemaining-=size;return block+1;}
void hdrStbFree(void* data){if(data){auto* block=static_cast<HdrAllocation*>(data)-1;hdrStbRemaining+=block->bytes;std::free(block);}}
void* hdrStbReallocate(void* data,size_t size){if(!data)return hdrStbAllocate(size);auto* old=static_cast<HdrAllocation*>(data)-1;if(size>hdrStbRemaining+old->bytes)return nullptr;auto* next=hdrStbAllocate(size);if(!next)return nullptr;std::memcpy(next,data,std::min(size,old->bytes));hdrStbFree(data);return next;}
}
#define STBI_MALLOC(size) hdrStbAllocate(size)
#define STBI_REALLOC(data,size) hdrStbReallocate(data,size)
#define STBI_FREE(data) hdrStbFree(data)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 30000
#include "third_party/stb_image.h"

namespace compositor::imaging {
namespace {
void check(const std::function<bool()>& cancelled){if(cancelled&&cancelled())throw std::runtime_error("HDR 操作已取消");}
class Input final:public Imf::IStream {
    std::ifstream file_;std::function<bool()> cancelled_;uint64_t size_{};
public:
    Input(const std::filesystem::path& path,std::function<bool()> cancelled):Imf::IStream("HDR 图像"),file_(path,std::ios::binary),cancelled_(std::move(cancelled)){if(!file_)throw std::runtime_error("无法打开 HDR 图像");file_.seekg(0,std::ios::end);size_=uint64_t(file_.tellg());file_.seekg(0);}
    bool read(char* data,int n)override{check(cancelled_);if(n<0||!file_.read(data,n))throw std::runtime_error("EXR 图像已截断");return uint64_t(file_.tellg())<size_;}
    uint64_t tellg()override{const auto p=file_.tellg();if(p<0)throw std::runtime_error("无法获取 EXR 位置");return uint64_t(p);}
    void seekg(uint64_t p)override{check(cancelled_);if(p>uint64_t(std::numeric_limits<std::streamoff>::max()))throw std::runtime_error("无效的 EXR 文件偏移");file_.clear();file_.seekg(std::streamoff(p));if(!file_)throw std::runtime_error("无效的 EXR 文件偏移");}
};
class Output final:public Imf::OStream {
    std::ofstream file_;std::function<bool()> cancelled_;
public:
    Output(const std::filesystem::path& path,std::function<bool()> cancelled):Imf::OStream("HDR 图像"),file_(path,std::ios::binary|std::ios::trunc),cancelled_(std::move(cancelled)){if(!file_)throw std::runtime_error("无法写入 EXR 图像");}
    void write(const char* data,int n)override{check(cancelled_);if(n<0||!file_.write(data,n))throw std::runtime_error("无法写入 EXR 像素");}
    uint64_t tellp()override{const auto p=file_.tellp();if(p<0)throw std::runtime_error("无法获取 EXR 输出位置");return uint64_t(p);}
    void seekp(uint64_t p)override{check(cancelled_);if(p>uint64_t(std::numeric_limits<std::streamoff>::max()))throw std::runtime_error("无效的 EXR 输出偏移");file_.seekp(std::streamoff(p));if(!file_)throw std::runtime_error("无法定位 EXR 输出");}
    void finish(){file_.flush();if(!file_)throw std::runtime_error("无法刷新 EXR 图像");}
};
void budget(int width,int height,const ImportOptions& options){
    checkedBytes(uint32_t(width),uint32_t(height),4,options,2);
    // Decoder RGBA32F + half pixels + SDR bytes and immutable tiles, plus a
    // conservative 16 MB codec allowance. Enforce before creating a framebuffer.
    const auto tiles=uint64_t((width+255)/256)*uint64_t((height+255)/256)*256*256*4;
    if(uint64_t(width)*height*28+tiles+16*1024*1024>options.maxWorkingBytes)throw std::runtime_error("HDR 解码超出工作内存预算");
}
HdrExtent header(const Imf::Header& h,int version,const ImportOptions& options){
    if(Imf::isMultiPart(version)||Imf::isNonImage(version))throw std::runtime_error("不支持深度和多部分 EXR 图像");
    const auto dw=h.dataWindow(),display=h.displayWindow();
    const int64_t width=int64_t(dw.max.x)-dw.min.x+1,height=int64_t(dw.max.y)-dw.min.y+1;
    if(width<1||height<1||width>30000||height>30000||display.max.x<display.min.x||display.max.y<display.min.y)throw std::runtime_error("无效的 EXR 图像窗口");
    // This importer is an RGB image workflow, not an arbitrary AOV/channel browser.
    const auto& channels=h.channels();for(const char* name:{"R","G","B"})if(!channels.findChannel(name))throw std::runtime_error("EXR 图像需要 R、G 和 B 通道");
    for(auto i=channels.begin();i!=channels.end();++i){const auto& c=i.channel();if((std::strcmp(i.name(),"R")&&std::strcmp(i.name(),"G")&&std::strcmp(i.name(),"B")&&std::strcmp(i.name(),"A"))||c.xSampling!=1||c.ySampling!=1||c.type==Imf::UINT)throw std::runtime_error("EXR 图像需要未降采样的 HALF 或 FLOAT RGB(A) 通道");}
    // Avoid silently treating ACES/AP1 and other primaries as sRGB. No general CMS.
    if(Imf::hasChromaticities(h)){
        const auto& c=Imf::chromaticities(h);const Imf::Chromaticities srgb;
        if((c.red-srgb.red).length()>.0001f||(c.green-srgb.green).length()>.0001f||(c.blue-srgb.blue).length()>.0001f||(c.white-srgb.white).length()>.0001f)throw std::runtime_error("EXR 色度必须是线性 sRGB/Rec.709");
    }
    budget(int(width),int(height),options);return {int(width),int(height)};
}
std::shared_ptr<const DeepImage> fromLinear(const std::vector<WidePixel>& pixels,int width,int height,const ImportOptions& options){
    auto image=makeDeep(width,height);
    for(int y=0;y<height;++y){checkCancelled(options);for(int x=0;x<width;++x){auto p=pixels[size_t(y)*width+x];for(float v:p)if(!std::isfinite(v))throw std::runtime_error("EXR 包含非有限值样本");if(p[3]<0||p[3]>1)throw std::runtime_error("HDR alpha 必须介于 0 和 1 之间");
        for(int c=0;c<3;++c)p[c]=p[3]>0?float(linearToSrgb(p[c]/p[3])*p[3]):0;image->setPixel(x,y,p);}}
    return image;
}
}
HdrExtent inspectExr(const std::filesystem::path& path,const ImportOptions& options){checkCancelled(options);Input stream(path,options.cancelled);Imf::InputFile file(stream,1);return header(file.header(),file.version(),options);}
std::shared_ptr<const DeepImage> decodeExr(const std::filesystem::path& path,const ImportOptions& options){
    checkCancelled(options);Input stream(path,options.cancelled);Imf::InputFile file(stream,1);const auto size=header(file.header(),file.version(),options);const auto dw=file.header().dataWindow();
    std::vector<WidePixel> pixels(size_t(size.width)*size.height);Imf::FrameBuffer frame;
    const char* names[]{"R","G","B","A"};for(int c=0;c<4;++c)frame.insert(names[c],Imf::Slice::Make(Imf::FLOAT,pixels.data()->data()+c,dw,sizeof(WidePixel),size_t(size.width)*sizeof(WidePixel),1,1,c==3?1:0));
    if(Imf::isTiled(file.version())){Input tiledStream(path,options.cancelled);Imf::TiledInputFile tiled(tiledStream,1);tiled.setFrameBuffer(frame);for(int ty=0;ty<tiled.numYTiles();++ty){checkCancelled(options);for(int tx=0;tx<tiled.numXTiles();++tx)tiled.readTile(tx,ty);}}
    else {file.setFrameBuffer(frame);for(int row=0;row<size.height;++row){checkCancelled(options);file.readPixels(dw.min.y+row);}}
    return fromLinear(pixels,size.width,size.height,options);
}
std::shared_ptr<const DeepImage> decodeRadiance(const std::filesystem::path& path,const ImportOptions& options){
    checkCancelled(options);std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("无法打开 Radiance HDR 图像");
    struct Reader {std::ifstream* file;const ImportOptions* options;};Reader reader{&file,&options};
    const stbi_io_callbacks callbacks{
        [](void* u,char* out,int n)->int{auto& reader=*static_cast<Reader*>(u);if(reader.options->cancelled&&reader.options->cancelled())return 0;auto& f=*reader.file;f.read(out,n);return int(f.gcount());},
        [](void* u,int n){auto& f=*static_cast<Reader*>(u)->file;f.clear();f.seekg(n,std::ios::cur);},
        [](void* u)->int{return static_cast<Reader*>(u)->file->eof();}};
    hdrStbRemaining=size_t(std::min<uint64_t>(options.maxWorkingBytes,SIZE_MAX));
    int width=0,height=0,channels=0;if(!stbi_info_from_callbacks(&callbacks,&reader,&width,&height,&channels))throw std::runtime_error("无效的 Radiance HDR 文件头");budget(width,height,options);file.clear();file.seekg(0);hdrStbRemaining=size_t(options.maxWorkingBytes-uint64_t(width)*height*12);
    // stb callbacks cannot throw across decoder cleanup; cancellation is polled
    // before/after its bounded decode and throughout conversion.
    std::unique_ptr<float,decltype(&stbi_image_free)> decoded(stbi_loadf_from_callbacks(&callbacks,&reader,&width,&height,&channels,4),stbi_image_free);
    checkCancelled(options);if(!decoded)throw std::runtime_error("无效或截断的 Radiance HDR 像素");budget(width,height,options);
    auto image=makeDeep(width,height);for(int y=0;y<height;++y){checkCancelled(options);for(int x=0;x<width;++x){const auto* p=decoded.get()+(size_t(y)*width+x)*4;image->setPixel(x,y,{float(linearToSrgb(p[0])),float(linearToSrgb(p[1])),float(linearToSrgb(p[2])),1});}}return image;
}
void encodeExr(const std::filesystem::path& path,int width,int height,const HdrRows& rows,const std::function<bool()>& cancelled){
    checkedBytes(uint32_t(width),uint32_t(height),4,{},2);if(!rows)throw std::runtime_error("EXR 行源缺失");check(cancelled);
    Output stream(path,cancelled);
    {Imf::Header h(width,height);h.compression()=Imf::ZIP_COMPRESSION;h.channels().insert("R",Imf::Channel(Imf::FLOAT));h.channels().insert("G",Imf::Channel(Imf::FLOAT));h.channels().insert("B",Imf::Channel(Imf::FLOAT));h.channels().insert("A",Imf::Channel(Imf::FLOAT));Imf::addChromaticities(h,Imf::Chromaticities{});
        Imf::OutputFile file(stream,h,1);std::vector<WidePixel> row(size_t(width),WidePixel{});const char* names[]{"R","G","B","A"};Imf::FrameBuffer frame;
        // yStride zero deliberately reuses the same owned row for each scanline.
        for(int c=0;c<4;++c)frame.insert(names[c],Imf::Slice(Imf::FLOAT,reinterpret_cast<char*>(row.data()->data()+c),sizeof(WidePixel),0));file.setFrameBuffer(frame);
        for(int y=0;y<height;++y){check(cancelled);rows(y,row);for(auto& p:row){if(!std::isfinite(p[3])||p[3]<0||p[3]>1)throw std::runtime_error("无效的 EXR alpha");for(int c=0;c<3;++c){if(!std::isfinite(p[c]))throw std::runtime_error("无效的 EXR 颜色");p[c]=p[3]>0?float(srgbToLinear(p[c]/p[3])*p[3]):0;}}file.writePixels(1);}
    }stream.finish();check(cancelled);
}
void encodeExr(const std::filesystem::path& path,const DeepImage& image,const std::function<bool()>& cancelled){
    if(!image.valid())throw std::runtime_error("无效的 HDR 图像存储");encodeExr(path,image.width,image.height,[&](int y,std::span<WidePixel> row){for(int x=0;x<image.width;++x)row[size_t(x)]=image.pixel(x,y);},cancelled);
}
}
