#include "AdjustmentDialog.h"
#include "DocumentPreview.h"
#include "LevelsPreviewHistogram.h"
#include "AdjustmentAdvancedControls.h"
#include "PropertyControls.h"
#include <QEventLoop>
#include <map>
#include "editing/Selection.h"
#include "layers/LayerOperations.h"
#include <QMouseEvent>
#include "effects/Adjustments.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QColorDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QPainter>
#include <algorithm>
#include <atomic>
#include <cmath>

static QString normalizeKind(const QString& k){struct MapEntry{const char* en;const char* zh;};static constexpr MapEntry map[]={{"Levels","色阶"},{"Curves","曲线"},{"Exposure","曝光"},{"Gradient Map","渐变映射"},{"Grain","颗粒"},{"Invert","反相"},{"Black & White","黑白"},{"Color Balance","色彩平衡"},{"Gaussian Blur","高斯模糊"},{"Motion Blur","动感模糊"},{"Add Noise","添加杂色"}};for(const auto& e:map)if(k==QLatin1String(e.en))return QLatin1String(e.zh);return k;}

namespace compositor {
namespace {
using Object=QJsonObject;
std::string encoded(const Object&o){return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();}
class AdjustmentPreview final:public QLabel {
public:
    int documentWidth{},documentHeight{};
    std::function<void(Point)> onClick;
    std::function<bool(Point)> onBeginDrag;
    std::function<void(double,bool)> onDrag;
    std::function<void()> onEndDrag;
    AdjustmentPreview(){setObjectName("adjustmentPreview");setAccessibleName("调整预览；单击可取样，拖动可进行目标色相调整");}
private:
    bool dragging_{};double anchor_{};
    QRectF imageRect()const{
        auto image=pixmap();if(image.isNull())return {};auto size=image.deviceIndependentSize();double scale=std::min(width()/size.width(),height()/size.height());size*=scale;return {(width()-size.width())/2.,(height()-size.height())/2.,size.width(),size.height()};
    }
    void paintEvent(QPaintEvent*)override{QPainter painter(this);painter.fillRect(rect(),palette().color(QPalette::Window));auto image=pixmap();if(!image.isNull()){painter.setRenderHint(QPainter::SmoothPixmapTransform);painter.drawPixmap(imageRect(),image,QRectF(image.rect()));}}
    std::optional<Point> documentPoint(QPointF position)const{
        auto box=imageRect();if(box.isEmpty()||!box.contains(position))return {};
        return Point{(position.x()-box.left())*documentWidth/box.width(),(position.y()-box.top())*documentHeight/box.height()};
    }
    void mousePressEvent(QMouseEvent* event)override{
        if(event->button()!=Qt::LeftButton)return;auto point=documentPoint(event->position());if(!point)return;
        if(onBeginDrag&&onBeginDrag(*point)){dragging_=true;anchor_=event->position().x();return;}if(onClick)onClick(*point);
    }
    void mouseMoveEvent(QMouseEvent* event)override{if(dragging_&&onDrag)onDrag(event->position().x()-anchor_,event->modifiers().testFlag(Qt::ControlModifier));}
    void mouseReleaseEvent(QMouseEvent* event)override{if(event->button()!=Qt::LeftButton||!dragging_)return;dragging_=false;if(onEndDrag)onEndDrag();}
};
// AdjustmentEditing.swift retains hierarchy and live-mask dependencies. Hidden
// records still supply masks; erasing a flat suffix changes or invalidates input.
Document levelsInput(Document document,const std::string& active,bool existing,const std::string& insertedId){
    std::string cutoff=active;
    if(!existing){auto found=std::find_if(document.layers.begin(),document.layers.end(),[&](const Layer& layer){return layer.id==active;});Layer marker;marker.id=insertedId;marker.adjustmentJson=effects::defaultAdjustmentJson("色阶");marker.transform={0,0,double(document.width),double(document.height)};if(found!=document.layers.end())marker.parentId=found->group?found->id:found->parentId;document.layers.insert(found==document.layers.end()?document.layers.end():found+1,marker);cutoff=marker.id;}
    std::unordered_set<std::string> underneath;for(const auto& entry:layers::entries(document)){if(entry.id==cutoff)break;underneath.insert(entry.id);}
    for(auto& layer:document.layers)if(!layer.group&&!underneath.contains(layer.id))layer.visible=false;
    document.selection.reset();return document;
}
struct HistogramResult {std::shared_ptr<const Raster> sample;std::shared_ptr<const Document> composite;Transform transform;effects_tools::LevelsHistogram bins{};QString error;};
struct PreviewResult { std::optional<AdjustmentDialogResult> value; QImage image; QString error; bool full{}; };
// Levels.swift135 and HueSaturation.swift384 use8000; the shared FilterEdit
// preview uses2048 except full-resolution grain. Drawing samples nearest here,
// matching BrushRaster.draw(.none); full Apply always reads the original.
std::shared_ptr<const Raster> adjustmentPreviewSource(const std::shared_ptr<const Raster>& source,const std::string& json,const std::function<bool()>& cancelled){
    auto check=[&]{if(cancelled&&cancelled())throw std::runtime_error("调整已取消");};check();
    const auto kind=QJsonDocument::fromJson(QByteArray::fromStdString(json)).object()["kind"].toString();
    if(kind=="颗粒")return source;
    const int limit=kind=="色阶"||kind=="Hue/Saturation"?8000:2048;
    const double factor=std::min(1.,double(limit)/std::max(source->width,source->height));if(factor==1)return source;
    const int width=std::max(1,int(source->width*factor)),height=std::max(1,int(source->height*factor));
    std::vector<Pixel> pixels(size_t(width)*height);
    for(int y=0;y<height;++y){check();for(int x=0;x<width;++x)
        pixels[size_t(y)*width+x]=source->pixel(std::min(source->width-1,int((x+.5)*source->width/width)),std::min(source->height-1,int((y+.5)*source->height/height)));}
    check();
    return Raster::fromRgba(width,height,reinterpret_cast<const uint8_t*>(pixels.data()),size_t(width)*4);
}
PreviewResult makePreview(Document doc,std::string active,const std::string&json,bool live,bool existing,const std::string&newId,bool showPreview,bool full,const std::function<bool()>& cancelled){
    auto check=[&]{if(cancelled&&cancelled())throw std::runtime_error("调整已取消");};
    PreviewResult result;result.full=full;const Document before=doc;
    try{
        check();auto found=std::find_if(doc.layers.begin(),doc.layers.end(),[&](const Layer&l){return l.id==active;});
        if(live){
            if(existing){if(found==doc.layers.end()||found->adjustmentJson.empty())throw std::runtime_error("调整图层已不存在");found->adjustmentJson=json;}
            else {Layer layer;layer.id=newId;layer.name=QJsonDocument::fromJson(QByteArray::fromStdString(json)).object()["kind"].toString().toStdString();layer.adjustmentJson=json;layer.transform={0,0,double(doc.width),double(doc.height)};
                if(found!=doc.layers.end())layer.parentId=found->group?found->id:found->parentId;
                doc.layers.insert(found==doc.layers.end()?doc.layers.end():found+1,layer);active=layer.id;}
        }else{
            if(found==doc.layers.end()||!found->raster)throw std::runtime_error("请选择一个图像图层");
            if(found->deep&&found->deep->isHdr()){
                // Preview and Apply both start from the retained pixels. The
                // reduced draft uses the same preview-size limit as SDR edits.
                if(!full){auto reduced=adjustmentPreviewSource(found->raster,json,cancelled);if(reduced->width!=found->deep->width||reduced->height!=found->deep->height){auto image=makeDeep(reduced->width,reduced->height);for(int y=0;y<image->height;++y){check();for(int x=0;x<image->width;++x)image->setPixel(x,y,found->deep->pixel(std::min(found->deep->width-1,int((x+.5)*found->deep->width/image->width)),std::min(found->deep->height-1,int((y+.5)*found->deep->height/image->height))));}found->deep=image;found->raster=reduced;}}
                auto selection=editing::mappedCoverage(doc,found->transform,found->deep->width,found->deep->height);
                auto changed=effects::applyAdjustment(found->deep,json,selection.get(),{},cancelled);
                if(changed!=found->deep){found->deep=changed;found->raster=rasterFromDeep(*changed);found->shapeJson.clear();found->text.reset();}
            }else{
            if(!full)found->raster=adjustmentPreviewSource(found->raster,json,cancelled);
            auto selection=editing::mappedCoverage(doc,found->transform,found->raster->width,found->raster->height);
            auto changed=effects::applyAdjustment(found->raster,json,selection.get(),{},cancelled);if(changed!=found->raster){found->raster=changed;found->deep.reset();found->shapeJson.clear();found->text.reset();}
            }
        }
        check();validateDocument(doc);
        result.image=fittedDocumentPreview(showPreview?doc:before,{600,460});
        check();result.value=AdjustmentDialogResult{std::move(doc),std::move(active)};
    }catch(const std::exception&e){result.error=e.what();}
    return result;
}
}

namespace {
class AdjustmentWindow final:public QDialog {
public:
    using QDialog::QDialog;std::function<bool()> mayReject;
    void reject()override{if(!mayReject||mayReject())QDialog::reject();}
};
class AdjustmentPanel final:public ui::EditPanelSession {
    Document original;
    std::string active;
    QString kind;
    bool live{},existing{},modal{},committing_{},closed_{},targetDragging_{},loadingSettings_{};
    double targetAnchor_{};
    AdjustmentDialogOptions options;
    Object settings;
    AdjustmentWindow dialog;
    QHBoxLayout layout;
    QWidget fields;
    QFormLayout form;
    QWidget right;
    QVBoxLayout rightLayout;
    AdjustmentPreview preview;
    QLabel status;
    QCheckBox previewEnabled;
    QDialogButtonBox buttons;
    QPushButton* apply{};
    QTimer debounce;
    QFutureWatcher<PreviewResult> watcher;
    QFutureWatcher<HistogramResult> histogramWatcher;
    std::optional<AdjustmentDialogResult> completed;
    uint64_t revision{},runningRevision{};
    bool pending{},closing{};
    std::shared_ptr<std::atomic_bool> previewCancelled=std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> histogramCancelled=std::make_shared<std::atomic_bool>(false);
    std::string insertedId=newId();
    std::function<void()> changed,start;
    std::vector<std::function<void()>> reloadControls;
    LevelsAdvancedControls* levelsControls{};
    QString balanceTone{"midtones"};
    HueAdvancedControls* hueControls{};
    static std::map<Kind,QPoint>& positions(){static std::map<Kind,QPoint> value;return value;}
    void retire(){if(closed_&&!watcher.isRunning()&&!histogramWatcher.isRunning())deleteLater();}
    void finish(int answer){
        if(closed_)return;closed_=true;closing=true;previewCancelled->store(true);histogramCancelled->store(true);debounce.stop();
        if(host_.closeColor)host_.closeColor(answer==QDialog::Accepted);
        if(!modal)positions()[kind_]=dialog.pos();
        try{if(answer==QDialog::Accepted&&completed&&(!host_.valid||host_.valid())&&host_.commit)host_.commit({std::move(completed->document),std::move(completed->active),false});}
        catch(const std::exception& error){if(host_.error)host_.error(QString::fromUtf8(error.what()));}
        if(host_.preview)host_.preview({});if(host_.closed)host_.closed();host_={};retire();
    }
    void initialize(const QString& requestedKind){
    if(existing){auto it=std::find_if(original.layers.begin(),original.layers.end(),[&](const Layer&l){return l.id==active;});if(it==original.layers.end())throw std::runtime_error("调整图层已不存在");settings=QJsonDocument::fromJson(QByteArray::fromStdString(it->adjustmentJson)).object();}
    else settings=QJsonDocument::fromJson(QByteArray::fromStdString(!live&&!options.initialAdjustmentJson.empty()?options.initialAdjustmentJson:effects::defaultAdjustmentJson(requestedKind.toStdString()))).object();
    kind=normalizeKind(settings["kind"].toString());kind_=kind=="色阶"?Kind::Levels:kind=="Hue/Saturation"?Kind::Hue:Kind::Filter;
    if(kind=="曝光"&&!settings.contains("exposureSettings"))settings["exposureSettings"]=Object{{"exposure",0},{"offset",0},{"gamma",1}};
    if(kind=="渐变映射"&&!settings.contains("gradientMapSettings"))settings["gradientMapSettings"]=Object{{"shadows",Object{{"red",0},{"green",0},{"blue",0}}},{"highlights",Object{{"red",1},{"green",1},{"blue",1}}},{"reversed",false}};
    if(kind=="颗粒"&&!settings.contains("grainSettings"))settings["grainSettings"]=Object{{"amount",25},{"size",1.5},{"roughness",50},{"seed",0}};
    if(kind=="色彩平衡"&&!settings.contains("colorBalanceSettings")){const Object zero{{"cyanRed",0},{"magentaGreen",0},{"yellowBlue",0}};settings["colorBalanceSettings"]=Object{{"shadows",zero},{"midtones",zero},{"highlights",zero}};}
    if(kind=="黑白"&&!settings.contains("blackWhiteSettings"))settings["blackWhiteSettings"]=Object{{"reds",40},{"yellows",60},{"greens",40},{"cyans",60},{"blues",20},{"magentas",80}};
    dialog.mayReject=[this]{return !committing_;};dialog.setObjectName("adjustmentDialog");dialog.setWindowTitle(kind);dialog.resize(840,560);
    layout.addWidget(&fields);
    preview.documentWidth=original.width;preview.documentHeight=original.height;preview.setMinimumSize(400,300);preview.setAlignment(Qt::AlignCenter);rightLayout.addWidget(&preview,1);status.setWordWrap(true);rightLayout.addWidget(&status);
    previewEnabled.setObjectName("adjustmentPreviewEnabled");previewEnabled.setChecked(true);rightLayout.addWidget(&previewEnabled);
    rightLayout.addWidget(&buttons);layout.addWidget(&right,1);
    apply=buttons.button(QDialogButtonBox::Apply);apply->setEnabled(false);apply->setDefault(true);buttons.button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    debounce.setSingleShot(true);debounce.setInterval(100);
    changed=[this]{++revision;if(watcher.isRunning())previewCancelled->store(true);apply->setEnabled(false);if(!previewEnabled.isChecked()&&host_.preview)host_.preview({});status.setText("正在更新预览…");debounce.start();};
    QObject::connect(&previewEnabled,&QCheckBox::toggled,&dialog,changed);
    auto number=[&](const QString&label,double value,double lo,double hi,int decimals,double fallback,std::function<void(double)>setter){
        const int stepDecimals=kind=="颗粒"?(label=="大小"?1:0):kind=="Hue/Saturation"?0:kind=="色阶"&&label!="伽马"?0:decimals;
        auto*spin=new ui::PropertyNumber;spin->setSingleStep(std::pow(10.,-stepDecimals));spin->setRange(lo,hi);spin->setDecimals(decimals);spin->setValue(value);spin->setAccessibleName(label);
        if(kind=="曝光"||kind=="颗粒"||kind=="Hue/Saturation"||kind=="黑白"){
            auto* row=new QWidget;auto* rowLayout=new QHBoxLayout(row);rowLayout->setContentsMargins(0,0,0,0);auto* slider=new ui::TrackSlider(Qt::Horizontal);slider->setRange(0,10000);slider->setAccessibleName(label+"滑块");
            const bool logarithmic=label=="伽马"||label=="大小";const double first=logarithmic?std::log(lo):lo,last=logarithmic?std::log(hi):hi;
            auto position=[first,last,logarithmic](double v){return int(std::lround(((logarithmic?std::log(v):v)-first)/(last-first)*10000));};
            slider->setValue(position(spin->value()));slider->setDefaultValue(position(fallback));slider->setMinimumWidth(170);spin->setFixedWidth(82);rowLayout->addWidget(slider,1);rowLayout->addWidget(spin);form.addRow(label,row);
            if(kind=="Hue/Saturation"){
                const auto stops=label=="色相"?"stop:0 #db6868, stop:0.17 #d5c46b, stop:0.33 #7bb878, stop:0.5 #6bbcc3, stop:0.67 #738cda, stop:0.83 #c879c5, stop:1 #db6868":label=="饱和度"?"stop:0 #777b84, stop:1 #629ee9":"stop:0 #17181c, stop:1 #edf0f4";
                slider->setStyleSheet(QString("QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: qlineargradient(x1:0,y1:0,x2:1,y2:0,%1); } QSlider::sub-page:horizontal { background: transparent; }").arg(stops));
            }
            QObject::connect(slider,&QSlider::valueChanged,&dialog,[spin,first,last,logarithmic,stepDecimals](int v){const double normalized=first+(last-first)*v/10000.;const double factor=std::pow(10.,stepDecimals);spin->setValue(std::round((logarithmic?std::exp(normalized):normalized)*factor)/factor);});
            QObject::connect(spin,&QDoubleSpinBox::valueChanged,&dialog,[slider,position](double v){QSignalBlocker block(slider);slider->setValue(position(v));});
        }else form.addRow(label,spin);
        QObject::connect(spin,&QDoubleSpinBox::valueChanged,&dialog,[&,setter](double v){if(loadingSettings_)return;setter(v);changed();});return spin;};
    auto flag=[&](const QString&label,bool value,std::function<void(bool)>setter){auto*box=new QCheckBox(label);box->setChecked(value);form.addRow(box);QObject::connect(box,&QCheckBox::toggled,&dialog,[&,setter](bool v){if(loadingSettings_)return;setter(v);changed();});return box;};
    auto property=[&](const QString&group,const QString&key,const QString&label,double fallback,double lo,double hi,int decimals){auto* spin=number(label,settings[group].toObject().value(key).toDouble(fallback),lo,hi,decimals,fallback,[&,group,key](double v){auto o=settings[group].toObject();o[key]=v;settings[group]=o;});reloadControls.push_back([this,spin,group,key,fallback]{spin->setValue(settings[group].toObject().value(key).toDouble(fallback));});return spin;};
    if(kind=="曝光"){
        property("exposureSettings","exposure","曝光",0,-20,20,2);property("exposureSettings","offset","Offset",0,-.5,.5,4);property("exposureSettings","gamma","伽马",1,.01,9.99,2);
    }else if(kind=="颗粒"){
        property("grainSettings","amount","数量",25,0,100,1);property("grainSettings","size","大小",1.5,.5,20,2);property("grainSettings","roughness","Roughness",50,0,100,1);
    }else if(kind=="色彩平衡"){
        // Mirrors the reference layout: Color Levels readouts, three opposing sliders, and a tone selector.
        const QStringList channelKeys{"cyanRed","magentaGreen","yellowBlue"};
        const QStringList leftLabels{"Cyan","Magenta","Yellow"},rightLabels{"红色","绿色","蓝色"};
        auto write=[this](const QString& channel,double value){auto group=settings["colorBalanceSettings"].toObject();auto band=group[balanceTone].toObject();band[channel]=value;group[balanceTone]=band;settings["colorBalanceSettings"]=group;};
        auto* levels=new QWidget;auto* levelsLayout=new QHBoxLayout(levels);levelsLayout->setContentsMargins(0,0,0,0);
        levelsLayout->addWidget(new QLabel("色阶："));
        std::array<QSpinBox*,3> levelBoxes{};std::array<ui::TrackSlider*,3> sliders{};
        auto* sliderArea=new QWidget;auto* grid=new QGridLayout(sliderArea);grid->setContentsMargins(0,10,0,0);grid->setVerticalSpacing(10);
        for(int i=0;i<3;++i){
            const QString pairName=leftLabels[i]+"-"+rightLabels[i];
            levelBoxes[size_t(i)]=new QSpinBox;levelBoxes[size_t(i)]->setRange(-100,100);levelBoxes[size_t(i)]->setObjectName(QString("colorLevel%1").arg(i));levelBoxes[size_t(i)]->setAccessibleName(pairName+" 值");levelsLayout->addWidget(levelBoxes[size_t(i)]);
            sliders[size_t(i)]=new ui::TrackSlider(Qt::Horizontal);sliders[size_t(i)]->setRange(-100,100);sliders[size_t(i)]->setDefaultValue(0);sliders[size_t(i)]->setObjectName(QString("colorBalanceSlider%1").arg(i));sliders[size_t(i)]->setAccessibleName(pairName+"滑块");sliders[size_t(i)]->setMinimumHeight(22);
            // Same muted palette as the Hue/Saturation sliders.
            const char* gradient[]{"#6bbcc3, stop:1 #db6868","#c879c5, stop:1 #7bb878","#d5c46b, stop:1 #738cda"};
            sliders[size_t(i)]->setStyleSheet(QString("QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 %1); } QSlider::sub-page:horizontal { background: transparent; }").arg(gradient[i]));
            grid->addWidget(new QLabel(leftLabels[i]),i,0);grid->addWidget(sliders[size_t(i)],i,1);grid->addWidget(new QLabel(rightLabels[i]),i,2);
        }
        levelsLayout->addStretch();
        auto sync=[this,levelBoxes,sliders,channelKeys]{
            const auto values=settings["colorBalanceSettings"].toObject()[balanceTone].toObject();
            for(size_t i=0;i<3;++i){const int value=int(std::lround(values.value(channelKeys[int(i)]).toDouble()));QSignalBlocker spin(levelBoxes[i]),slider(sliders[i]);levelBoxes[i]->setValue(value);sliders[i]->setValue(value);}
        };
        for(int i=0;i<3;++i){
            const QString channel=channelKeys[i];
            QObject::connect(sliders[size_t(i)],&QSlider::valueChanged,&dialog,[&,write,sync,channel](int value){if(loadingSettings_)return;write(channel,value);sync();changed();});
            QObject::connect(levelBoxes[size_t(i)],&QSpinBox::valueChanged,&dialog,[&,write,sync,channel](int value){if(loadingSettings_)return;write(channel,value);sync();changed();});
        }
        auto* toneGroup=new QGroupBox("色调平衡");auto* toneLayout=new QHBoxLayout(toneGroup);
        const std::array<std::pair<const char*,const char*>,3> toneChoices{{{"shadows","阴影"},{"midtones","中间调"},{"highlights","高光"}}};
        for(const auto& [toneKey,toneLabel]:toneChoices){
            auto* radio=new QRadioButton(toneLabel);radio->setObjectName(QString("colorBalanceTone_%1").arg(toneKey));radio->setChecked(balanceTone==toneKey);toneLayout->addWidget(radio);
            QObject::connect(radio,&QRadioButton::toggled,&dialog,[&,sync,toneKey](bool on){if(!on)return;balanceTone=toneKey;sync();});
        }
        form.addRow(levels);form.addRow(sliderArea);form.addRow(toneGroup);
        reloadControls.push_back(sync);sync();
    }else if(kind=="黑白"){
        property("blackWhiteSettings","reds","红色",40,-200,300,0);property("blackWhiteSettings","yellows","黄色",60,-200,300,0);property("blackWhiteSettings","greens","绿色",40,-200,300,0);
        property("blackWhiteSettings","cyans","青色",60,-200,300,0);property("blackWhiteSettings","blues","蓝色",20,-200,300,0);property("blackWhiteSettings","magentas","洋红",80,-200,300,0);
    }else if(kind=="渐变映射"){
        for(const QString key:{"shadows","highlights"}){auto*button=new QPushButton;auto update=[&,button,key]{auto c=settings["gradientMapSettings"].toObject()[key].toObject();button->setText(QColor::fromRgbF(c["red"].toDouble(key=="highlights"),c["green"].toDouble(key=="highlights"),c["blue"].toDouble(key=="highlights")).name());};update();reloadControls.push_back(update);form.addRow(key=="shadows"?"阴影":"高光",button);
            QObject::connect(button,&QPushButton::clicked,&dialog,[&,key,update]{auto o=settings["gradientMapSettings"].toObject();auto c=o[key].toObject();
                if(host_.openColor){QPointer<AdjustmentPanel> owner=this;host_.openColor({c["red"].toDouble(),c["green"].toDouble(),c["blue"].toDouble()},key=="highlights"?"拾色器（渐变映射高光）":"拾色器（渐变映射阴影）",[owner,key,update](effects_tools::PaletteColor color){if(!owner||owner->closed_||owner->committing_)return;auto next=owner->settings["gradientMapSettings"].toObject();auto value=Object{{"red",color.red},{"green",color.green},{"blue",color.blue}};if(next[key].toObject()==value)return;next[key]=value;owner->settings["gradientMapSettings"]=next;update();owner->changed();});return;}
                auto color=QColorDialog::getColor(QColor::fromRgbF(c["red"].toDouble(),c["green"].toDouble(),c["blue"].toDouble()),&dialog,key);if(color.isValid()){o[key]=Object{{"red",color.redF()},{"green",color.greenF()},{"blue",color.blueF()}};settings["gradientMapSettings"]=o;update();changed();}});}
        auto* reverse=flag("反向",settings["gradientMapSettings"].toObject()["reversed"].toBool(),[&](bool v){auto o=settings["gradientMapSettings"].toObject();o["reversed"]=v;settings["gradientMapSettings"]=o;});reloadControls.push_back([this,reverse]{reverse->setChecked(settings["gradientMapSettings"].toObject()["reversed"].toBool());});
    }else if(kind=="色阶"){
        auto* channel=new QComboBox;channel->setObjectName("levelsChannel");channel->addItems({"RGB","红色","绿色","蓝色"});form.addRow("通道",channel);
        auto* controls=new LevelsAdvancedControls;levelsControls=controls;controls->setAdjustmentJson(encoded(settings));form.addRow(controls);
        std::array<QDoubleSpinBox*,5> values{};const QStringList labels{"输入黑场","伽马","输入白场","输出黑场","输出白场"};
        for(int i=0;i<5;++i){values[size_t(i)]=number(labels[i],i==1?1:i>=2?255:0,i==1?.1:0,i==1?9.99:255,i==1?2:1,i==1?1:i>=2?255:0,[&,controls,i](double value){auto typed=effects_tools::levelsFromAdjustmentJson(encoded(settings));auto& range=typed.ranges[size_t(typed.channel)];double* fields[]{&range.black,&range.gamma,&range.white,&range.outputBlack,&range.outputWhite};*fields[i]=value;settings=QJsonDocument::fromJson(QByteArray::fromStdString(effects_tools::withLevelsSettings(encoded(settings),typed))).object();controls->setAdjustmentJson(encoded(settings));});values[size_t(i)]->setObjectName(QString("levelsValue%1").arg(i));}
        for(auto* value:values){auto row=form.takeRow(value);if(row.labelItem)delete row.labelItem->widget();delete row.labelItem;delete row.fieldItem;}
        for(int startIndex:{0,3}){
            auto* row=new QWidget;auto* columns=new QHBoxLayout(row);columns->setContentsMargins(0,0,0,0);columns->setSpacing(12);
            for(int i=startIndex;i<(startIndex==0?3:5);++i){auto* column=new QVBoxLayout;column->setSpacing(5);auto* label=new QLabel(labels[i]);label->setStyleSheet("color: #989ba3; font-size: 11px;");column->addWidget(label);column->addWidget(values[size_t(i)]);columns->addLayout(column,1);}
            form.addRow(row);
        }
        auto load=[&,channel,values,controls]{auto typed=effects_tools::levelsFromAdjustmentJson(encoded(settings));QSignalBlocker block(channel);channel->setCurrentIndex(int(typed.channel));auto range=typed.ranges[size_t(typed.channel)].normalized();const double inputs[]{range.black,range.gamma,range.white,range.outputBlack,range.outputWhite};for(int i=0;i<5;++i){QSignalBlocker spin(values[size_t(i)]);values[size_t(i)]->setValue(inputs[i]);}controls->setAdjustmentJson(encoded(settings));};load();reloadControls.push_back(load);
        QObject::connect(channel,&QComboBox::currentIndexChanged,&dialog,[&,channel,load]{auto typed=effects_tools::levelsFromAdjustmentJson(encoded(settings));typed.channel=effects_tools::LevelsChannel(channel->currentIndex());settings=QJsonDocument::fromJson(QByteArray::fromStdString(effects_tools::withLevelsSettings(encoded(settings),typed))).object();load();changed();});
        for(auto* spin:values)QObject::connect(spin,&QDoubleSpinBox::valueChanged,&dialog,[load]{load();});
        controls->onChanged=[&,load](const std::string& json){settings=QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();load();changed();};
        auto* reset=new QPushButton("重置色阶");reset->setObjectName("levelsReset");form.addRow(reset);QObject::connect(reset,&QPushButton::clicked,&dialog,[&,load]{settings=QJsonDocument::fromJson(QByteArray::fromStdString(effects_tools::withLevelsSettings(encoded(settings),{}))).object();load();changed();});
        auto sample=std::make_shared<HistogramResult>();
        controls->onSampleModeChanged=[&](auto mode){preview.setCursor(mode?Qt::CrossCursor:Qt::ArrowCursor);};
        preview.onClick=[&,controls,sample](Point point){if(!controls->sampleMode())return;try{auto rgb=sample->composite?sampleDocumentOriginalRGB(*sample->composite,point):sample->sample?effects_tools::sampleOriginalRGB(*sample->sample,sample->transform,point,original.width,original.height):std::nullopt;if(rgb)controls->applySample(*rgb);}catch(const std::exception& error){status.setText(error.what());}};
        QObject::connect(&histogramWatcher,&QFutureWatcher<HistogramResult>::finished,&dialog,[&,controls,sample]{if(closing){retire();return;}*sample=histogramWatcher.result();if(!sample->error.isEmpty()){status.setText(sample->error);return;}controls->setHistogram(sample->bins);});
        histogramWatcher.setFuture(QtConcurrent::run([original=original,active=active,live=live,existing=existing,insertedId=insertedId,histogramCancelled=histogramCancelled]{HistogramResult result;try{result.transform={0,0,double(original.width),double(original.height)};auto found=std::find_if(original.layers.begin(),original.layers.end(),[&](const Layer& layer){return layer.id==active;});if(!live){if(found==original.layers.end()||!found->raster)throw std::runtime_error("请选择一个图像图层");result.sample=found->raster;result.transform=found->transform;result.bins=levelsPreviewHistogram(*result.sample,result.transform,original,[histogramCancelled]{return histogramCancelled->load();}).bins;}else{result.composite=std::make_shared<const Document>(levelsInput(original,active,existing,insertedId));result.bins=documentLevelsPreviewHistogram(*result.composite,[histogramCancelled]{return histogramCancelled->load();});}}catch(const std::exception& error){result.error=error.what();}return result;}));
    }else if(kind=="曲线"){
        auto* channel=new QComboBox;channel->setObjectName("curvesChannel");channel->addItems({"RGB","红色","绿色","蓝色"});channel->setCurrentIndex(int(effects_tools::curvesFromAdjustmentJson(encoded(settings)).channel));form.addRow("通道",channel);
        auto* controls=new CurvesAdvancedControls;controls->setAdjustmentJson(encoded(settings));form.addRow(controls);
        reloadControls.push_back([this,channel,controls]{QSignalBlocker block(channel);channel->setCurrentIndex(int(effects_tools::curvesFromAdjustmentJson(encoded(settings)).channel));controls->setAdjustmentJson(encoded(settings));});
        controls->onChanged=[&](const std::string& json){settings=QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();changed();};
        QObject::connect(channel,&QComboBox::currentIndexChanged,&dialog,[&,channel,controls]{auto typed=effects_tools::curvesFromAdjustmentJson(encoded(settings));typed.channel=effects_tools::LevelsChannel(channel->currentIndex());settings=QJsonDocument::fromJson(QByteArray::fromStdString(effects_tools::withCurvesSettings(encoded(settings),typed))).object();controls->setAdjustmentJson(encoded(settings));changed();});
    }else if(kind=="Hue/Saturation"){
        settings=QJsonDocument::fromJson(QByteArray::fromStdString(effects_tools::withHueSettings(encoded(settings),effects_tools::hueFromAdjustmentJson(encoded(settings))))).object();
        auto* range=new QComboBox;range->setObjectName("hueRange");range->addItems({"主","红色","黄色","绿色","青色","蓝色","洋红"});form.addRow("范围",range);
        auto* controls=new HueAdvancedControls;hueControls=controls;controls->setAdjustmentJson(encoded(settings));
        std::array<QDoubleSpinBox*,3> spins{};const QStringList labels{"色相","饱和度","Lightness"};
        for(int i=0;i<3;++i){spins[size_t(i)]=number(labels[i],0,i==0?-180:-100,i==0?180:100,1,0,[&,controls,i](double value){auto typed=effects_tools::hueFromAdjustmentJson(encoded(settings));auto& adjustment=typed.adjustments[size_t(typed.range)];if(i==0)adjustment.hue=value;else if(i==1)adjustment.saturation=value;else adjustment.lightness=value;settings=QJsonDocument::fromJson(QByteArray::fromStdString(effects_tools::withHueSettings(encoded(settings),typed))).object();controls->setAdjustmentJson(encoded(settings));});spins[size_t(i)]->setObjectName(QString("hueValue%1").arg(i));}
        form.addRow(controls);auto* outside=new QCheckBox("改为应用到该范围之外");outside->setObjectName("hueInvertRange");form.addRow(outside);auto* colorize=new QCheckBox("着色");colorize->setObjectName("hueColorize");form.addRow(colorize);
        auto load=[&,range,controls,spins,outside,colorize]{auto typed=effects_tools::hueFromAdjustmentJson(encoded(settings));QSignalBlocker rangeBlock(range),outsideBlock(outside),colorizeBlock(colorize);range->setCurrentIndex(int(typed.range));range->setEnabled(!typed.colorize);outside->setChecked(typed.invertRange);outside->setVisible(typed.range!=effects_tools::ColorRange::Master&&!typed.colorize);colorize->setChecked(typed.colorize);auto value=typed.adjustments[size_t(typed.range)];double inputs[]{value.hue,value.saturation,value.lightness};for(int i=0;i<3;++i){QSignalBlocker spin(spins[size_t(i)]);spins[size_t(i)]->setRange(i==0?(typed.colorize?0:-180):i==1&&typed.colorize?0:-100,i==0?(typed.colorize?360:180):100);spins[size_t(i)]->setValue(inputs[i]);}controls->setAdjustmentJson(encoded(settings));};load();reloadControls.push_back(load);
        auto store=[&,load](const effects_tools::HueSettings& typed){settings=QJsonDocument::fromJson(QByteArray::fromStdString(effects_tools::withHueSettings(encoded(settings),typed))).object();load();changed();};
        QObject::connect(range,&QComboBox::currentIndexChanged,&dialog,[&,range,store]{auto typed=effects_tools::hueFromAdjustmentJson(encoded(settings));typed.range=effects_tools::ColorRange(range->currentIndex());store(typed);});
        QObject::connect(outside,&QCheckBox::toggled,&dialog,[&,store](bool value){auto typed=effects_tools::hueFromAdjustmentJson(encoded(settings));typed.invertRange=value;store(typed);});
        QObject::connect(colorize,&QCheckBox::toggled,&dialog,[store](bool value){store(value?effects_tools::HueSettings::colorizeStart():effects_tools::HueSettings{});});
        auto* reset=new QPushButton("重置色相/饱和度");reset->setObjectName("hueReset");form.addRow(reset);QObject::connect(reset,&QPushButton::clicked,&dialog,[&,store]{store(effects_tools::hueFromAdjustmentJson(encoded(settings)).colorize?effects_tools::HueSettings::colorizeStart():effects_tools::HueSettings{});});
        controls->onChanged=[&,load](const std::string& json){settings=QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();load();changed();};
        auto cursor=[&,controls]{preview.setCursor(controls->sampleMode()||controls->targeting()?Qt::CrossCursor:Qt::ArrowCursor);};controls->onSampleModeChanged=[cursor](auto){cursor();};controls->onTargetingChanged=[cursor](bool){cursor();};
        auto colorAt=[&](Point point)->std::optional<effects_tools::PaletteColor>{try{if(host_.sampleComposite)return host_.sampleComposite(point);return effects_tools::sampleCompositeColor(previewEnabled.isChecked()&&completed?completed->document:original,point,SoftwareRenderer());}catch(const std::exception& error){status.setText(error.what());return {};}};
        preview.onClick=[controls,colorAt](Point point){if(controls->sampleMode())if(auto color=colorAt(point))controls->applySample(*color);};
        preview.onBeginDrag=[controls,colorAt](Point point){if(!controls->targeting())return false;if(auto color=colorAt(point))return controls->beginTarget(*color);return false;};
        preview.onDrag=[controls](double delta,bool hue){controls->dragTarget(delta,hue);};preview.onEndDrag=[controls]{controls->endTarget();};
    }
    start=[this]{if(closing)return;if(watcher.isRunning()){pending=true;previewCancelled->store(true);return;}pending=false;previewCancelled=std::make_shared<std::atomic_bool>(false);runningRevision=revision;const auto json=encoded(settings);const bool showPreview=previewEnabled.isChecked(),full=committing_;status.setText(full?"正在应用调整…":"正在更新预览…");watcher.setFuture(QtConcurrent::run([original=original,active=active,json,live=live,existing=existing,insertedId=insertedId,showPreview,full,previewCancelled=previewCancelled]{return makePreview(original,active,json,live,existing,insertedId,showPreview,full,[previewCancelled]{return previewCancelled->load();});}));};
    QObject::connect(&debounce,&QTimer::timeout,&dialog,start);
    QObject::connect(&watcher,&QFutureWatcher<PreviewResult>::finished,&dialog,[&]{if(closing){retire();return;}auto result=watcher.result();if(pending||runningRevision!=revision){start();return;}if(!result.error.isEmpty()){status.setText(result.error);apply->setEnabled(false);if(result.full){if(host_.error)host_.error(result.error);committing_=false;dialog.reject();}return;}completed=std::move(result.value);if(host_.valid&&!host_.valid()){committing_=false;cancel();return;}if(result.full){dialog.accept();return;}if(host_.preview)host_.preview(previewEnabled.isChecked()?std::make_shared<const Document>(completed->document):nullptr);preview.setPixmap(QPixmap::fromImage(result.image));status.setText(previewEnabled.isChecked()?"":"原始图像");apply->setEnabled(true);});
    QObject::connect(apply,&QPushButton::clicked,&dialog,[&]{
        if(host_.valid&&!host_.valid()){cancel();return;}
        if(host_.closeColor)host_.closeColor(true);
        if(!live){
            const auto exposure=settings["exposureSettings"].toObject(),grain=settings["grainSettings"].toObject();
            if((kind=="曝光"&&exposure.value("exposure").toDouble()==0&&exposure.value("offset").toDouble()==0&&exposure.value("gamma").toDouble(1)==1)||(kind=="颗粒"&&grain.value("amount").toDouble(25)==0)){dialog.reject();return;}
            if(options.onApply)options.onApply(encoded(settings));
        }
        committing_=true;apply->setEnabled(false);buttons.button(QDialogButtonBox::Cancel)->setEnabled(false);fields.setEnabled(false);previewEnabled.setEnabled(false);debounce.stop();
        if(host_.preview)host_.preview(previewEnabled.isChecked()&&completed?std::make_shared<const Document>(completed->document):nullptr);
        start();
    });QObject::connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(&dialog,&QDialog::finished,this,[this](int answer){finish(answer);});
    if(modal){dialog.setWindowModality(Qt::WindowModal);}else{
        dialog.setWindowFlags(Qt::Tool|Qt::WindowTitleHint|Qt::WindowCloseButtonHint);dialog.setWindowModality(Qt::NonModal);preview.hide();
        layout.setDirection(QBoxLayout::TopToBottom);layout.setContentsMargins(18,16,18,16);layout.setSpacing(12);layout.setStretch(1,0);
        fields.setMinimumWidth(360);form.setContentsMargins(0,0,0,0);form.setVerticalSpacing(12);form.setHorizontalSpacing(16);
        rightLayout.setContentsMargins(0,0,0,0);rightLayout.removeWidget(&previewEnabled);rightLayout.removeWidget(&buttons);
        auto* footer=new QHBoxLayout;footer->addWidget(&previewEnabled);footer->addStretch();footer->addWidget(&buttons);rightLayout.addLayout(footer);
        status.setStyleSheet("color: #989ba3; font-size: 11px;");status.setMinimumHeight(16);
        dialog.resize(dialog.sizeHint());if(auto found=positions().find(kind_);found!=positions().end())dialog.move(found->second);
    }
    start();dialog.show();dialog.raise();dialog.activateWindow();
    if(auto* first=dialog.findChild<QDoubleSpinBox*>())first->setFocus(Qt::ActiveWindowFocusReason);
    else previewEnabled.setFocus(Qt::ActiveWindowFocusReason);

    }
public:
    AdjustmentPanel(QWidget* parent,const Document& input,const std::string& id,const QString& requestedKind,bool isLive,bool isExisting,const AdjustmentDialogOptions& initial,ui::EditPanelHost host,bool compatibility)
        :EditPanelSession(parent,requestedKind=="色阶"?Kind::Levels:requestedKind=="Hue/Saturation"?Kind::Hue:Kind::Filter,isLive,std::move(host)),
        original(input),active(id),live(isLive),existing(isExisting),modal(compatibility),options(initial),dialog(parent),layout(&dialog),form(&fields),rightLayout(&right),previewEnabled("预览"),buttons(QDialogButtonBox::Apply|QDialogButtonBox::Cancel){initialize(requestedKind);kind_=kind=="色阶"?Kind::Levels:kind=="Hue/Saturation"?Kind::Hue:Kind::Filter;}
    ~AdjustmentPanel()override{closing=true;previewCancelled->store(true);histogramCancelled->store(true);QObject::disconnect(&watcher,nullptr,&dialog,nullptr);QObject::disconnect(&histogramWatcher,nullptr,&dialog,nullptr);watcher.waitForFinished();histogramWatcher.waitForFinished();}
    QDialog* panel()const override{return const_cast<AdjustmentWindow*>(&dialog);}
    bool committing()const override{return committing_;}
    void cancel()override{if(!committing_&&!closed_)dialog.reject();}
    std::string adjustmentSettings()const override{return encoded(settings);}
    std::optional<std::string> originalAdjustmentSettings()const override{
        if(closed_||!live)return {};
        auto found=std::find_if(original.layers.begin(),original.layers.end(),[this](const Layer& layer){return layer.id==active;});
        if(found==original.layers.end()||found->adjustmentJson.empty())return {};
        return found->adjustmentJson;
    }
    bool updateAdjustmentSettings(const std::string& json,bool previewState)override{
        if(closed_||committing_)return false;
        effects::validateAdjustmentJson(json);
        auto next=QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
        if(next["kind"].toString()!=kind)return false;
        // A complete state replacement keeps the ordinary control observers,
        // debounced worker revision and eventual Apply/history transaction.
        settings=std::move(next);
        {QScopedValueRollback<bool> loading(loadingSettings_,true);for(const auto& reload:reloadControls)reload();QSignalBlocker block(previewEnabled);previewEnabled.setChecked(previewState);}
        changed();return true;
    }
    bool samplePress(Point point,double viewX,Qt::KeyboardModifiers)override{
        if(closed_||committing_)return false;
        if((levelsControls&&levelsControls->sampleMode())||(hueControls&&hueControls->sampleMode())){if(preview.onClick)preview.onClick(point);dialog.activateWindow();return true;}
        if(hueControls&&hueControls->targeting()&&preview.onBeginDrag&&preview.onBeginDrag(point)){targetDragging_=true;targetAnchor_=viewX;return true;}return false;
    }
    bool sampleMove(Point,double viewX,Qt::KeyboardModifiers modifiers,bool finish)override{
        if(!targetDragging_)return false;if(preview.onDrag)preview.onDrag(viewX-targetAnchor_,modifiers.testFlag(Qt::ControlModifier));if(finish){targetDragging_=false;if(preview.onEndDrag)preview.onEndDrag();}return true;
    }
};
}
ui::EditPanelSession* openAdjustmentPanel(QWidget* parent,const Document& original,const std::string& active,const QString& kind,bool live,bool existing,const AdjustmentDialogOptions& options,ui::EditPanelHost host){
    return new AdjustmentPanel(parent,original,active,kind,live,existing,options,std::move(host),false);
}
std::optional<AdjustmentDialogResult> showAdjustmentDialog(QWidget* parent,const Document& original,const std::string& active,const QString& kind,bool live,bool existing,const AdjustmentDialogOptions& options){
    QEventLoop loop;std::optional<AdjustmentDialogResult> result;ui::EditPanelHost host;
    host.commit=[&](ui::EditPanelResult value){result=AdjustmentDialogResult{std::move(value.document),std::move(value.active)};};host.closed=[&]{loop.quit();};
    new AdjustmentPanel(parent,original,active,kind,live,existing,options,std::move(host),true);loop.exec();return result;
}
}
