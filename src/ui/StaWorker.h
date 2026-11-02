#pragma once
#include <QEventLoop>
#include <QObject>
#include <QThread>
#include <Windows.h>
#include <objbase.h>
#include <exception>
#include <functional>
#include <stdexcept>

namespace compositor::ui {
template<typename Callable>
void runOnStaThread(Callable&& work){
    std::exception_ptr error;
    auto* thread=QThread::create([&work,&error]{
        const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        struct Release{HRESULT value;~Release(){if(SUCCEEDED(value))CoUninitialize();}} release{apartment};
        if(FAILED(apartment)){
            try{throw std::runtime_error("初始化 STA 单元 (HRESULT 0x"+std::to_string(static_cast<uint32_t>(apartment))+")");}
            catch(...){error=std::current_exception();}
            return;
        }
        try{work();}catch(...){error=std::current_exception();}
    });
    if(!thread)throw std::runtime_error("无法创建 STA 工作线程");
    QEventLoop loop;
    QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);
    QObject::connect(thread,&QThread::finished,&loop,&QEventLoop::quit,Qt::QueuedConnection);
    thread->start();
    if(!thread->isFinished())loop.exec(QEventLoop::ExcludeUserInputEvents);
    if(error)std::rethrow_exception(error);
}
}
