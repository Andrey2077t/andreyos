// Andrey OS - Display de volume (OSD) em C++ / Qt6 (Widgets)
//
// O desenho (barra, caixa do icone, alto-falante, slider, divisor e texto) e o Display UI original.
// Coordenadas de desenho baseadas na imagem original (1672x941) e escaladas proporcionalmente.
// Aqui ele virou o aviso na tela do volume do sistema:
//
//   andrey-volume-osd VOL [MUDO]          mostra o aviso (VOL 0-100, MUDO 0|1) e some sozinho
//   andrey-volume-osd --window [VOL]      janela comum que nao some (para testar o visual)
//   andrey-volume-osd --grab ARQ.png [VOL [MUDO]]   grava o desenho num PNG e sai (testes)
//
// Instancia unica: se ja houver um aviso aberto, o novo valor e enviado a ele (socket local) e o processo
// sai. Assim, segurar a tecla de volume so atualiza a mesma janela.
//
// Interacao (com o aviso na tela): arrastar/clicar no trilho ou girar a roda ajusta o volume; clicar no
// icone liga/desliga o mudo. Quem mexe no som de verdade e o script `andrey-volume` (set/mute --quiet).
//
// Compilar:
//   g++ -std=c++17 -O2 -fPIC -Wall -Wextra andrey_volume_osd.cpp -o andrey-volume-osd \
//       $(pkg-config --cflags --libs Qt6Widgets Qt6Network)

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QTimer>
#include <QPropertyAnimation>
#include <QProcess>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScreen>
#include <QCursor>
#include <QPixmap>
#include <QGuiApplication>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unistd.h>

class VolumeDisplay : public QWidget {
public:
    // osd = true: aviso sem moldura, sempre por cima, sem roubar o foco, que some sozinho.
    // osd = false: janela comum e persistente (--window).
    explicit VolumeDisplay(bool osd, QWidget *parent = nullptr) : QWidget(parent), m_osd(osd) {
        m_cmd = qEnvironmentVariable("ANDREY_VOLUME_CMD", "andrey-volume");
        setWindowTitle("Volume");

        m_proc = new QProcess(this);
        connect(m_proc, &QProcess::finished, this, [this] { if (m_dirty) pushToBackend(); });

        if (m_osd) {
            setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus);
            setAttribute(Qt::WA_TranslucentBackground);
            setAttribute(Qt::WA_ShowWithoutActivating);
            setFocusPolicy(Qt::NoFocus);

            QScreen *scr = QGuiApplication::primaryScreen();
            const int sw = scr ? scr->geometry().width() : 1280;
            const int w = std::clamp(int(sw * 0.30), 480, 820);
            resize(w, int(std::lround(w * kCropH / kCropW)));

            m_hide = new QTimer(this);
            m_hide->setSingleShot(true);
            m_hide->setInterval(1800);
            connect(m_hide, &QTimer::timeout, this, [this] {
                if (m_hover || m_dragging) { m_hide->start(); return; }   // mouse em cima: nao some
                m_fade->stop();
                m_fade->setStartValue(windowOpacity());
                m_fade->setEndValue(0.0);
                m_fade->start();
            });

            m_fade = new QPropertyAnimation(this, "windowOpacity", this);
            m_fade->setDuration(220);
            connect(m_fade, &QPropertyAnimation::finished, this, [this] {
                if (windowOpacity() <= 0.01) qApp->quit();
            });
        } else {
            setMinimumSize(600, int(600 * kCropH / kCropW));
            resize(1254, int(std::lround(1254 * kCropH / kCropW)));
            setFocusPolicy(Qt::StrongFocus);
        }
    }

    // So guarda e redesenha (usado por --grab e --window).
    void setState(int volume, bool muted) {
        if (m_dragging) return;                  // durante o arraste manda o que o usuario esta fazendo
        m_volume = std::clamp(volume, 0, 100);
        m_muted = muted;
        update();
    }

    // Chamado na partida e a cada novo valor que chega pelo socket: atualiza e (re)mostra o aviso.
    void apply(int volume, bool muted) {
        setState(volume, muted);
        present();
    }

