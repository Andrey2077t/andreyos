#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QPixmap>
#include <QMainWindow>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QImage>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QProcess>
#include <QFile>
#include <QHash>
#include <QTimer>
#include <QDateTime>
#include <QCloseEvent>
#include <QMessageBox>
#include <QFont>
#include <sys/stat.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr quint16 PORT = 47017;
constexpr int VIDEO_DEVICE = 42;
const QString RED = "#e51b32";
const QString DARK = "#21070c";
const QString CARD = "#3a0d15";
const QString MUTED = "#d8c4c8";
}

class AndreyCam final : public QMainWindow {
public:
    AndreyCam() {
        setWindowTitle("AndreyCam");
        setMinimumSize(1050, 700);
        resize(1320, 760);
        setStyleSheet(QStringLiteral(
            "QMainWindow,QWidget#central{background:%1;color:#fff;font-family:'Inter','Noto Sans',sans-serif;}"
            "QLabel,QRadioButton{background:transparent;color:#fff;}"
            "QFrame#panel{background:%2;border:1px solid #8f2638;border-radius:18px;}"
            "QLabel#muted{color:%3;} QRadioButton{spacing:10px;font-size:15px;}"
            "QRadioButton::indicator{width:18px;height:18px;}"
            "QRadioButton::indicator:checked{border:5px solid %4;background:#fff;border-radius:9px;}"
            "QRadioButton::indicator:unchecked{border:2px solid #aa9298;border-radius:9px;}"
            "QPushButton{background:#48101a;border:1px solid #9e2c3c;border-radius:13px;padding:12px 18px;font-weight:600;}"
            "QPushButton:hover{background:#651322;} QPushButton#start{background:%4;border:0;font-size:17px;}"
            "QPushButton#start:hover{background:#f3384b;}"
        ).arg(DARK, CARD, MUTED, RED));

        auto *central = new QWidget;
        central->setObjectName("central");
        auto *outer = new QHBoxLayout(central);
        outer->setContentsMargins(18, 18, 18, 18);
        outer->setSpacing(18);
        auto *sidebar = new QWidget;
        sidebar->setObjectName("sidebar");
        sidebar->setFixedWidth(205);
        sidebar->setStyleSheet("background:#19050a;border:1px solid #5e1421;border-radius:18px;");
        auto *side = new QVBoxLayout(sidebar);
        side->setContentsMargins(14, 18, 14, 18); side->setSpacing(8);
        auto *sideBrand = new QLabel("📷  AndreyCam"); sideBrand->setStyleSheet("font-size:20px;font-weight:700;padding:8px 2px 20px 2px;");
        side->addWidget(sideBrand);
        auto addNav = [side](const QString &name, bool active) {
            auto *item = new QLabel(name); item->setMinimumHeight(42); item->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            item->setStyleSheet(active ? "background:#a30e22;border:1px solid #e84a5c;border-radius:10px;padding-left:12px;font-weight:700;" : "color:#e5d9dc;padding-left:12px;");
            side->addWidget(item);
        };
        addNav("⌂   Início", true); addNav("⚙   Configurações", false); addNav("ⓘ   Sobre", false); addNav("?   Ajuda", false);
        auto *separator = new QFrame; separator->setFrameShape(QFrame::HLine); separator->setStyleSheet("color:#6a1827;margin:12px 3px;"); side->addWidget(separator);
        side->addStretch();
        auto *sideFooter = new QLabel("Conecte seu celular,\nuse como câmera ou\nmicrofone!");
        sideFooter->setAlignment(Qt::AlignCenter); sideFooter->setStyleSheet("color:#d9c4c9;font-size:14px;line-height:1.3;"); side->addWidget(sideFooter);
        outer->addWidget(sidebar);
        auto *content = new QWidget;
        auto *root = new QVBoxLayout(content);
        root->setContentsMargins(26, 22, 26, 24);
        root->setSpacing(18);
        auto *header = new QHBoxLayout;
        auto *brand = new QLabel("📷  AndreyCam");
        brand->setStyleSheet("font-size:30px;font-weight:700;");
        header->addWidget(brand);
        header->addStretch();
        status_ = new QLabel("●  Aguardando celular");
        status_->setStyleSheet("color:#ffc7cf;font-size:15px;");
        header->addWidget(status_);
        root->addLayout(header);

        auto *intro = new QLabel("Use seu celular como câmera ou microfone");
        intro->setObjectName("muted"); intro->setStyleSheet("font-size:16px;");
        root->addWidget(intro);

        auto *connection = panel();
        auto *connectionLayout = new QHBoxLayout(connection);
        connectionLayout->setContentsMargins(18,14,18,14);
        auto *address = new QLabel(QString("No AndreyCam Mobile, informe:  <b>%1</b>  porta  <b>%2</b>  código  <b>%3</b>")
                                   .arg(localAddress(), QString::number(PORT), QString::number(token_)));
        address->setWordWrap(true);
        connectionLayout->addWidget(address, 1);
        auto *copy = new QPushButton("Copiar endereço");
        connect(copy, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(localAddress() + ":" + QString::number(PORT) + "  código " + QString::number(token_));
        });
        connectionLayout->addWidget(copy);
        root->addWidget(connection);

