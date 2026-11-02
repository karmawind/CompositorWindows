#pragma once
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace compositor::imaging {
// Filename adaptation only. Callers keep their document/lock identities in the
// original normalized path; no filesystem traversal or reparse resolution occurs.
inline std::filesystem::path win32FilePath(const std::filesystem::path& path) {
    auto name=path.native();std::replace(name.begin(),name.end(),L'/',L'\\');
    if(name.empty()||name.find(L'\0')!=std::wstring::npos)throw std::invalid_argument("图像文件名为空或包含 NUL");
    if(name.starts_with(L"\\\\.\\")||name.starts_with(L"\\??\\")||name.starts_with(L"\\\\??\\"))throw std::invalid_argument("图像文件名不能指向设备命名空间");
    auto drive=[](const std::wstring& s){return s.size()>=3&&((s[0]>=L'A'&&s[0]<=L'Z')||(s[0]>=L'a'&&s[0]<=L'z'))&&s[1]==L':'&&s[2]==L'\\';};
    if(name.starts_with(L"\\\\?\\")) {
        const auto tail=name.substr(4);
        if(tail.starts_with(L"UNC\\"))name=L"\\\\"+tail.substr(4);
        else if(drive(tail))name=tail;
        else throw std::invalid_argument("仅驱动器和 UNC 图像文件名支持扩展命名空间");
    }
    if(!drive(name)&&!name.starts_with(L"\\\\")) {
        if(name.size()>1&&name[1]==L':')throw std::invalid_argument("相对于驱动器的图像文件名有歧义");
        name=std::filesystem::absolute(std::filesystem::path(name)).native();std::replace(name.begin(),name.end(),L'/',L'\\');
    }
    auto component=[](const std::wstring& part){
        if(part.empty()||part.size()>255||part.back()==L' '||part.back()==L'.')throw std::invalid_argument("图像文件名组成部分为空、过长或以点/空格结尾");
        for(auto c:part)if(c<32||std::wstring(L"<>:\"|?*").find(c)!=std::wstring::npos)throw std::invalid_argument("图像文件名包含保留字符或备用数据流");
        auto stem=part.substr(0,part.find(L'.'));for(auto& c:stem)if(c>=L'a'&&c<=L'z')c-=L'a'-L'A';
        const bool numbered=stem.size()==4&&(stem.starts_with(L"COM")||stem.starts_with(L"LPT"))&&((stem[3]>=L'1'&&stem[3]<=L'9')||stem[3]==L'\u00b9'||stem[3]==L'\u00b2'||stem[3]==L'\u00b3');
        if(numbered||stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL"||stem==L"CLOCK$"||stem==L"CONIN$"||stem==L"CONOUT$")throw std::invalid_argument("图像文件名使用了保留的 DOS 设备名");
    };
    std::wstring root,tail;bool unc=false;
    if(drive(name)){root=name.substr(0,3);tail=name.substr(3);}
    else if(name.starts_with(L"\\\\")){
        unc=true;const auto serverEnd=name.find(L'\\',2);
        if(serverEnd==std::wstring::npos)throw std::invalid_argument("UNC 图像路径需要服务器和共享名");
        const auto shareEnd=name.find(L'\\',serverEnd+1);const auto server=name.substr(2,serverEnd-2),share=name.substr(serverEnd+1,shareEnd==std::wstring::npos?shareEnd:shareEnd-serverEnd-1);
        component(server);component(share);root=server+L"\\"+share+L"\\";tail=shareEnd==std::wstring::npos?L"":name.substr(shareEnd+1);
    }else throw std::invalid_argument("无法将图像文件名转为绝对路径");
    std::vector<std::wstring> parts;size_t start=0;
    while(start<tail.size()){auto end=tail.find(L'\\',start);auto part=tail.substr(start,end==std::wstring::npos?end:end-start);if(part==L".."){if(parts.empty())throw std::invalid_argument("图像路径离开了其驱动器或 UNC 共享");parts.pop_back();}else if(!part.empty()&&part!=L"."){component(part);parts.push_back(std::move(part));}if(end==std::wstring::npos)break;start=end+1;}
    if(parts.empty())throw std::invalid_argument("图像文件名必须包含文件组成部分");
    std::wstring result=unc?L"\\\\?\\UNC\\"+root:L"\\\\?\\"+root;for(size_t i=0;i<parts.size();++i){if(i)result+=L'\\';result+=parts[i];}
    if(result.size()>=32767)throw std::invalid_argument("图像文件名超出扩展 Win32 长度限制");
    return std::filesystem::path(result);
}
}