private:
    // ---- espaco de design (px da imagem original) ----
    static constexpr double kW = 1672.0, kH = 941.0;
    // recorte usado pelo aviso: so a barra (com o brilho externo), sem o resto da imagem
    static constexpr double kCropX = 8, kCropY = 318, kCropW = 1656, kCropH = 284;
    static constexpr double kTrackX = 319, kTrackY = 425, kTrackW = 1036, kTrackH = 72;
    static constexpr double kKnobR = 47.5;
    // posicoes do centro do knob: 0% -> 367, 70% -> 957 (exato da imagem), 100% -> 1307
    static constexpr double kX0 = 367, kX70 = 957, kX100 = 1307;

    bool m_osd;
    int  m_volume   = 70;
    bool m_muted    = false;
    bool m_dragging = false;
    bool m_hover    = false;
    bool m_dirty    = false;
    QString m_cmd;
    QProcess *m_proc = nullptr;
    QTimer *m_hide = nullptr;
    QPropertyAnimation *m_fade = nullptr;

    double knobX(double pct) const {
        double p = pct / 100.0;
        if (p <= 0.7) return kX0 + (p / 0.7) * (kX70 - kX0);
        return kX70 + ((p - 0.7) / 0.3) * (kX100 - kX70);
    }
    int volumeFromX(double x) const {
        double p;
        if (x <= kX70) p = (x - kX0) / (kX70 - kX0) * 0.7;
        else           p = 0.7 + (x - kX70) / (kX100 - kX70) * 0.3;
        return int(std::lround(std::clamp(p, 0.0, 1.0) * 100.0));
    }

    double scaleF() const { return std::min(width() / kCropW, height() / kCropH); }
    QPointF offset() const {
        double s = scaleF();
        return QPointF((width() - kCropW * s) / 2.0, (height() - kCropH * s) / 2.0);
    }
    QPointF toDesign(const QPointF &pt) const {
        double s = scaleF();
        QPointF o = offset();
        return QPointF((pt.x() - o.x()) / s + kCropX, (pt.y() - o.y()) / s + kCropY);
    }

    // ---- ligacao com o som de verdade ----
    void pushToBackend() {       // junta varios movimentos do arraste num so comando
        if (m_proc->state() != QProcess::NotRunning) { m_dirty = true; return; }
        m_dirty = false;
        m_proc->start(m_cmd, {"set", QString::number(m_volume), "--quiet"});
    }
    void userSetVolume(int v) {
        v = std::clamp(v, 0, 100);
        m_muted = false;
        if (v != m_volume) { m_volume = v; pushToBackend(); }
        update();
        if (m_osd) m_hide->start();
    }
    void userToggleMute() {
        m_muted = !m_muted;
        QProcess::startDetached(m_cmd, {"mute", "--quiet"});
        update();
        if (m_osd) m_hide->start();
    }

    // ---- aparecer / posicionar ----
    void present() {
        if (!m_osd) return;
        m_fade->stop();
        setWindowOpacity(1.0);
        if (!isVisible()) {
            QScreen *scr = QGuiApplication::screenAt(QCursor::pos());
            if (!scr) scr = QGuiApplication::primaryScreen();
            if (scr) {
                const QRect a = scr->availableGeometry();
                move(a.center().x() - width() / 2, a.bottom() - height() - int(a.height() * 0.08));
            }
            show();
        }
        raise();
        m_hide->start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                         QPainter::SmoothPixmapTransform);
        if (!m_osd) p.fillRect(rect(), Qt::black);

        p.translate(offset());
        p.scale(scaleF(), scaleF());
        p.translate(-kCropX, -kCropY);

        drawBar(p);
        drawIconBox(p);
        drawSpeaker(p);
        drawSlider(p);
        drawDivider(p);
        drawText(p);
    }

    // ---------- barra externa (pilula) ----------
    void drawBar(QPainter &p) {
        const QRectF bar(33, 349, 1607, 222);

        // brilho externo suave
        for (int i = 6; i >= 1; --i) {
            p.setPen(QPen(QColor(200, 10, 50, 10 + (6 - i) * 3), 6 + i * 3));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(bar, 111, 111);
        }

        // preenchimento escuro
        QLinearGradient bg(0, bar.top(), 0, bar.bottom());
        bg.setColorAt(0.0, QColor(0x32, 0x00, 0x0c));
        bg.setColorAt(0.45, QColor(0x14, 0x00, 0x05));
        bg.setColorAt(1.0, QColor(0x22, 0x00, 0x08));
        p.setBrush(bg);

        // borda principal vermelha
        QLinearGradient bd(0, bar.top(), 0, bar.bottom());
        bd.setColorAt(0.0, QColor(0xf0, 0x20, 0x4c));
        bd.setColorAt(0.5, QColor(0x9a, 0x0a, 0x28));
        bd.setColorAt(1.0, QColor(0xc8, 0x10, 0x36));
        p.setPen(QPen(QBrush(bd), 6));
        p.drawRoundedRect(bar, 111, 111);

        // linha interna fininha (moldura dupla)
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0x7a, 0x0c, 0x26, 170), 2));
        p.drawRoundedRect(QRectF(43, 359, 1587, 202), 101, 101);
    }

    // ---------- caixa do icone ----------
    void drawIconBox(QPainter &p) {
        const QRectF box(57, 372, 224, 176);
        QLinearGradient g(0, box.top(), 0, box.bottom());
        g.setColorAt(0.0, QColor(0x6a, 0x0b, 0x1e));
        g.setColorAt(1.0, QColor(0x38, 0x05, 0x10));
        p.setBrush(g);
        p.setPen(QPen(QColor(0x96, 0x12, 0x2e), 2));
        p.drawRoundedRect(box, 30, 30);

        // brilho sutil no topo
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 80, 110, 50), 2));
        p.drawRoundedRect(box.adjusted(3, 3, -3, -3), 27, 27);
    }

    // ---------- icone de alto-falante ----------
    void drawSpeaker(QPainter &p) {
        QLinearGradient g(0, 408, 0, 514);
        g.setColorAt(0.0, QColor(0xff, 0xf6, 0xf8));
        g.setColorAt(1.0, QColor(0xe6, 0xd6, 0xdb));

        QPainterPath body;
        body.moveTo(106, 440);
        body.lineTo(134, 440);
        body.lineTo(171, 410);
        body.lineTo(177, 412);
        body.lineTo(177, 510);
        body.lineTo(171, 512);
        body.lineTo(134, 482);
        body.lineTo(106, 482);
        body.closeSubpath();

        p.setBrush(g);
        QPen pen(QBrush(g), 6);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPath(body);

        QPen wave(QBrush(g), 11);
        wave.setCapStyle(Qt::RoundCap);
        p.setBrush(Qt::NoBrush);

        if (m_muted || m_volume == 0) {
            // mudo: um X no lugar das ondas
            p.setPen(wave);
            p.drawLine(QPointF(204, 436), QPointF(248, 486));
            p.drawLine(QPointF(248, 436), QPointF(204, 486));
            return;
        }

        // ondas sonoras (a segunda so aparece a partir de 1/3, como nos icones de volume)
        p.setPen(wave);
        QRectF small(176 - 28, 461 - 28, 56, 56);
        p.drawArc(small, -42 * 16, 84 * 16);

        if (m_volume >= 34) {
            wave.setWidth(12);
            p.setPen(wave);
            QRectF big(176 - 62, 461 - 62, 124, 124);
            p.drawArc(big, -40 * 16, 80 * 16);
        }
    }

    // ---------- slider ----------
    void drawSlider(QPainter &p) {
        const QRectF track(kTrackX, kTrackY, kTrackW, kTrackH);
        const double kx = knobX(m_volume);

        // trilho vazio
        QLinearGradient eg(0, track.top(), 0, track.bottom());
        eg.setColorAt(0.0, QColor(0x24, 0x03, 0x0b));
        eg.setColorAt(1.0, QColor(0x14, 0x01, 0x06));
        p.setBrush(eg);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(track, 36, 36);

        // trecho preenchido (recortado no trilho arredondado); apagado/cinza quando mudo
        QPainterPath clip;
        clip.addRoundedRect(track, 36, 36);
        p.save();
        p.setClipPath(clip);
        QLinearGradient fg(0, track.top(), 0, track.bottom());
        if (m_muted) {
            fg.setColorAt(0.0, QColor(0x7a, 0x4c, 0x56));
            fg.setColorAt(0.5, QColor(0x5a, 0x34, 0x3c));
            fg.setColorAt(1.0, QColor(0x66, 0x3c, 0x45));
        } else {
            fg.setColorAt(0.0, QColor(0xe4, 0x12, 0x40));
            fg.setColorAt(0.5, QColor(0xb0, 0x0a, 0x2e));
            fg.setColorAt(1.0, QColor(0xc6, 0x0c, 0x36));
        }
        p.fillRect(QRectF(kTrackX, kTrackY, kx - kTrackX, kTrackH), fg);
        // reflexo no topo do preenchimento
        QLinearGradient hl(0, track.top(), 0, track.top() + 22);
        hl.setColorAt(0.0, QColor(255, 120, 150, 70));
        hl.setColorAt(1.0, QColor(255, 120, 150, 0));
        p.fillRect(QRectF(kTrackX, kTrackY, kx - kTrackX, 22), hl);
        p.restore();

        // borda do trilho
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0x8a, 0x0e, 0x2a), 2));
        p.drawRoundedRect(track, 36, 36);

        // sombra/brilho atras do knob
        const QPointF c(kx, kTrackY + kTrackH / 2.0);
        QRadialGradient sh(c, 80);
        sh.setColorAt(0.55, QColor(0, 0, 0, 150));
        sh.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(sh);
        p.drawEllipse(c, 80, 80);

        // anel vermelho do knob
        QLinearGradient ring(0, c.y() - 54, 0, c.y() + 54);
        if (m_muted) {
            ring.setColorAt(0.0, QColor(0x84, 0x58, 0x62));
            ring.setColorAt(1.0, QColor(0x30, 0x18, 0x1e));
        } else {
            ring.setColorAt(0.0, QColor(0xd8, 0x14, 0x40));
            ring.setColorAt(1.0, QColor(0x5a, 0x06, 0x18));
        }
        p.setBrush(ring);
        p.drawEllipse(c, kKnobR + 5.5, kKnobR + 5.5);

        // knob branco
        QRadialGradient kg(QPointF(c.x() - 10, c.y() - 14), 70);
        kg.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
        kg.setColorAt(0.7, QColor(0xe8, 0xe0, 0xe3));
        kg.setColorAt(1.0, QColor(0xcf, 0xc3, 0xc8));
        p.setBrush(kg);
        p.drawEllipse(c, kKnobR, kKnobR);
    }

    // ---------- divisor vertical ----------
    void drawDivider(QPainter &p) {
        QLinearGradient g(0, 394, 0, 528);
        g.setColorAt(0.0, QColor(0x8a, 0x10, 0x30, 120));
        g.setColorAt(0.5, QColor(0xb4, 0x14, 0x3c, 230));
        g.setColorAt(1.0, QColor(0x8a, 0x10, 0x30, 120));
        p.setPen(QPen(QBrush(g), 2));
        p.drawLine(QPointF(1398, 394), QPointF(1398, 528));
    }

    // ---------- texto "70%" (ou "Mudo") ----------
    void drawText(QPainter &p) {
        const QString s = m_muted ? QStringLiteral("Mudo") : QString::number(m_volume) + "%";
        QFont f;
        f.setFamilies({"Segoe UI", "Inter", "DejaVu Sans"});
        f.setPixelSize(80);
        f.setWeight(QFont::DemiBold);
        f.setLetterSpacing(QFont::AbsoluteSpacing, -1.0);
        // a fonte de reserva pode ser mais larga que a original: encolhe ate caber na caixa
        while (f.pixelSize() > 40 && QFontMetricsF(f).horizontalAdvance(s) > 180)
            f.setPixelSize(f.pixelSize() - 4);
        p.setFont(f);
        p.setPen(m_muted ? QColor(0xc8, 0xb4, 0xba) : QColor(0xfa, 0xf2, 0xf4));
        p.drawText(QRectF(1420, 394, 196, 134), Qt::AlignCenter, s);
    }

    // ---------- interacao ----------
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        const QPointF d = toDesign(e->position());
        const QRectF hit(kTrackX - 25, kTrackY - 30, kTrackW + 50, kTrackH + 60);
        const QRectF iconBox(57, 372, 224, 176);
        if (hit.contains(d)) {
            m_dragging = true;
            userSetVolume(volumeFromX(d.x()));
        } else if (iconBox.contains(d)) {
            userToggleMute();
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (m_dragging) userSetVolume(volumeFromX(toDesign(e->position()).x()));
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        m_dragging = false;
        if (m_osd) m_hide->start();
    }
    void wheelEvent(QWheelEvent *e) override {
        const int notches = e->angleDelta().y() / 120;
        userSetVolume(m_volume + (notches != 0 ? notches * 5 : (e->angleDelta().y() > 0 ? 1 : -1)));
    }
    void enterEvent(QEnterEvent *) override { m_hover = true; }
    void leaveEvent(QEvent *) override {
        m_hover = false;
        if (m_osd) m_hide->start();
    }
    void keyPressEvent(QKeyEvent *e) override {   // so na janela comum (o aviso nao tem foco)
        switch (e->key()) {
        case Qt::Key_Right: case Qt::Key_Up:   userSetVolume(m_volume + 1); break;
        case Qt::Key_Left:  case Qt::Key_Down: userSetVolume(m_volume - 1); break;
        case Qt::Key_M:                        userToggleMute(); break;
        default: QWidget::keyPressEvent(e);
        }
    }
};

