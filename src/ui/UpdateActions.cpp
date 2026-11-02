#include "MainWindow.h"
#include "AppLogging.h"
#include "UpdatePanel.h"
#include "update/ReleaseCheck.h"
#include "update/Updater.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <memory>
#include <optional>

namespace compositor {
namespace {
std::optional<update::Version> runningVersion(){
    try{return update::Version::parse(QCoreApplication::applicationVersion());}catch(const std::exception&){return std::nullopt;}
}
struct FetchResult {QByteArray response;QString error;};
}
void MainWindow::checkForUpdates(){
    ui::UpdatePanelHost host;
    host.checkRelease=[this](std::function<void(const QString&)> report){checkForRelease(std::move(report));};
    host.restart=[this](const std::filesystem::path& root){
        // Keep the old process usable if a save prompt or launch fails.
        const bool quit=qApp->quitOnLastWindowClosed();
        qApp->setQuitOnLastWindowClosed(false);
        try{
            if(!close()){qApp->setQuitOnLastWindowClosed(quit);return false;}
            update::launchCurrent(root,{});
        }catch(...){
            // Accepted closing already released every project. Reopen a normal
            // welcome session so a launch error leaves a usable editor.
            closingWindow_=false;
            qApp->setQuitOnLastWindowClosed(quit);
            if(projects_.empty())addEmptyProject(false);
            show();throw;
        }
        qApp->setQuitOnLastWindowClosed(quit);
        if(quit)QTimer::singleShot(0,qApp,&QCoreApplication::quit);
        return true;
    };
    ui::openUpdatePanel(this,std::move(host));
}
void MainWindow::startAutomaticUpdateChecks(int initialDelayMs,int intervalMs){
    if(releaseTimer_)return;
    releaseTimer_=new QTimer(this);releaseTimer_->setObjectName("automaticReleaseCheck");releaseTimer_->setInterval(intervalMs);
    auto tick=[this]{
        QSettings settings;
        if(!update::automaticChecksEnabled(settings)||!update::releaseCheckDue(settings,QDateTime::currentDateTimeUtc()))return;
        runReleaseCheck(false);
    };
    connect(releaseTimer_,&QTimer::timeout,this,tick);
    QTimer::singleShot(initialDelayMs,this,[this,tick]{tick();releaseTimer_->start();});
}
void MainWindow::checkForRelease(std::function<void(const QString&)> report){
    if(report)releaseReports_.push_back(std::move(report));
    runReleaseCheck(true);
}
void MainWindow::runReleaseCheck(bool manual){
    releaseCheckManual_=releaseCheckManual_||manual;
    if(releaseCheckRunning_)return; // The running check reports to every waiting caller.
    if(!runningVersion()){finishReleaseCheck({},"此构建没有可比较的发布版本。");return;}
    releaseCheckRunning_=true;
    auto fetcher=releaseFetcher_?releaseFetcher_:ReleaseFetcher([url=update::releaseCheckUrl()](const std::function<bool()>& cancelled){return update::fetchReleaseMetadata(url,cancelled);});
    auto cancelled=std::make_shared<std::atomic<bool>>(false);
    // The watcher belongs to the window: if the window goes away first, the
    // worker is told to stop and its late result is never delivered.
    auto* watcher=new QFutureWatcher<FetchResult>(this);
    connect(watcher,&QObject::destroyed,[cancelled]{cancelled->store(true);});
    connect(watcher,&QFutureWatcher<FetchResult>::finished,this,[this,watcher]{
        const auto result=watcher->result();watcher->deleteLater();
        finishReleaseCheck(result.response,result.error);
    });
    watcher->setFuture(QtConcurrent::run([fetcher,cancelled]{
        FetchResult result;
        try{result.response=fetcher([cancelled]{return cancelled->load();});}
        catch(const std::exception& error){result.error=QString::fromUtf8(error.what());}
        catch(...){result.error="未知的发布检查失败";}
        return result;
    }));
}
void MainWindow::finishReleaseCheck(const QByteArray& response,const QString& fetchError){
    const bool manual=releaseCheckManual_;releaseCheckRunning_=false;releaseCheckManual_=false;
    auto reports=std::move(releaseReports_);releaseReports_.clear();
    QString status;
    const auto running=runningVersion();
    QSettings settings;
    if(running)update::recordReleaseCheck(settings,QDateTime::currentDateTimeUtc());
    try{
        if(!fetchError.isEmpty())throw std::runtime_error(fetchError.toUtf8().constData());
        const auto release=update::parseLatestRelease(response);
        if(!release||release->version<=*running)status="Compositor "+running->text()+" 已是最新版本。";
        else{
            status="Compositor "+release->version.text()+" 现已发布。";
            const auto skipped=update::skippedRelease(settings);
            if(manual||!skipped||*skipped!=release->version)showUpdateNotice(release->version.text(),running->text(),release->pageUrl);
        }
    }catch(const std::exception& error){
        status="无法检查更新： "+QString::fromUtf8(error.what());
        logging::trace("release check failed: "+QString::fromUtf8(error.what()));
    }
    setProperty("releaseCheckStatus",status);
    for(auto& report:reports)report(status);
}
void MainWindow::showUpdateNotice(const QString& version,const QString& running,const QString& pageUrl){
    for(auto* old:findChildren<QDialog*>("updateAvailableNotice")){old->setObjectName({});old->close();}
    auto* notice=new QDialog(this);notice->setObjectName("updateAvailableNotice");notice->setAttribute(Qt::WA_DeleteOnClose);
    notice->setWindowTitle("有可用更新");notice->setWindowModality(Qt::NonModal);notice->setProperty("releaseVersion",version);
    auto* layout=new QVBoxLayout(notice);
    auto* text=new QLabel(QString("Compositor %1 已可用，当前运行的是 %2。\n\n安装仍需手动完成：请从发布页面下载新的 MSI 或便携版程序包。Compositor 本身不会校验下载内容。").arg(version,running));
    text->setObjectName("updateNoticeText");text->setWordWrap(true);layout->addWidget(text);
    auto* row=new QHBoxLayout;layout->addLayout(row);
    auto* download=new QPushButton("下载…");download->setObjectName("updateNoticeDownload");download->setDefault(true);
    auto* skip=new QPushButton("跳过此版本");skip->setObjectName("updateNoticeSkip");
    auto* later=new QPushButton("Later");later->setObjectName("updateNoticeLater");
    row->addWidget(download);row->addStretch();row->addWidget(skip);row->addWidget(later);
    connect(download,&QPushButton::clicked,notice,[this,notice,pageUrl]{
        const QUrl url(pageUrl);
        if(urlOpener_)urlOpener_(url);else QDesktopServices::openUrl(url);
        notice->close();
    });
    connect(skip,&QPushButton::clicked,notice,[notice,version]{
        QSettings settings;update::skipRelease(settings,update::Version::parse(version));notice->close();
    });
    connect(later,&QPushButton::clicked,notice,&QDialog::close);
    notice->show();
}
}