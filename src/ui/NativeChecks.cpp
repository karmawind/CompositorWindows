#include "MainWindow.h"
#include "VisualStyle.h"
#include "persistence/ProjectStore.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QScreen>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QTest>
#include <icm.h>
#include <cmath>

namespace {
QColor monitorExpectedColor(compositor::platform::DisplayProfile& destination,QColor canonical=QColor(35,65,90)){
    // The canonical fixture remains sRGB. Use Windows' CPU color engine as an
    // independent expectation for the displayed opaque pixel, not the D2D path.
    auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    wchar_t system[MAX_PATH]{};require(GetSystemDirectoryW(system,MAX_PATH)!=0,"Cannot locate the sRGB profile");
    auto source=compositor::platform::loadDisplayProfile(std::filesystem::path(system)/L"spool/drivers/color/sRGB Color Space Profile.icm");
    require(source.kind==compositor::platform::DisplayProfileKind::Icc,"System sRGB profile is invalid");
    PROFILE s{PROFILE_MEMBUFFER,source.icc.data(),DWORD(source.icc.size())},d{PROFILE_MEMBUFFER,destination.icc.data(),DWORD(destination.icc.size())};
    struct OpenProfile{HPROFILE value{};~OpenProfile(){if(value)CloseColorProfile(value);}};
    OpenProfile from{OpenColorProfileW(&s,PROFILE_READ,FILE_SHARE_READ,OPEN_EXISTING)},to{OpenColorProfileW(&d,PROFILE_READ,FILE_SHARE_READ,OPEN_EXISTING)};
    require(from.value&&to.value,"WCS cannot open the profile pair");
    HPROFILE profiles[]{from.value,to.value};DWORD intents[]{INTENT_RELATIVE_COLORIMETRIC,INTENT_RELATIVE_COLORIMETRIC};
    struct OpenTransform{HTRANSFORM value{};~OpenTransform(){if(value)DeleteColorTransform(value);}};
    OpenTransform transform{CreateMultiProfileTransform(profiles,2,intents,2,BEST_MODE,INDEX_DONT_CARE)};
    require(transform.value,"WCS cannot create the CPU profile transform");COLOR input{},output{};input.rgb={WORD(canonical.red()*257),WORD(canonical.green()*257),WORD(canonical.blue()*257)};
    require(TranslateColors(transform.value,&input,1,COLOR_RGB,&output,COLOR_RGB),"WCS CPU color translation failed");
    return {int(std::lround(output.rgb.red/257.)),int(std::lround(output.rgb.green/257.)),int(std::lround(output.rgb.blue/257.))};
}
}

