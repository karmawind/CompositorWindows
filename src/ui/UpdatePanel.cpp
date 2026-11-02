#include "UpdatePanel.h"
#include "update/ReleaseCheck.h"
#include "update/Updater.h"
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCheckBox>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QSettings>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>

namespace compositor::ui {
namespace {
struct Configuration {
    std::filesystem::path root;
    QString feed, runningVersion, information;
    bool configured{};
};
Configuration configuration(const QString& applicationDirectory) {
    Configuration result;
    if (QCoreApplication::instance()->property("manualUpdatesOnly").toBool()) {
        result.information = "Compositor 会检查 GitHub 上的新稳定版本，并提供其下载页面的链接。要更新，请安装新的 MSI 或解压新的便携版下载包。";
        return result;
    }
    QDir version(applicationDirectory);
    result.runningVersion = version.dirName();
    QDir versions = version;
    if (!versions.cdUp() || versions.dirName().compare("versions", Qt::CaseInsensitive) != 0) {
        result.information = "此开发版本未配置更新。此副本不在带版本的安装中。";
        return result;
    }
    try { (void)update::Version::parse(result.runningVersion); }
    catch (const std::exception&) {
        result.information = "此开发版本未配置更新。安装版本无效。";
        return result;
    }
    if (!versions.cdUp()) throw std::runtime_error("无法找到更新安装根目录");
    result.root = std::filesystem::path(versions.absolutePath().toStdWString());
    const QString path = versions.filePath("update-source.json");
    if (!QFileInfo::exists(path)) {
        result.information = "此开发版本未配置更新。未配置更新源，且生产更新不可用。";
        return result;
    }
    const auto wide = path.toStdWString();
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("更新配置必须是普通文件");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 16384)
        throw std::runtime_error("无法读取受限的更新配置");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("更新配置不是 JSON 对象");
    const auto object = document.object();
    const QStringList expected{"allowTestKey", "schema", "testFeed"};
    if (object.keys() != expected || !object["schema"].isDouble() || object["schema"].toDouble() != 1 ||
        !object["allowTestKey"].isBool() || !object["allowTestKey"].toBool() ||
        !object["testFeed"].isString() || object["testFeed"].toString().trimmed().isEmpty())
        throw std::runtime_error("Development updates require schema 1, a testFeed, and explicit allowTestKey: true");
    result.feed = object["testFeed"].toString();
    if (result.feed.size() > 8192 || result.feed.contains(QChar(u'\0')))
        throw std::runtime_error("更新源长度无效或包含空字符");
    result.configured = true;
    result.information = "已签名的开发更新源。未配置生产更新。";
    return result;
}
struct Progress {
    std::atomic<bool> cancelled{};
    std::atomic<std::uint64_t> done{}, total{};
};
struct Result {
    std::optional<update::ActiveState> active;
    std::optional<update::CheckResult> checked;
    std::optional<update::InstallResult> installed;
    QString error;
    bool cancelled{};
};
class UpdatePanel final : public QDialog {
public:
    UpdatePanel(QWidget* parent, UpdatePanelHost host, QString directory)
        : QDialog(parent), host_(std::move(host)), directory_(std::move(directory)) {
        setObjectName("updatePanel"); setWindowTitle("检查更新");
        setWindowModality(Qt::NonModal); setAttribute(Qt::WA_DeleteOnClose); resize(520, 260);
        auto* layout = new QVBoxLayout(this);
        information_ = new QLabel; information_->setObjectName("updateSourceInformation"); information_->setWordWrap(true); layout->addWidget(information_);
        versions_ = new QLabel; versions_->setObjectName("updateVersions"); versions_->setWordWrap(true); layout->addWidget(versions_);
        status_ = new QLabel("正在准备更新检查…"); status_->setObjectName("updateStatus"); status_->setAccessibleName("更新状态"); status_->setWordWrap(true); layout->addWidget(status_);
        progressBar_ = new QProgressBar; progressBar_->setObjectName("updateProgress"); progressBar_->setAccessibleName("更新进度"); layout->addWidget(progressBar_); progressBar_->hide();
        retained_ = new QLabel; retained_->setObjectName("updateRetainedVersion"); retained_->setWordWrap(true); layout->addWidget(retained_);
        automatic_ = new QCheckBox("自动检查新的稳定版本"); automatic_->setObjectName("updateAutomatic");
        { QSettings settings; automatic_->setChecked(update::automaticChecksEnabled(settings)); }
        automatic_->setToolTip("Compositor 每天会向 GitHub 查询一次最新的稳定版本。安装仍需手动进行。");
        connect(automatic_, &QCheckBox::toggled, this, [](bool enabled) { QSettings settings; update::setAutomaticChecksEnabled(settings, enabled); });
        layout->addWidget(automatic_);
        releaseStatus_ = new QLabel; releaseStatus_->setObjectName("updateReleaseStatus"); releaseStatus_->setAccessibleName("版本检查状态"); releaseStatus_->setWordWrap(true); layout->addWidget(releaseStatus_);
        auto* row = new QHBoxLayout; layout->addLayout(row);
        releaseCheck_ = button(row, "检查新版本", "updateReleaseCheck"); releaseCheck_->setVisible(bool(host_.checkRelease));
        connect(releaseCheck_, &QPushButton::clicked, this, [this] { checkRelease(); });
        check_ = button(row, "重新检查", "updateCheck"); install_ = button(row, "安装更新", "updateInstall");
        cancel_ = button(row, "取消", "updateCancel"); restart_ = button(row, "重启 Compositor", "updateRestart");
        row->addStretch(); close_ = button(row, "Close", "updateClose");
        connect(check_, &QPushButton::clicked, this, [this] { begin(false); });
        connect(install_, &QPushButton::clicked, this, [this] { begin(true); });
        connect(cancel_, &QPushButton::clicked, this, [this] { requestCancel(); });
        connect(close_, &QPushButton::clicked, this, &QDialog::reject);
        connect(restart_, &QPushButton::clicked, this, [this] { restart(); });
        connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] { finished(); });
        timer_.setInterval(40); connect(&timer_, &QTimer::timeout, this, [this] { paintProgress(); });
        controls(); QTimer::singleShot(0, this, [this] { begin(false); });
    }
    ~UpdatePanel() override {
        progress_->cancelled.store(true); watcher_.disconnect(this);
        try { watcher_.waitForFinished(); } catch (...) { /* A worker result cannot escape QWidget teardown. */ }
    }
    void reject() override {
        if (busy_) { closeWhenSettled_ = true; requestCancel(); return; }
        QDialog::reject();
    }
