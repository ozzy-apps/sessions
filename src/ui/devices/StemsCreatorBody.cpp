#include "Bodies.h"
#include "core/Model.h"
#include "stems/StemsCreator.h"
#include "stems/ModelAssets.h"
#include "ui/InfoView.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMenu>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
namespace sessions::devui {
namespace {
class StemsBody final : public DeviceBody {
public:
    StemsBody(const DeviceContext& context, QWidget* parent) : DeviceBody(context, parent) {
        setObjectName("stemsCreatorBody"); setAcceptDrops(true); setFixedWidth(344);
        service = StemsCreator::forProject(context.project, context.transport);
        auto* root = new QVBoxLayout(this); root->setContentsMargins(7, 5, 7, 5); root->setSpacing(4);
        source = new QLabel(this); source->setObjectName("stemsSource"); source->setTextFormat(Qt::PlainText);
        root->addWidget(source);
        auto* outputs = new QLabel("Vocals · Drums · Bass · Piano · Other", this);
        outputs->setTextFormat(Qt::PlainText); root->addWidget(outputs);
        auto* cpu = new QHBoxLayout;
        cpu->addWidget(new QLabel("CPU threads", this)); cpu->addWidget(field(0, 48)); cpu->addStretch();
        auto* options = new QPushButton("Options…", this); options->setObjectName("stemsOptions"); cpu->addWidget(options); root->addLayout(cpu);
        auto* actions = new QHBoxLayout;
        load = new QPushButton("Load File…", this); load->setObjectName("stemsLoadFile"); actions->addWidget(load);
        create = new QPushButton("Create Stems", this); create->setObjectName("stemsCreate"); actions->addWidget(create);
        cancel = new QPushButton("Cancel", this); cancel->setObjectName("stemsCancel"); actions->addWidget(cancel); root->addLayout(actions);
        progress = new QProgressBar(this); progress->setObjectName("stemsProgress"); progress->setRange(0, 1000); progress->setTextVisible(false); progress->setFixedHeight(7); root->addWidget(progress);
        status = new QLabel(this); status->setObjectName("stemsStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true);
        status->setFixedHeight(32); root->addWidget(status);
        auto* destination = new QLabel("Creates a group at the Arrangement insert marker.", this);
        destination->setTextFormat(Qt::PlainText); destination->setWordWrap(true); root->addWidget(destination); root->addStretch();
        connect(load, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getOpenFileName(this, "Stems Creator — Load Audio", {}, "Audio (" + samples::nameFilters().join(' ') + ");;All files (*)");
            if (!file.isEmpty()) service->loadSource(ctx().deviceId, file);
        });
        connect(create, &QPushButton::clicked, this, [this] { service->start(ctx().deviceId); });
        connect(cancel, &QPushButton::clicked, service, &StemsCreator::cancel);
        connect(options, &QPushButton::clicked, this, [this, options] {
            QMenu menu(this);
            menu.addAction("Output Folder…", this, [this] {
                const QString folder = QFileDialog::getExistingDirectory(this, "Stems Output Folder");
                if (!folder.isEmpty()) QSettings("Sessions", "Sessions").setValue("stems/outputFolder", folder);
            });
            menu.addAction("Use Project Output Folder", this, [] { QSettings("Sessions", "Sessions").remove("stems/outputFolder"); });
            menu.addSeparator();
            menu.addAction("Model Folder…", this, [this] {
                const QString folder = QFileDialog::getExistingDirectory(this, "Spleeter Model Folder", stems::modelDirectory());
                if (!folder.isEmpty()) QSettings("Sessions", "Sessions").setValue("stems/modelDirectory", folder);
                refresh();
            });
            menu.addAction("Use Bundled Models", this, [this] { QSettings("Sessions", "Sessions").remove("stems/modelDirectory"); refresh(); });
            menu.exec(options->mapToGlobal(QPoint(0, options->height())));
        });
        connect(service, &StemsCreator::changed, this, [this] { refresh(); });
        InfoView::attach(this, "Stems Creator", "Local neural separation into five categories. Other includes guitar and remaining instruments. Separation can contain bleed. Create Stems adds five aligned audio tracks in a group; Ctrl+Z removes the import.");
        refresh();
    }
    void refresh() override {
        DeviceBody::refresh();
        if (!source || !service) return;
        const auto* d = device();
        const bool active = service->busy() && service->activeDevice() == ctx().deviceId;
        const QString path = d ? d->samplePath : QString{};
        source->setText(fontMetrics().elidedText(path.isEmpty() ? "No audio loaded" : QFileInfo(path).fileName(), Qt::ElideMiddle, 326)); source->setToolTip(path);
        const QString missing = service->unavailableReason();
        load->setEnabled(!active);
        create->setEnabled(!service->busy() && !service->loadingSource(ctx().deviceId) && d && d->sample && missing.isEmpty()); create->setToolTip(missing);
        cancel->setEnabled(active);
        progress->setValue(int(service->progress() * 1000)); progress->setVisible(active);
        progress->setStyleSheet(QString("QProgressBar { background:%1; border:1px solid %2; } QProgressBar::chunk { background:%3; }").arg(col::display.name(), col::separator.name(), theme::accent.name()));
        status->setText(!missing.isEmpty() && !active ? missing : service->status(ctx().deviceId)); status->setToolTip(status->text());
    }
protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 && event->mimeData()->urls()[0].isLocalFile()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty() && event->mimeData()->urls()[0].isLocalFile()) {
            service->loadSource(ctx().deviceId, event->mimeData()->urls()[0].toLocalFile()); event->acceptProposedAction();
        }
    }
private:
    StemsCreator* service = nullptr;
    QLabel* source = nullptr;
    QLabel* status = nullptr;
    QPushButton *load = nullptr, *create = nullptr, *cancel = nullptr;
    QProgressBar* progress = nullptr;
};
}
DeviceBody* createStemsCreatorBody(const DeviceContext& context, QWidget* parent) { return new StemsBody(context, parent); }
} // namespace sessions::devui