namespace compositor {
void MainWindow::exerciseNativeUi(const QString&dir){
    QDir().mkpath(dir);if(!canvas())addFeasibilityDocument();QTest::qWait(150);
    auto require=[](bool ok,const char*error){if(!ok)throw std::runtime_error(error);};
    QJsonArray checks;auto passed=[&](const char*name){checks.append(name);QFile progress(dir+"/native-ui-progress.json");require(progress.open(QIODevice::WriteOnly),"UI progress evidence output failed");progress.write(QJsonDocument(QJsonObject{{"status","running"},{"completed_checks",checks}}).toJson());};
    canvas()->repaint();QTest::qWait(30);
    if(!canvas()->deviceReady())throw std::runtime_error("原生图形设备不可用："+canvas()->deviceError().toStdString());
    auto original=active()->transform;auto middle=canvas()->rect().center();
    QTest::mousePress(canvas(),Qt::LeftButton,Qt::NoModifier,middle);QTest::mouseMove(canvas(),middle+QPoint(23,11));QTest::mouseRelease(canvas(),Qt::LeftButton,Qt::NoModifier,middle+QPoint(23,11));
    require(active()->transform!=original&&current()->history.canUndo(),"原生指针事务失败");undo_->trigger();require(active()->transform==original,"原生撤销失败");redo_->trigger();passed("鼠标移动 / 撤销 / 重做");
    auto viewPoint=[&](Point p){auto at=canvas()->viewMapping().toView(p);return QPointF(at.x,at.y).toPoint();};
    selectTool(Tool::Marquee);auto start=viewPoint({20,20}),end=viewPoint({120,100});
    QTest::mousePress(canvas(),Qt::LeftButton,Qt::NoModifier,start);QTest::mouseMove(canvas(),end);QTest::mouseRelease(canvas(),Qt::LeftButton,Qt::NoModifier,end);
    require(current()->document->selection&&current()->document->selection->coverage->pixel(60,50)==255&&current()->document->selection->coverage->pixel(200,150)==0,"原生选框覆盖失败");
    undo_->trigger();require(!current()->document->selection,"Selection undo failed");redo_->trigger();require(current()->document->selection.has_value(),"Selection redo failed");passed("原生选框与选区历史记录");
    edit("Deselect",[](Document&d){d.selection.reset();});
    selectTool(Tool::Brush);brushSettings_.radius=16;brushSettings_.hardness=.2;brushSettings_.opacity=.5;
    auto source=active()->raster;auto historyCount=current()->history.undoCount();Raster::resetMaterializationCount();
    for(int stroke=0;stroke<2;++stroke){auto a=viewPoint({260.+stroke*30,180}),b=viewPoint({290.+stroke*30,205});QTest::mousePress(canvas(),Qt::LeftButton,Qt::NoModifier,a);for(int i=1;i<=8;++i)QTest::mouseMove(canvas(),a+(b-a)*i/8);QTest::mouseRelease(canvas(),Qt::LeftButton,Qt::NoModifier,b);QTest::qWait(20);}
    require(active()->raster!=source&&current()->history.undoCount()==historyCount+2,"原生画笔提交失败");
    const auto flattenCount=Raster::materializationCount();require(flattenCount==0,"Brush/UI forced full raster materialization");passed("two native brush strokes, one history step each, zero rgba materializations");
    // A tab switch cancels the original tab's live edit before its project changes.
    selectTool(Tool::Move);auto*owner=current();auto beforeMove=*owner->document;pointerBegin(QPointF(200,160),Qt::NoModifier);pointerUpdate(QPointF(240,180),Qt::NoModifier);Document blank;blank.id=newId();blank.width=32;blank.height=32;addProject(blank);
    require(*owner->document==beforeMove,"Tab switch did not cancel original transaction");closeProject(tabs_->currentIndex());passed("tab switch cancels captured transaction");
    const auto beforeAdjustment=*current()->document;
    bool applied=false,timedOut=false;
    auto driveAdjustment=[&](const QString& kind,bool commit){
        QPointer<QDialog> panel;bool finished=false,submitted=false;QEventLoop wait;QMetaObject::Connection completion;
        QElapsedTimer elapsed;elapsed.start();QTimer drive;drive.setInterval(40);
        connect(&drive,&QTimer::timeout,this,[&]{
            if(elapsed.elapsed()>30000){timedOut=true;if(panel)panel->reject();wait.quit();return;}
            if(!panel){
                for(auto* object:findChildren<QObject*>())if(auto* session=dynamic_cast<ui::EditPanelSession*>(object))
                    if(session->canvas()==canvas()&&session->panel()&&session->panel()->isVisible()){panel=session->panel();break;}
                if(panel)completion=connect(panel,&QDialog::finished,&wait,[&]{finished=true;wait.quit();});
            }
            auto* d=panel.data();if(!d)return;
            if(!commit){if(elapsed.elapsed()>=180){auto name=kind;name.replace('/', '-');d->grab().save(dir+"/panel-"+name+".png");d->reject();}return;}
            for(auto*spin:d->findChildren<QDoubleSpinBox*>())if(spin->accessibleName()=="曝光"&&!applied){spin->setValue(1);applied=true;}
            for(auto*box:d->findChildren<QDialogButtonBox*>())if(auto*button=box->button(QDialogButtonBox::Apply);button&&button->isEnabled()&&applied&&!submitted){submitted=true;button->click();}
        });
        adjust(kind,false);drive.start();if(!finished)wait.exec();drive.stop();disconnect(completion);
        if(!finished)throw std::runtime_error("Owned adjustment panel did not finish within the30-second limit");
    };
    driveAdjustment("曝光",true);require(!timedOut&&applied&&*current()->document!=beforeAdjustment,"原生调整应用失败");undo_->trigger();require(*current()->document==beforeAdjustment,"Adjustment undo failed");passed("Exposure dialog Apply / undo");
    for(const QString kind:{"Hue/Saturation","色阶","曲线","曝光","渐变映射","颗粒"}){auto before=*current()->document;driveAdjustment(kind,false);require(*current()->document==before,"Adjustment Cancel changed document");}passed("six adjustment dialogs Cancel preserve document");
    auto gesture=[&](Tool tool,Point a,Point b){selectTool(tool);QTest::qWait(30);auto start=viewPoint(a),end=viewPoint(b);QTest::mousePress(canvas(),Qt::LeftButton,Qt::NoModifier,start);QTest::mouseMove(canvas(),end);QTest::mouseRelease(canvas(),Qt::LeftButton,Qt::NoModifier,end);};
    {auto before=*current()->document;auto count=current()->history.undoCount();gesture(Tool::Shape,{40,40},{130,110});require(current()->document->layers.size()==before.layers.size()+1&&current()->history.undoCount()==count+1&&!active()->shapeJson.empty(),"原生形状事务失败");undo_->trigger();require(*current()->document==before,"Shape undo failed");passed("shape mouse gesture / live metadata / one undo");}
    {auto before=*current()->document;const auto history=current()->history.undoCount();gesture(Tool::Gradient,{220,160},{330,240});require(*current()->document==before&&current()->gradientPreview&&current()->history.undoCount()==history,"原生待处理渐变失败");findChild<QAction*>("applyGradient")->trigger();require(current()->history.undoCount()==history+1,"原生渐变应用失败");undo_->trigger();require(*current()->document==before,"Gradient undo failed");passed("gradient mouse gesture / exact undo");}
    {auto before=*current()->document;current()->maskSelected=true;maskPaintWhite_=false;gesture(Tool::Brush,{280,190},{310,210});require(active()->mask!=before.layers.back().mask&&active()->raster==before.layers.back().raster,"原生蒙版画笔失败");undo_->trigger();require(*current()->document==before,"蒙版画笔撤销失败");current()->maskSelected=false;passed("蒙版画笔保留图像 / 精确撤销");}
    {auto before=*current()->document;gesture(Tool::Crop,{30,25},{200,180});require(cropDraft_.has_value()&&*current()->document==before,"Crop preview mutated document");applyCrop();require(current()->document->width<before.width&&current()->document->height<before.height,"原生裁剪应用失败");undo_->trigger();require(*current()->document==before,"Crop undo failed");canvas()->fit();passed("crop preview / Apply / exact undo");}
    {auto before=*current()->document;selectTool(Tool::Wand);wandAllLayers_=true;wandTolerance_=0;runWand({20,20},Qt::NoModifier);require(current()->document->selection&&current()->document->selection->coverage->pixel(20,20)>0,"原生魔棒结果失败");undo_->trigger();require(*current()->document==before,"Wand undo failed");passed("asynchronous wand / exact undo");}
    {auto paletteBefore=foreground_;openPalette();require(colorPicker_,"Floating picker failed to open");pointerBegin({100,350},Qt::NoModifier);pointerEnd({100,350},Qt::NoModifier);auto sample=colorPicker_->color();require(sample==effects_tools::PaletteColor{35/255.,65/255.,90/255.}&&foreground_==paletteBefore,"Picker sampling changed palette before Apply");colorPicker_->grab().save(dir+"/panel-Palette.png");colorPicker_->reject();require(foreground_==paletteBefore,"Palette Cancel changed foreground");openPalette();pointerBegin({100,350},Qt::NoModifier);pointerEnd({100,350},Qt::NoModifier);colorPicker_->accept();require(foreground_.red()==35&&foreground_.green()==65&&foreground_.blue()==90,"Palette Apply failed");foreground_=paletteBefore;passed("floating palette original-composite sampling / Cancel / Apply");}
    {auto zoom=canvas()->zoom;selectTool(Tool::Zoom);auto point=canvas()->rect().center();auto documentBefore=canvas()->documentPoint(point);QTest::mouseClick(canvas(),Qt::LeftButton,{},point);require(std::abs(canvas()->zoom-zoom*2)<1e-9&&QLineF(canvas()->documentPoint(point),documentBefore).length()<1e-6,"Anchored zoom click failed");QTest::mouseClick(canvas(),Qt::LeftButton,Qt::AltModifier,point);require(std::abs(canvas()->zoom-zoom)<1e-9,"点击缩小失败");passed("anchored zoom tool / Alt zoom out");}
    {Document large;large.id=newId();large.width=large.height=30000;Layer emptyLayer;emptyLayer.id=newId();emptyLayer.name="图层 1";emptyLayer.transform={0,0,30000,30000};large.layers.push_back(emptyLayer);QElapsedTimer clock;clock.start();Raster::resetMaterializationCount();addProject(large);canvas()->repaint();QTest::qWait(30);require(canvas()->deviceReady()&&canvas()->deviceError().isEmpty(),"大型空白画布呈现失败");auto frame=canvas()->captureRendered();require(!frame.isNull(),"大型空白画布回读失败");QFile timing(dir+"/large-canvas.json");if(timing.open(QIODevice::WriteOnly))timing.write(QJsonDocument(QJsonObject{{"width",30000},{"height",30000},{"create_and_present_ms",double(clock.elapsed())},{"display_tile_budget",64},{"rgba_materializations",double(Raster::materializationCount())}}).toJson());require(Raster::materializationCount()==0,"Blank canvas presentation flattened pixels");closeProject(tabs_->currentIndex());passed("30000-square blank canvas native presentation with bounded viewport tiles");}
    {
        // A real mouse stroke publishes a renderer-only source before commit.
        // Capture the D3D back buffer while the canonical Layer is still intact.
        Document document;document.id=newId();document.width=64;document.height=32;
        Layer layer;layer.id=newId();layer.name="原生润饰范围见证";layer.transform={8,8,16,16};layer.raster=Raster::filled(16,16,{70,180,120,255});document.layers={layer};
        auto& project=addProject(document);selectTool(Tool::CloneStamp);cloneSettings_.radius=4;cloneSettings_.hardness=1;cloneSettings_.opacity=1;project.cloneSampleAllLayers=false;refreshRetouchControls();canvas()->fit();QTest::qWait(60);
        const auto sourcePoint=viewPoint({16,16}),destination=viewPoint({40,16}),endpoint=viewPoint({44,16});
        QTest::mouseClick(canvas(),Qt::LeftButton,Qt::AltModifier,sourcePoint);
        require(project.cloneAlignment.source().has_value()&&project.history.undoCount()==0,"原生仿制源取样更改了历史记录");
        const auto canonical=project.document;const auto originalRaster=active()->raster;
        auto probe=[&](const QImage& image){auto point=viewPoint({40.5,16.5})*canvas()->devicePixelRatioF();require(image.rect().contains(point),"原生润饰取样在视口外");return image.pixelColor(point);};
        auto beforeFrame=canvas()->captureRendered();require(!beforeFrame.isNull()&&beforeFrame.save(dir+"/retouch-before.png"),"原生润饰基线捕获失败");const auto beforeColor=probe(beforeFrame);
        QTest::mousePress(canvas(),Qt::LeftButton,Qt::NoModifier,destination);QTest::qWait(40);
        require(retouch_&&project.retouchPreview&&project.document==canonical&&active()->raster==originalRaster&&project.history.undoCount()==0,"原生润饰实时预览更改了规范状态");
        auto liveFrame=canvas()->captureRendered();require(!liveFrame.isNull()&&liveFrame.save(dir+"/retouch-live.png"),"原生润饰实时 D3D 捕获失败");
        require(retouch_&&project.retouchPreview&&project.document==canonical,"原生呈现已提交润饰描边");
        auto profile=platform::discoverDisplayProfile(reinterpret_cast<void*>(canvas()->winId()));
        require(canvas()->presentationProfileHash()==QString::fromStdString(profile.sha256),"润饰回读期间监视器配置文件已更改");
        const auto expected=canvas()->presentationConvertsColor()?monitorExpectedColor(profile,QColor(70,180,120)):QColor(70,180,120);
        require(canvas()->presentationConvertsColor()||!canvas()->presentationDiagnostic().isEmpty(),"Retouch sRGB fallback lacks diagnostic");
        const auto liveColor=probe(liveFrame);auto matches=[&](QColor actual,QColor wanted){return std::abs(actual.red()-wanted.red())<=1&&std::abs(actual.green()-wanted.green())<=1&&std::abs(actual.blue()-wanted.blue())<=1;};
        require(matches(liveColor,expected),"Presented live Clone pixels differ from profile-qualified source color");
        require(!matches(beforeColor,expected),"原生润饰基线缺少明显不同的外部前提条件");
        QTest::qWait(40);auto liveWindow=screen()->grabWindow(0,mapToGlobal(QPoint(0,0)).x(),mapToGlobal(QPoint(0,0)).y(),width(),height());require(!liveWindow.isNull()&&liveWindow.save(dir+"/retouch-window-live.png"),"实时润饰窗口截图失败");
        QTest::mouseMove(canvas(),endpoint);require(project.document==canonical&&project.retouchPreview,"原生仿制移动更改了规范图层");
        QTest::mouseRelease(canvas(),Qt::LeftButton,Qt::NoModifier,endpoint);QTest::mouseMove(canvas(),sourcePoint);QTest::qWait(30);
        require(!retouch_&&!project.retouchPreview&&project.history.undoCount()==1&&project.history.undoName()=="仿制图章","原生仿制未提交单个历史记录事务");
        require(active()->transform.x==8&&active()->transform.y==8&&active()->transform.width>16&&active()->raster!=originalRaster,"原生仿制提交丢失了扩展后的放置位置");
        require(SoftwareRenderer().render(*project.document,0,0,64,32)->pixel(40,16)==Pixel{70,180,120,255}&&originalRaster->pixel(8,8)==Pixel{70,180,120,255},"原生仿制提交的像素或不可变输入不一致");
        const auto committed=project.document;auto commitFrame=canvas()->captureRendered();require(!commitFrame.isNull()&&commitFrame.save(dir+"/retouch-committed.png"),"原生仿制提交捕获失败");const auto committedColor=probe(commitFrame);require(matches(committedColor,expected),"原生仿制提交呈现丢失实时像素");
        undo_->trigger();require(project.document==canonical&&!project.retouchPreview,"原生扩展仿制撤销丢失了原始图层元数据");auto undoFrame=canvas()->captureRendered();require(!undoFrame.isNull()&&undoFrame.save(dir+"/retouch-undone.png"),"原生仿制撤销捕获失败");const auto undoneColor=probe(undoFrame);require(matches(undoneColor,beforeColor),"原生仿制撤销保留了外部预览像素");
        redo_->trigger();require(project.document==committed&&!project.retouchPreview,"原生扩展仿制重做与提交的图层不一致");undo_->trigger();require(project.document==canonical,"原生仿制清理撤销结果不一致");
        auto rgb=[](QColor c){return QJsonArray{c.red(),c.green(),c.blue()};};QFile evidence(dir+"/retouch-pixel-oracle.json");require(evidence.open(QIODevice::WriteOnly),"Retouch pixel evidence output failed");
        evidence.write(QJsonDocument(QJsonObject{{"canonical_rgb",QJsonArray{70,180,120}},{"expected_display_rgb",rgb(expected)},{"before_rgb",rgb(beforeColor)},{"live_rgb",rgb(liveColor)},{"committed_rgb",rgb(committedColor)},{"undone_rgb",rgb(undoneColor)},{"tolerance",1},{"canvas",QJsonArray{64,32}},{"original_layer_bounds",QJsonArray{8,8,16,16}},{"sample_source",QJsonArray{16,16}},{"stroke_begin",QJsonArray{40,16}},{"stroke_end",QJsonArray{44,16}},{"live_canonical_unchanged",true},{"history_transactions",1},{"undo_redo_exact",true},{"profile_sha256",canvas()->presentationProfileHash()},{"converted",canvas()->presentationConvertsColor()},{"oracle",canvas()->presentationConvertsColor()?"Windows WCS CPU profile transform":"explicit sRGB fallback"}}).toJson());
        project.history.markSaved();closeProject(tabs_->currentIndex());passed("原生仿制范围增长 / 不可变实时 D3D 呈现 / 提交 / 撤销 / 重做");
    }
    selectTool(Tool::Move);
    auto rendered=SoftwareRenderer().render(*current()->document,0,0,current()->document->width,current()->document->height)->rgba();
    current()->path=dir+"/Project 実証 test.comp";require(saveProject()&&waitForProjectSave(*current()),"原生保存失败");auto path=current()->path;
    ProjectStore store(makeWicProjectCodec());const auto persisted=store.load(std::filesystem::path(path.toStdWString()));
    auto reopened=SoftwareRenderer().render(persisted.document,0,0,persisted.document.width,persisted.document.height)->rgba();
    require(rendered==reopened&&persisted.activeLayer==current()->active&&!current()->history.modified(),"Persisted project changed rendered pixels, active layer or dirty state");
    auto* savedProject=current();const auto tabCount=tabs_->count();const auto savedDocument=current()->document;openPath(path);
    require(current()==savedProject&&tabs_->count()==tabCount&&current()->document==savedDocument&&!current()->history.modified(),"Opening an existing project did not preserve its tab and state");
    passed("原生保存/加载 Unicode 目录包、精确合成往返与现有标签页复用");
    QAbstractButton* maximize=nullptr;for(auto* button:findChildren<QAbstractButton*>())if(button->accessibleName()=="最大化或还原窗口")maximize=button;
    if(ui::macTitleBarEnabled()){
        require(maximize&&windowFlags().testFlag(Qt::FramelessWindowHint),"Custom window controls missing");maximize->click();QTest::qWait(50);require(isMaximized(),"Custom maximize failed");maximize->click();QTest::qWait(50);require(!isMaximized(),"Custom restore failed");
    }else{
        const auto hwnd=reinterpret_cast<HWND>(winId());const auto style=GetWindowLongPtrW(hwnd,GWL_STYLE);
        require((style&(WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_THICKFRAME))==(WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_THICKFRAME),"原生标题栏或窗口控件缺失");
        SendMessageW(hwnd,WM_SYSCOMMAND,SC_MAXIMIZE,0);QTest::qWait(50);require(isMaximized(),"原生最大化失败");SendMessageW(hwnd,WM_SYSCOMMAND,SC_RESTORE,0);QTest::qWait(50);require(!isMaximized(),"原生还原失败");
    }
    passed("selected title bar maximize and restore");
    canvas()->recreateDevice();resize(1100,740);QTest::qWait(100);require(canvas()->deviceReady(),"Device recreation failed");canvas()->repaint();QTest::qWait(120);
    auto gpu=canvas()->captureRendered();require(!gpu.isNull()&&gpu.save(dir+"/canvas-readback.png"),"GPU capture failed");
    auto sample=viewPoint({100,350})*devicePixelRatioF();require(gpu.rect().contains(sample),"GPU sample is outside the resized viewport");auto color=gpu.pixelColor(sample);
    auto profile=platform::discoverDisplayProfile(reinterpret_cast<void*>(canvas()->winId()));
    require(canvas()->presentationProfileHash()==QString::fromStdString(profile.sha256),"原生像素检查期间监视器配置文件已更改");
    const auto expected=canvas()->presentationConvertsColor()?monitorExpectedColor(profile):QColor(35,65,90);
    require(canvas()->presentationConvertsColor()||!canvas()->presentationDiagnostic().isEmpty(),"原生 sRGB 回退缺少诊断信息");
    QFile pixelEvidence(dir+"/pixel-oracle.json");require(pixelEvidence.open(QIODevice::WriteOnly),"Pixel oracle evidence output failed");
    pixelEvidence.write(QJsonDocument(QJsonObject{{"canonical_rgb",QJsonArray{35,65,90}},{"expected_display_rgb",QJsonArray{expected.red(),expected.green(),expected.blue()}},{"actual_display_rgb",QJsonArray{color.red(),color.green(),color.blue()}},{"tolerance",1},{"profile_sha256",canvas()->presentationProfileHash()},{"converted",canvas()->presentationConvertsColor()},{"oracle",canvas()->presentationConvertsColor()?"Windows WCS CPU profile transform":"explicit sRGB fallback"}}).toJson());
    require(std::abs(color.red()-expected.red())<=1&&std::abs(color.green()-expected.green())<=1&&std::abs(color.blue()-expected.blue())<=1,"GPU background pixels differ from the profile-qualified canonical composite");passed("WARP readback pixel invariant, resize and device recreation");
    auto screenshot=screen()->grabWindow(0,mapToGlobal(QPoint(0,0)).x(),mapToGlobal(QPoint(0,0)).y(),width(),height());require(!screenshot.isNull()&&screenshot.save(dir+"/native-window.png"),"原生截图失败");
    auto widgetLayout=grab();
    {QPainter painter(&widgetLayout);painter.drawImage(QRect(canvas()->mapTo(this,QPoint{}),canvas()->size()),gpu);}
    require(widgetLayout.save(dir+"/widget-layout.png"),"Widget layout capture failed");
    addEmptyProject(false);QTest::qWait(30);require(grab().save(dir+"/welcome.png"),"Welcome capture failed");
    const auto demo=qEnvironmentVariable("COMPOSITOR_UI_DEMO");
    if(!demo.isEmpty()){
        openPath(demo);require(current()&&current()->document,"Demo capture project failed to open");resize(1440,900);canvas()->fit();QTest::qWait(150);
        auto demoFrame=canvas()->captureRendered();require(!demoFrame.isNull(),"Demo D3D readback failed");auto demoLayout=grab();
        {QPainter painter(&demoLayout);painter.drawImage(QRect(canvas()->mapTo(this,QPoint{}),canvas()->size()),demoFrame);}
        require(demoLayout.save(dir+"/demo-workspace.png"),"Demo workspace capture failed");
    }
    QFile report(dir+"/native-ui.json");require(report.open(QIODevice::WriteOnly),"UI evidence output failed");report.write(QJsonDocument(QJsonObject{{"status","passed"},{"checks",checks},{"devicePixelRatio",devicePixelRatioF()},{"brush_full_raster_materializations",double(flattenCount)},{"human_acceptance",false},{"timestamp",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson());for(auto&p:projects_)p->history.markSaved();
}
}