        auto *choices = panel();
        auto *choiceLayout = new QVBoxLayout(choices);
        choiceLayout->setContentsMargins(20,18,20,18);
        auto *title = new QLabel("Escolha o que deseja usar");
        title->setStyleSheet("font-size:21px;font-weight:700;");
        choiceLayout->addWidget(title);
        auto *grid = new QGridLayout;
        grid->setSpacing(12);
        modes_ = new QButtonGroup(this);
        const QStringList labels = {"Apenas Microfone", "Apenas Câmera", "Microfone e Câmera"};
        const QStringList descriptions = {"Use o celular como entrada de áudio", "Use o celular como webcam", "Ative áudio e vídeo juntos"};
        for (int i=0; i<3; ++i) {
            auto *card = panel();
            auto *v = new QVBoxLayout(card);
            v->setContentsMargins(16,18,16,18);
            auto *radio = new QRadioButton(labels[i]);
            radio->setStyleSheet("font-weight:700;");
            auto *desc = new QLabel(descriptions[i]); desc->setObjectName("muted"); desc->setWordWrap(true);
            v->addWidget(radio); v->addWidget(desc); v->addStretch();
            modes_->addButton(radio, i); grid->addWidget(card, 0, i);
        }
        modes_->button(1)->setChecked(true);
        choiceLayout->addLayout(grid);
        preview_ = new QLabel("A prévia da câmera aparecerá aqui quando o celular iniciar a transmissão.");
        preview_->setAlignment(Qt::AlignCenter);
        preview_->setMinimumHeight(130);
        preview_->setStyleSheet("background:#100307;border:1px solid #6f1b2a;border-radius:12px;color:#bca8ad;padding:8px;");
        choiceLayout->addWidget(preview_);
        root->addWidget(choices, 1);

        auto *bottom = panel();
        auto *bottomLayout = new QHBoxLayout(bottom);
        bottomLayout->setContentsMargins(18,14,18,14);
        auto *help = new QLabel("Abra o AndreyCam Mobile no Android e conecte os dois dispositivos à mesma rede Wi-Fi.");
        help->setWordWrap(true); help->setObjectName("muted");
        bottomLayout->addWidget(help, 1);
        start_ = new QPushButton("Iniciar conexão"); start_->setObjectName("start");
        connect(start_, &QPushButton::clicked, this, &AndreyCam::toggleStream);
        bottomLayout->addWidget(start_);
        root->addWidget(bottom);
        outer->addWidget(content, 1);
        setCentralWidget(central);