private:
    static QPushButton* button(QHBoxLayout* row, const char* text, const char* name) {
        auto* result = new QPushButton(QString::fromUtf8(text)); result->setObjectName(name); row->addWidget(result); return result;
    }
    void controls() {
        check_->setEnabled(!busy_); install_->setEnabled(!busy_ && available_);
        cancel_->setVisible(busy_); cancel_->setEnabled(busy_ && !progress_->cancelled.load());
        restart_->setVisible(installed_); restart_->setEnabled(!busy_ && installed_ && bool(host_.restart));
        close_->setEnabled(!busy_);
    }
    void begin(bool install) {
        if (busy_ || (install && !available_)) return;
        available_ = false; installed_ = false; retained_->clear();
        try {
            configuration_ = configuration(directory_);
            // An unconfigured build explains itself in the status line alone; showing it in both labels repeated the text.
            information_->setText(configuration_.configured ? configuration_.information : QString{}); information_->setVisible(configuration_.configured);
            versions_->setText(configuration_.root.empty() ? QString{} : "运行版本：" + configuration_.runningVersion);
            if (!configuration_.configured) { status_->setText(configuration_.information); setProperty("updateState", "unconfigured"); controls(); return; }
            progress_ = std::make_shared<Progress>(); busy_ = true; installing_ = install;
            setProperty("updateState", install ? "installing" : "checking");
            status_->setText(install ? "正在安装并验证已签名的更新…" : "正在检查已签名的更新…");
            progressBar_->setRange(0, 0); progressBar_->show(); controls(); timer_.start();
            const auto config = configuration_; const auto progress = progress_;
            watcher_.setFuture(QtConcurrent::run([config, progress, install] {
                Result result;
                try {
                    update::Updater updater(config.root); result.active = updater.state();
                    update::Options options; options.allowTestKey = true;
                    options.cancelled = [progress] { return progress->cancelled.load(); };
                    options.progress = [progress](auto done, auto total) { progress->total.store(total); progress->done.store(done); };
                    if (install) result.installed = updater.install(config.feed, options);
                    else result.checked = updater.check(config.feed, options);
                    result.active = updater.state();
                } catch (const std::exception& error) {
                    result.error = QString::fromUtf8(error.what()); result.cancelled = progress->cancelled.load();
                    try { result.active = update::Updater(config.root).state(); } catch (...) {}
                }
                return result;
            }));
        } catch (const std::exception& error) {
            busy_ = false; timer_.stop(); progressBar_->hide(); status_->setText(QString::fromUtf8(error.what()));
            setProperty("updateState", "error"); controls();
        }
    }
    void checkRelease() {
        if (!host_.checkRelease || !releaseCheck_->isEnabled()) return;
        releaseCheck_->setEnabled(false); releaseStatus_->setText("正在检查新的稳定版本…");
        QPointer<UpdatePanel> alive(this);
        host_.checkRelease([alive](const QString& status) {
            if (!alive) return;
            alive->releaseStatus_->setText(status); alive->releaseCheck_->setEnabled(true);
        });
    }
    void requestCancel() {
        if (!busy_) return;
        progress_->cancelled.store(true); status_->setText("正在取消更新操作…"); controls();
    }
    void paintProgress() {
        const auto total = progress_->total.load(), done = progress_->done.load();
        if (!total) return;
        progressBar_->setRange(0, 1000); progressBar_->setValue(int(std::min<long double>(1000, static_cast<long double>(done) * 1000 / total)));
        if (installing_ && done >= total && !progress_->cancelled.load()) status_->setText("正在验证文件并测试已安装的版本…");
    }
    void finished() {
        timer_.stop(); paintProgress(); busy_ = false; progressBar_->hide();
        try {
            const auto result = watcher_.result();
            versions_->setText(QString("运行版本：%1\n当前安装版本：%2").arg(configuration_.runningVersion,
                result.active ? result.active->current.text() : "unavailable"));
            retained_->setText(result.active && !result.active->previous.isEmpty() ? "保留的先前版本：" + result.active->previous : QString{});
            installed_ = result.active && !result.active->pending && result.active->current.text() != configuration_.runningVersion;
            if (!result.error.isEmpty()) {
                status_->setText(result.cancelled ? (result.active
                    ? "更新已取消。当前安装的版本： " + result.active->current.text() + "."
                    : "更新已取消。无法读取当前安装的版本。 " + result.error) : result.error);
                setProperty("updateState", result.cancelled ? "cancelled" : "error"); available_ = false;
            } else if (result.installed) {
                status_->setText(installed_ ? "更新已安装。准备就绪后请重启 Compositor。" : "此版本已安装。");
                setProperty("updateState", installed_ ? "installed" : "current"); available_ = false;
            } else if (result.checked) {
                available_ = result.checked->available;
                status_->setText(available_ ? "更新 " + result.checked->manifest.version.text() + " 现已发布。"
                                           : installed_ ? "更新已安装完毕。准备就绪后请重启 Compositor。" : "没有可用的新版本。");
                setProperty("updateState", available_ ? "available" : installed_ ? "installed" : "current");
            }
        } catch (const std::exception& error) { status_->setText(QString::fromUtf8(error.what())); setProperty("updateState", "error"); }
        controls(); if (closeWhenSettled_) QDialog::reject();
    }
    void restart() {
        if (busy_ || !installed_ || !host_.restart) return;
        QPointer<UpdatePanel> alive(this);
        try {
            const bool accepted = host_.restart(configuration_.root);
            if (!alive) return;
            if (accepted) QDialog::accept();
            else status_->setText("已取消重启。已安装的更新将保持就绪，等待下次启动。");
        } catch (const std::exception& error) { if (alive) status_->setText(QString::fromUtf8(error.what())); }
    }
    UpdatePanelHost host_;
    QString directory_;
    Configuration configuration_;
    std::shared_ptr<Progress> progress_{std::make_shared<Progress>()};
    QFutureWatcher<Result> watcher_;
    QTimer timer_;
    QLabel *information_{}, *versions_{}, *status_{}, *retained_{}, *releaseStatus_{};
    QCheckBox* automatic_{};
    QPushButton* releaseCheck_{};
    QProgressBar* progressBar_{};
    QPushButton *check_{}, *install_{}, *cancel_{}, *restart_{}, *close_{};
    bool busy_{}, installing_{}, available_{}, installed_{}, closeWhenSettled_{};
};
}
QDialog* openUpdatePanel(QWidget* parent, UpdatePanelHost host, QString applicationDirectory) {
    if (parent) for (auto* dialog : parent->findChildren<QDialog*>("updatePanel"))
        if (auto* existing = dynamic_cast<UpdatePanel*>(dialog)) { existing->show(); existing->raise(); existing->activateWindow(); return existing; }
    if (applicationDirectory.isEmpty()) applicationDirectory = QCoreApplication::applicationDirPath();
    auto* panel = new UpdatePanel(parent, std::move(host), std::move(applicationDirectory)); panel->show(); return panel;
}
}
