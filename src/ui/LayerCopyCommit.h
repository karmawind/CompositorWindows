#pragma once
#include "MainWindow.h"
#include <stdexcept>
#include <utility>

namespace compositor::ui {
// Shared by menu copies and the asynchronous project-layer drop worker.
inline bool commitPreparedLayerCopy(EditorProject& project,const std::optional<Document>& before,
    const std::string& beforeActive,const layers::EditResult& result){
    if(project.document!=before||project.active!=beforeActive)
        throw std::runtime_error("复制目标已更改；已准备的图层未应用");
    const bool first=!project.document;
    std::optional<Document> document=result.document;
    auto active=result.selection.primary;
    auto selected=result.selection.ids;
    project.history.begin("从项目复制图层",project.document,project.active);
    try{
        project.document=std::move(document);
        project.active=std::move(active);
        project.history.end(project.document,project.active);
    }catch(...){
        if(auto snapshot=project.history.cancel()){
            project.document=std::move(snapshot->document);
            project.active=std::move(snapshot->activeLayer);
        }
        throw;
    }
    project.selected=std::move(selected);
    project.maskSelected=false;
    return first;
}
}