        server_ = new QTcpServer(this);
        connect(server_, &QTcpServer::newConnection, this, &AndreyCam::acceptPhone);
        if (!server_->listen(QHostAddress::AnyIPv4, PORT)) {
            status_->setText("●  Não foi possível abrir a porta de conexão");
            status_->setStyleSheet("color:#ff7382;");
            start_->setEnabled(false);
        }
    }

    ~AndreyCam() override { stopOutputs(); }

protected:
    void closeEvent(QCloseEvent *event) override {
        if (phone_) phone_->disconnectFromHost();
        stopOutputs();
        QMainWindow::closeEvent(event);
    }

private:
    QLabel *status_ = nullptr;
    QPushButton *start_ = nullptr;
    QButtonGroup *modes_ = nullptr;
    QLabel *preview_ = nullptr;
    QTcpServer *server_ = nullptr;
    QTcpSocket *phone_ = nullptr;
    QByteArray rx_;
    QString token_ = QString::number(QRandomGenerator::global()->bounded(100000, 999999));
    bool authenticated_ = false;
    bool streaming_ = false;
    bool moduleLoading_ = false;
    QProcess *ffmpeg_ = nullptr;
    QProcess *modprobe_ = nullptr;
    QString pulseModule_;
    QString peer_;
    QHash<QString, int> authFailures_;
    QHash<QString, qint64> failureWindow_;
    int audioFd_ = -1;
    int pulseFd_ = -1;
    QByteArray currentFrame_;

    QFrame *panel() {
        auto *f = new QFrame; f->setObjectName("panel"); return f;
    }

    QString localAddress() const {
        QString fallback;
        for (const auto &iface : QNetworkInterface::allInterfaces()) {
            if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack)) continue;
            for (const auto &entry : iface.addressEntries()) {
                if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.ip().isLoopback()) continue;
                if (iface.type() == QNetworkInterface::Wifi) return entry.ip().toString();
                if (fallback.isEmpty() && iface.type() == QNetworkInterface::Ethernet)
                    fallback = entry.ip().toString();
            }
        }
        return fallback.isEmpty() ? "IP do computador" : fallback;
    }

    void toggleStream() {
        if (!phone_ || !authenticated_) {
            QMessageBox::information(this, "Conecte o celular", "Abra o AndreyCam Mobile, informe o endereço e o código mostrados nesta janela e toque em Conectar.");
            return;
        }
        if (streaming_) {
            phone_->write("STOP\n"); streaming_ = false; stopOutputs();
            start_->setText("Iniciar conexão"); status_->setText("●  Celular conectado"); return;
        }
        const int mode = modes_->checkedId();
        if (mode != 0 && !ensureVideoOutput()) {
            if (!moduleLoading_) QMessageBox::warning(this, "Câmera virtual indisponível",
                "Não consegui preparar /dev/video42. Instale v4l2loopback-dkms e os headers do kernel e tente novamente.");
            else { start_->setEnabled(false); status_->setText("●  Aguardando autorização para carregar a câmera virtual…"); }
            return;
        }
        beginStream(mode);
    }

    void beginStream(int mode) {
        if (!phone_ || !authenticated_) { stopOutputs(); status_->setText("●  Aguardando celular"); return; }
        if (mode != 1 && audioFd_ < 0) setupPulseSource();
        if (mode != 1 && audioFd_ < 0) {
            stopOutputs();
            QMessageBox::warning(this, "Microfone virtual indisponível", "Não consegui criar a entrada de áudio AndreyCam. Verifique se o PulseAudio está ativo.");
            return;
        }
        if (phone_ && authenticated_) phone_->write(QByteArray("START ") + QByteArray::number(mode) + "\n");
        streaming_ = true; start_->setEnabled(true); start_->setText("Parar conexão"); status_->setText("●  Transmitindo");
    }

    void acceptPhone() {
        while (server_->hasPendingConnections()) {
            auto *next = server_->nextPendingConnection();
            if (phone_) { next->disconnectFromHost(); next->deleteLater(); continue; }
            const QString peer = next->peerAddress().toString();
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - failureWindow_.value(peer, now) > 60000) { failureWindow_.remove(peer); authFailures_.remove(peer); }
            if (authFailures_.value(peer) >= 5) { next->disconnectFromHost(); next->deleteLater(); continue; }
            peer_ = peer;
            phone_ = next; authenticated_ = false; rx_.clear();
            connect(phone_, &QTcpSocket::readyRead, this, &AndreyCam::readPhone);
            connect(phone_, &QTcpSocket::disconnected, this, [this] {
                phone_->deleteLater(); phone_ = nullptr; authenticated_ = streaming_ = false;
                start_->setEnabled(true); start_->setText("Iniciar conexão"); status_->setText("●  Aguardando celular");
                stopOutputs();
            });
            status_->setText("●  Validando celular…");
            QTimer::singleShot(15000, this, [this, next] {
                if (phone_ == next && !authenticated_) next->disconnectFromHost();
            });
        }
    }

    void readPhone() {
        if (!phone_) return;
        rx_ += phone_->readAll();
        if (!authenticated_) {
            const int end = rx_.indexOf('\n');
            if (end < 0) { if (rx_.size() > 96) phone_->disconnectFromHost(); return; }
            const QByteArray hello = rx_.left(end).trimmed();
            rx_.remove(0, end + 1);
            if (hello != QByteArray("ANDREYCAM/1 ") + token_.toUtf8()) {
                if (!failureWindow_.contains(peer_)) failureWindow_[peer_] = QDateTime::currentMSecsSinceEpoch();
                authFailures_[peer_] = authFailures_.value(peer_) + 1;
                phone_->write("AUTH_ERROR\n"); phone_->disconnectFromHost(); return;
            }
            authFailures_.remove(peer_); failureWindow_.remove(peer_);
            authenticated_ = true;
            phone_->write("OK\n");
            status_->setText("●  Celular conectado"); status_->setStyleSheet("color:#39e56a;font-size:15px;");
        }
        while (rx_.size() >= 5) {
            const char type = rx_[0];
            const quint32 size = (quint8(rx_[1])<<24) | (quint8(rx_[2])<<16) | (quint8(rx_[3])<<8) | quint8(rx_[4]);
            if (size > 12*1024*1024) { phone_->disconnectFromHost(); return; }
            if (rx_.size() < 5 + qsizetype(size)) return;
            QByteArray payload = rx_.mid(5, size); rx_.remove(0, 5 + size);
            if (type == 'V') receiveVideo(payload);
            else if (type == 'A') receiveAudio(payload);
        }
    }

    void receiveVideo(const QByteArray &jpeg) {
        QImage image; image.loadFromData(jpeg, "JPEG");
        if (image.isNull()) return;
        preview_->setPixmap(QPixmap::fromImage(image).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        preview_->setText(QString());
        if (!streaming_ || modes_->checkedId() == 0) return;
        if (ffmpeg_ && ffmpeg_->state() == QProcess::Running) ffmpeg_->write(jpeg);
        currentFrame_ = jpeg;
    }

    void receiveAudio(const QByteArray &pcm) {
        if (!streaming_ || modes_->checkedId() == 1 || audioFd_ < 0) return;
        const char *p = pcm.constData(); qsizetype left = pcm.size();
        while (left > 0) {
            const ssize_t n = ::write(audioFd_, p, size_t(left));
            if (n > 0) { p += n; left -= n; }
            else if (errno == EINTR) continue;
            else break;
        }
    }

    void setupPulseSource() {
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (runtime.isEmpty() || !QProcess::startDetached("pactl", {"info"})) return;
        const QString fifo = runtime + QString("/andreycam-audio-%1.pcm").arg(QCoreApplication::applicationPid());
        ::unlink(fifo.toLocal8Bit().constData());
        if (::mkfifo(fifo.toLocal8Bit().constData(), 0600) != 0 && errno != EEXIST) return;
        const QString sourceName = QString("source_name=andreycam_%1").arg(QCoreApplication::applicationPid());
        QProcess pactl; pactl.start("pactl", {"load-module", "module-pipe-source", sourceName, "source_properties=device.description=AndreyCam", "file="+fifo, "format=s16le", "rate=48000", "channels=1"});
        if (!pactl.waitForFinished(2500) || pactl.exitCode() != 0) return;
        pulseModule_ = QString::fromUtf8(pactl.readAllStandardOutput()).trimmed();
        pulseFd_ = ::open(fifo.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        audioFd_ = ::open(fifo.toLocal8Bit().constData(), O_WRONLY | O_NONBLOCK);
    }

    bool ensureVideoOutput() {
        if (ffmpeg_ && ffmpeg_->state() == QProcess::Running) return true;
        const QString dev = QString("/dev/video%1").arg(VIDEO_DEVICE);
        auto isAndreyCam = [] {
            QFile name("/sys/class/video4linux/video42/name");
            if (!name.open(QIODevice::ReadOnly)) return false;
            return QString::fromUtf8(name.readAll()).trimmed() == "AndreyCam";
        };
        if (!QFile::exists(dev)) {
            if (moduleLoading_) return false;
            moduleLoading_ = true; start_->setEnabled(false);
            modprobe_ = new QProcess(this);
            connect(modprobe_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [this, isAndreyCam](int code, QProcess::ExitStatus) {
                moduleLoading_ = false; start_->setEnabled(true);
                if (code != 0 || !isAndreyCam() || !ensureVideoOutput()) {
                    QMessageBox::warning(this, "Câmera virtual indisponível", "O módulo v4l2loopback não foi carregado. Instale-o junto com os headers do kernel atual e tente novamente.");
                    status_->setText("●  Celular conectado"); return;
                }
                beginStream(modes_->checkedId());
            });
            connect(modprobe_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
                if (!moduleLoading_) return;
                moduleLoading_ = false; start_->setEnabled(true);
                QMessageBox::warning(this, "Câmera virtual indisponível", "Não consegui iniciar o pkexec para carregar v4l2loopback.");
            });
            modprobe_->start("pkexec", {"modprobe", "v4l2loopback", "devices=1", "video_nr=42", "card_label=AndreyCam", "exclusive_caps=1"});
            return false;
        }
        if (!isAndreyCam()) return false;
        ffmpeg_ = new QProcess(this);
        ffmpeg_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        ffmpeg_->start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-f", "image2pipe", "-vcodec", "mjpeg", "-i", "pipe:0", "-f", "v4l2", dev});
        if (!ffmpeg_->waitForStarted(2500)) { ffmpeg_->deleteLater(); ffmpeg_=nullptr; return false; }
        if (!currentFrame_.isEmpty()) ffmpeg_->write(currentFrame_);
        return true;
    }

    void stopOutputs() {
        if (ffmpeg_) { ffmpeg_->closeWriteChannel(); ffmpeg_->terminate(); if (!ffmpeg_->waitForFinished(1000)) ffmpeg_->kill(); ffmpeg_->deleteLater(); ffmpeg_=nullptr; }
        if (audioFd_ >= 0) { ::close(audioFd_); audioFd_=-1; }
        if (pulseFd_ >= 0) { ::close(pulseFd_); pulseFd_=-1; }
        if (!pulseModule_.isEmpty()) { QProcess::execute("pactl", {"unload-module", pulseModule_}); pulseModule_.clear(); }
        if (!qEnvironmentVariable("XDG_RUNTIME_DIR").isEmpty()) ::unlink((qEnvironmentVariable("XDG_RUNTIME_DIR")+QString("/andreycam-audio-%1.pcm").arg(QCoreApplication::applicationPid())).toLocal8Bit().constData());
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("AndreyCam");
    AndreyCam window; window.show();
    return app.exec();
}

