#include "Updater.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <array>

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){try{
    std::array<wchar_t,32768> module{};DWORD length=GetModuleFileNameW(nullptr,module.data(),DWORD(module.size()));if(!length||length>=module.size())throw std::runtime_error("无法找到 Compositor 安装位置");
    int count=0;auto native=CommandLineToArgvW(GetCommandLineW(),&count);if(!native)throw std::runtime_error("无法读取启动器参数");QStringList arguments;for(int i=1;i<count;++i)arguments.push_back(QString::fromWCharArray(native[i]));LocalFree(native);
    return compositor::update::launchCurrent(std::filesystem::path(module.data()).parent_path(),arguments);
}catch(const std::exception& error){auto message=QString::fromUtf8(error.what()).toStdWString();MessageBoxW(nullptr,message.c_str(),L"Compositor",MB_OK|MB_ICONERROR);return 2;}}