// ---------------------------------------------------------------- partida
static void usage() {
    fputs("uso: andrey-volume-osd VOL [MUDO]\n"
          "     andrey-volume-osd --window [VOL [MUDO]]\n"
          "     andrey-volume-osd --grab ARQUIVO.png [VOL [MUDO]]\n", stderr);
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("andrey-volume-osd");

    bool window = false;
    QString grabFile;
    QList<int> nums;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        if (a == "--window") window = true;
        else if (a == "--grab" && i + 1 < args.size()) grabFile = args[++i];
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else {
            bool ok = false;
            const int n = a.toInt(&ok);
            if (!ok) { usage(); return 2; }
            nums << n;
        }
    }
    const int  vol   = nums.size() > 0 ? std::clamp(nums[0], 0, 100) : 70;
    const bool muted = nums.size() > 1 && nums[1] != 0;

    // desenha num PNG (testes: QT_QPA_PLATFORM=offscreen)
    if (!grabFile.isEmpty()) {
        VolumeDisplay w(!window);
        w.setState(vol, muted);       // nao mostra a janela: so desenha
        return w.grab().save(grabFile) ? 0 : 1;
    }

    // janela comum de teste
    if (window) {
        VolumeDisplay w(false);
        w.setState(vol, muted);
        w.show();
        return app.exec();
    }

    // ---- aviso (OSD): instancia unica por usuario ----
    const QString name = QStringLiteral("andrey-volume-osd-%1").arg(getuid());
    const QByteArray msg = QStringLiteral("%1 %2\n").arg(vol).arg(muted ? 1 : 0).toUtf8();

    QLocalServer server;
    if (!server.listen(name)) {
        QLocalSocket s;
        s.connectToServer(name);
        if (s.waitForConnected(300)) {            // ja ha um aviso aberto: so atualiza e sai
            s.write(msg);
            s.waitForBytesWritten(300);
            s.disconnectFromServer();
            return 0;
        }
        QLocalServer::removeServer(name);         // socket velho de um aviso que morreu
        server.listen(name);                      // se ainda falhar, o aviso aparece mesmo assim
    }

    VolumeDisplay w(true);
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&server, &w] {
        while (QLocalSocket *c = server.nextPendingConnection()) {
            auto pump = [&w, c] {
                while (c->canReadLine()) {
                    const QList<QByteArray> parts = c->readLine().trimmed().split(' ');
                    if (parts.size() >= 2) w.apply(parts[0].toInt(), parts[1].toInt() != 0);
                }
            };
            QObject::connect(c, &QLocalSocket::readyRead, c, pump);
            QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
            pump();                               // o que ja chegou antes de ligarmos o sinal
        }
    });

    w.apply(vol, muted);
    return app.exec();
}
