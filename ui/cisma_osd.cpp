// Cisma OS - Display de desempenho (OSD) em C++ / Qt6 (Widgets)
//
// O desenho (cabecalho com logo e relogio, CPU, GPU, RAM, armazenamento, rede e rodape com FPS) e o Display UI
// original (Cisma OSD), portado do GDI+ para o QPainter com as mesmas coordenadas de desenho. Os valores sao
// reais, lidos do Linux por ui/cisma_osd_sensors.h (/proc, /sys, sem root).
//
//   cisma-osd                    mostra o painel (flutuante, sempre por cima, sem roubar o foco)
//                                rodar de novo enquanto ele esta aberto FECHA o painel (liga/desliga)
//   cisma-osd --dump             imprime os valores lidos e sai (nao precisa de tela)
//   cisma-osd --grab ARQ.png     grava o desenho com valores de exemplo num PNG e sai (testes)
//
// Controles: arrastar com o botao esquerdo = mover (a posicao fica em ~/.config/cisma-os/osd-position)
//            botao direito = fechar. Escala da janela: variavel CISMA_OSD_SCALE (padrao 0.6).
//
// Compilar:
//   g++ -std=c++17 -O2 -fPIC -pthread -Wall -Wextra cisma_osd.cpp -o cisma-osd $(pkg-config --cflags --libs Qt6Widgets Qt6Network)

#include "cisma_osd_sensors.h"

#include <QApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QScreen>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// ---------- Canvas (coordenadas de design = pixels da imagem original) ----------
constexpr double kOX = 105.0, kOY = 5.0;       // origem do recorte
constexpr double kDW = 1065.0, kDH = 1185.0;   // tamanho do recorte
constexpr int kHist = 60;                      // pontos dos graficos (1 por segundo)

// ---------- Cores ----------
const QColor cRed(232, 0, 28);
const QColor cRedHi(255, 20, 40);
const QColor cWhite(245, 245, 245);
const QColor cGray(200, 204, 216);
const QColor cSub(215, 215, 220);
const QColor cDark(10, 4, 6);

// ---------- Utilidades de desenho ----------
QFont mkFont(int px, bool bold) {
    QFont f;
    f.setFamilies(QStringList{"Bahnschrift", "Inter", "Noto Sans", "DejaVu Sans"});
    f.setStyleHint(QFont::SansSerif);
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

double strW(const QString &t, int size, bool bold) {
    return QFontMetricsF(mkFont(size, bold)).horizontalAdvance(t);
}

// (x, y) = canto superior do texto; alinhamento Right/HCenter usam x como borda direita/centro; maxW > 0 corta com "..."
void str(QPainter &p, const QString &t, double x, double y, int size, const QColor &col, bool bold = false,
         Qt::Alignment al = Qt::AlignLeft, double maxW = 0) {
    const QFont f = mkFont(size, bold);
    const QFontMetricsF fm(f);
    const QString s = maxW > 0 ? fm.elidedText(t, Qt::ElideRight, maxW) : t;
    const double w = fm.horizontalAdvance(s);
    double px = x;
    if (al & Qt::AlignRight) px = x - w;
    else if (al & Qt::AlignHCenter) px = x - w / 2;
    p.setFont(f);
    p.setPen(col);
    p.drawText(QPointF(px, y + fm.ascent()), s);
}

QPen mkPen(const QColor &c, double w, Qt::PenCapStyle cap = Qt::FlatCap) {
    QPen p(c, w);
    p.setCapStyle(cap);
    p.setJoinStyle(Qt::MiterJoin);
    return p;
}

void line(QPainter &p, const QPen &pn, double x1, double y1, double x2, double y2) {
    p.setPen(pn);
    p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
}

void strokeRect(QPainter &p, const QPen &pn, double x, double y, double w, double h) {
    p.setPen(pn);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(x, y, w, h));
}

void ellipse(QPainter &p, const QPen &pn, double x, double y, double w, double h) {
    p.setPen(pn);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(x, y, w, h));
}

void fillEllipse(QPainter &p, const QColor &c, double x, double y, double w, double h) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(x, y, w, h));
}

void fillRect(QPainter &p, const QColor &c, double x, double y, double w, double h) {
    p.fillRect(QRectF(x, y, w, h), c);
}

void fillPoly(QPainter &p, const QBrush &b, const QPolygonF &poly) {
    p.setPen(Qt::NoPen);
    p.setBrush(b);
    p.drawPolygon(poly);
}

QPolygonF poly(std::initializer_list<QPointF> pts) { return QPolygonF(QList<QPointF>(pts)); }

QPainterPath polyPath(const QPolygonF &pg) {
    QPainterPath path;
    path.addPolygon(pg);
    path.closeSubpath();
    return path;
}

// Contorno com brilho (glow)
void glowPath(QPainter &p, const QPainterPath &path, double w) {
    p.setBrush(Qt::NoBrush);
    for (int i = 4; i >= 1; --i) {
        QPen pg(QColor(255, 0, 30, 28), w + i * 5.0);
        pg.setJoinStyle(Qt::RoundJoin);
        p.setPen(pg);
        p.drawPath(path);
    }
    QPen pr(cRed, w);
    pr.setJoinStyle(Qt::MiterJoin);
    p.setPen(pr);
    p.drawPath(path);
}

// ---------- Icones (desenhados em torno de cx, cy) ----------
void iconCPU(QPainter &p, double cx, double cy) {
    const QPen pn = mkPen(cRed, 5);
    for (int i = 0; i < 5; ++i) {   // pinos
        const double o = -34.0 + i * 17.0;
        line(p, pn, cx + o, cy - 48, cx + o, cy - 38);
        line(p, pn, cx + o, cy + 38, cx + o, cy + 48);
        line(p, pn, cx - 48, cy + o, cx - 38, cy + o);
        line(p, pn, cx + 38, cy + o, cx + 48, cy + o);
    }
    strokeRect(p, pn, cx - 36, cy - 36, 72, 72);
    strokeRect(p, pn, cx - 22, cy - 22, 44, 44);
    const QPen thin = mkPen(cRed, 4);
    strokeRect(p, thin, cx - 10, cy - 10, 20, 20);
    line(p, thin, cx, cy - 10, cx, cy + 10);
    line(p, thin, cx - 10, cy, cx + 10, cy);
}

void iconGPU(QPainter &p, double cx, double cy) {
    const QPen pn = mkPen(cRed, 5);
    line(p, pn, cx - 48, cy - 28, cx - 48, cy + 34);   // bracket
    strokeRect(p, pn, cx - 40, cy - 20, 88, 42);             // placa
    for (int i = 0; i < 2; ++i) {
        const double fx = cx - 14 + i * 40.0;
        ellipse(p, pn, fx - 15, cy - 15, 30, 30);
        fillEllipse(p, cRed, fx - 5, cy - 5, 10, 10);
        line(p, pn, fx - 10, cy - 10, fx + 10, cy + 10);
    }
    line(p, pn, cx - 30, cy + 32, cx - 6, cy + 32);
    line(p, pn, cx + 6, cy + 32, cx + 36, cy + 32);
}

void iconRAM(QPainter &p, double cx, double cy) {
    const QPen pn = mkPen(cRed, 5);
    strokeRect(p, pn, cx - 48, cy - 16, 96, 30);
    for (int i = 0; i < 3; ++i) fillRect(p, cRed, cx - 34 + i * 26.0, cy - 8, 18, 14);
    for (int i = 0; i < 7; ++i) fillRect(p, cRed, cx - 44 + i * 14.0, cy + 16, 8, 8);
    line(p, pn, cx - 48, cy + 14, cx + 48, cy + 14);
}

void iconDisk(QPainter &p, double cx, double cy) {
    QPainterPath body;   // (angulos do Qt: anti-horario; no GDI+ eram horarios)
    body.arcMoveTo(QRectF(cx - 36, cy - 36, 20, 20), 180);
    body.arcTo(QRectF(cx - 36, cy - 36, 20, 20), 180, -90);
    body.arcTo(QRectF(cx + 16, cy - 36, 20, 20), 90, -90);
    body.lineTo(cx + 36, cy - 26);
    body.lineTo(cx + 36, cy + 8);
    body.lineTo(cx - 36, cy + 8);
    body.lineTo(cx - 36, cy - 26);
    body.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(cRed);
    p.drawPath(body);
    fillEllipse(p, cDark, cx - 22, cy - 26, 44, 26);
    fillEllipse(p, cRed, cx - 11, cy - 20, 22, 14);
    fillRect(p, cRed, cx - 36, cy + 14, 72, 22);
    fillRect(p, cDark, cx + 18, cy + 21, 10, 8);
    fillRect(p, cDark, cx - 28, cy + 21, 30, 4);
}

void iconWifi(QPainter &p, double cx, double cy) {
    const QPen pn = mkPen(cRed, 7, Qt::RoundCap);
    const double rs[3] = {22.0, 42.0, 62.0};
    p.setPen(pn);
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 3; ++i)
        p.drawArc(QRectF(cx - rs[i], cy + 24 - rs[i], rs[i] * 2, rs[i] * 2), 135 * 16, -90 * 16);
    fillEllipse(p, cRed, cx - 7, cy + 17, 14, 14);
    fillRect(p, cRed, cx - 4, cy + 28, 8, 18);
}

void iconGamepad(QPainter &p, double cx, double cy) {
    QPainterPath ears;
    ears.addEllipse(QRectF(cx - 40, cy - 6, 34, 40));
    ears.addEllipse(QRectF(cx + 6, cy - 6, 34, 40));
    p.fillPath(ears, cRed);
    QPainterPath b;
    b.arcMoveTo(QRectF(cx - 36, cy - 26, 30, 30), 180);
    b.arcTo(QRectF(cx - 36, cy - 26, 30, 30), 180, -90);
    b.arcTo(QRectF(cx + 6, cy - 26, 30, 30), 90, -90);
    b.lineTo(cx + 36, cy + 4);
    b.lineTo(cx + 30, cy + 20);
    b.lineTo(cx - 30, cy + 20);
    b.lineTo(cx - 36, cy + 4);
    b.closeSubpath();
    p.fillPath(b, cRed);
    fillRect(p, cRed, cx - 30, cy - 4, 60, 24);
    fillRect(p, cDark, cx - 24, cy - 2, 18, 6);    // dpad
    fillRect(p, cDark, cx - 18, cy - 8, 6, 18);
    fillEllipse(p, cDark, cx + 10, cy - 8, 7, 7);  // botoes
    fillEllipse(p, cDark, cx + 20, cy + 1, 7, 7);
}

void iconThermo(QPainter &p, double cx, double cy) {
    QPainterPath t;
    t.arcMoveTo(QRectF(cx - 9, cy - 42, 18, 18), 180);
    t.arcTo(QRectF(cx - 9, cy - 42, 18, 18), 180, -180);
    t.lineTo(cx + 9, cy + 8);
    t.arcTo(QRectF(cx - 22, cy + 4, 44, 44), 60, -300);
    t.lineTo(cx - 9, cy + 8);
    t.lineTo(cx - 9, cy - 33);
    t.closeSubpath();
    p.setPen(mkPen(cRed, 5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(t);
    fillEllipse(p, cRed, cx - 13, cy + 14, 26, 26);
    fillRect(p, cRed, cx - 4, cy - 20, 8, 38);
    for (int i = 0; i < 4; ++i) fillRect(p, cRed, cx + 20, cy - 36 + i * 9.0, i % 2 ? 6 : 9, 3);
    line(p, mkPen(cRed, 5), cx - 26, cy + 52, cx + 26, cy + 52);
}

void iconBolt(QPainter &p, double cx, double cy) {
    fillPoly(p, cRed,
             poly({{cx + 8, cy - 44}, {cx + 34, cy - 44}, {cx + 18, cy - 14}, {cx + 38, cy - 14},
                   {cx - 22, cy + 38}, {cx - 6, cy + 4}, {cx - 28, cy + 4}}));
    fillRect(p, cRed, cx - 26, cy + 44, 44, 5);
}

// ---------- Graficos ----------
void graph(QPainter &p, const std::vector<float> &h, double gx, double gy, double gw, double gh) {
    const int n = int(h.size());
    if (n < 2) return;
    QList<QPointF> pts;
    pts.append(QPointF(gx, gy + gh));
    for (int i = 0; i < n; ++i)
        pts.append(QPointF(gx + gw * i / (n - 1), gy + gh - double(std::clamp(h[size_t(i)], 0.f, 1.f)) * gh));
    pts.append(QPointF(gx + gw, gy + gh));
    QLinearGradient lg(QPointF(0, gy), QPointF(0, gy + gh));
    lg.setColorAt(0, QColor(190, 0, 20, 170));
    lg.setColorAt(1, QColor(40, 0, 6, 230));
    fillPoly(p, QBrush(lg), QPolygonF(pts));
    QPen ln(cRedHi, 3);
    ln.setJoinStyle(Qt::RoundJoin);
    p.setPen(ln);
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(pts.constData() + 1, n);
}

void bar(QPainter &p, double x, double y, double w, double h, double frac) {
    frac = std::clamp(frac, 0.0, 1.0);
    fillRect(p, QColor(40, 6, 10), x, y, w, h);
    strokeRect(p, mkPen(QColor(70, 8, 14), 2), x, y, w, h);
    if (frac <= 0) return;
    QLinearGradient lg(QPointF(x, 0), QPointF(x + w * frac, 0));
    lg.setColorAt(0, QColor(200, 0, 22));
    lg.setColorAt(1, QColor(255, 12, 40));
    p.fillRect(QRectF(x, y, w * frac, h), QBrush(lg));
}

void divider(QPainter &p, double y) { line(p, mkPen(QColor(150, 0, 20), 2), 168, y, 1118, y); }

// ---------- Formatacao ----------
QString pct(double frac) { return QStringLiteral("%1%").arg(int(std::lround(std::clamp(frac, 0.0, 1.0) * 100))); }
QString gb(double bytes, int prec) { return QString::number(bytes / 1073741824.0, 'f', prec); }
QString degrees(double c) { return c >= 0 ? QStringLiteral("%1°C").arg(int(std::lround(c))) : QStringLiteral("--"); }

QString rate(double bps) {
    const double kb = bps / 1024.0;
    return kb < 1000.0 ? QString::number(kb, 'f', 1) + QStringLiteral(" KB/s")
                       : QString::number(kb / 1024.0, 'f', 2) + QStringLiteral(" MB/s");
}

// pseudo-aleatorio simples e deterministico (historicos de exemplo do --grab)
float demoRand(unsigned &seed) {
    seed = seed * 1664525u + 1013904223u;
    return float((seed >> 8) & 0xFFFF) / 65535.0f;
}

}  // namespace

// =========================================================================
class PerfDisplay : public QWidget {
public:
    explicit PerfDisplay(bool demo, QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("Cisma OSD");
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);

        bool ok = false;
        const double sc = qEnvironmentVariable("CISMA_OSD_SCALE").toDouble(&ok);
        m_scale = ok ? std::clamp(sc, 0.3, 2.0) : 0.6;
        setFixedSize(int(kDW * m_scale + 0.5), int(kDH * m_scale + 0.5));

        m_cpu.assign(kHist, 0.f);
        m_gpu.assign(kHist, 0.f);
        m_net.assign(kHist, 0.f);

        if (demo) {
            m_s.loadDemo();
            unsigned seed = 7;
            float a = 0.35f, b = 0.45f;
            for (int i = 0; i < kHist; ++i) {
                a = std::clamp(a + (demoRand(seed) - 0.5f) * 0.3f, 0.15f, 0.9f);
                b = std::clamp(b + (demoRand(seed) - 0.5f) * 0.3f, 0.15f, 0.9f);
                m_cpu[size_t(i)] = a;
                m_gpu[size_t(i)] = b;
                m_net[size_t(i)] = demoRand(seed) < 0.06f ? 600.f * 1024 : demoRand(seed) * 40.f * 1024;
            }
        } else {
            m_s.init();
            restorePosition();
            m_timer.setInterval(1000);
            connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
            m_timer.start();
            tick();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.scale(m_scale, m_scale);
        p.translate(-kOX, -kOY);
        draw(p);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        m_dragging = true;
        m_dragOff = e->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (m_dragging) move(e->globalPosition().toPoint() - m_dragOff);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::RightButton) { QApplication::quit(); return; }
        if (m_dragging) { m_dragging = false; savePosition(); }
    }

private:
    osd::Sensors m_s;
    double m_scale = 0.6;
    std::vector<float> m_cpu, m_gpu, m_net;   // historicos: CPU/GPU 0..1, rede em bytes/s
    QTimer m_timer;
    bool m_dragging = false;
    QPoint m_dragOff;

    static void push(std::vector<float> &v, float x) {
        v.erase(v.begin());
        v.push_back(x);
    }

    void tick() {
        m_s.update();
        push(m_cpu, float(m_s.cpuUse));
        push(m_gpu, m_s.gpuUse < 0 ? 0.f : float(m_s.gpuUse));
        push(m_net, float(m_s.netDown + m_s.netUp));
        update();
    }

    static QString posFile() {
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/cisma-os/osd-position";
    }

    void restorePosition() {
        QFile f(posFile());
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QList<QByteArray> parts = f.readLine().trimmed().split(' ');
            if (parts.size() >= 2) {
                bool okx = false, oky = false;
                const int x = parts[0].toInt(&okx), y = parts[1].toInt(&oky);
                if (okx && oky && QGuiApplication::screenAt(QPoint(x + 50, y + 50))) {
                    move(x, y);
                    return;
                }
            }
        }
        const QScreen *sc = QGuiApplication::primaryScreen();
        const QRect g = sc ? sc->availableGeometry() : QRect(0, 0, 1280, 720);
        move(g.left() + 60, g.top() + 60);
    }

    void savePosition() const {
        QDir().mkpath(QFileInfo(posFile()).absolutePath());
        QFile f(posFile());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            f.write(QByteArray::number(x()) + ' ' + QByteArray::number(y()) + '\n');
    }

    // ---------- desenho principal ----------
    void draw(QPainter &p) const {
        const osd::Sensors &s = m_s;

        // ===== Formas =====
        const QPolygonF header = poly({{128, 75}, {165, 18}, {305, 18}, {335, 42}, {690, 42}, {706, 24}, {1148, 26},
                                       {1148, 150}, {1126, 172}, {870, 172}, {860, 182}, {640, 182}, {626, 172},
                                       {165, 172}, {128, 140}});
        const QPolygonF body = poly({{135, 176}, {1140, 176}, {1152, 345}, {1138, 500}, {1138, 960}, {1152, 980},
                                     {118, 980}, {135, 960}, {135, 500}, {125, 345}});
        const QPolygonF footer = poly({{118, 985}, {1155, 985}, {1155, 1088}, {1136, 1106}, {1136, 1128}, {1068, 1168},
                                       {205, 1168}, {135, 1128}, {135, 1106}, {116, 1088}});
        const QPainterPath pH = polyPath(header), pB = polyPath(body), pF = polyPath(footer);

        QLinearGradient bgG(QPointF(0, 20), QPointF(0, 1170));
        bgG.setColorAt(0, QColor(18, 4, 8));
        bgG.setColorAt(1, QColor(6, 3, 4));
        p.setPen(Qt::NoPen);
        p.setBrush(QBrush(bgG));
        p.drawPath(pB);
        p.drawPath(pH);
        p.drawPath(pF);
        glowPath(p, pB, 3);
        glowPath(p, pH, 4);
        glowPath(p, pF, 4);

        // ===== Cabecalho =====
        {   // logo "C"
            QLinearGradient lg(QPointF(160, 40), QPointF(300, 150));
            lg.setColorAt(0, QColor(255, 20, 40));
            lg.setColorAt(1, QColor(190, 0, 18));
            fillPoly(p, QBrush(lg),
                     poly({{200, 40}, {300, 40}, {282, 70}, {222, 70}, {212, 82}, {212, 108}, {222, 120}, {282, 120},
                           {300, 148}, {200, 148}, {160, 122}, {160, 66}}));
            line(p, mkPen(QColor(120, 0, 12), 3), 226, 94, 290, 94);   // sulco horizontal fino
        }
        str(p, QStringLiteral("Cisma "), 342, 58, 64, cWhite, true);
        str(p, QStringLiteral("OSD"), 342 + strW(QStringLiteral("Cisma "), 64, true), 58, 64, cRed, true);
        // faixas diagonais
        fillPoly(p, QColor(120, 0, 12, 110), poly({{738, 50}, {760, 50}, {700, 140}, {678, 140}}));
        line(p, mkPen(cRed, 4), 812, 34, 720, 158);
        fillPoly(p, QColor(150, 0, 16, 120), poly({{705, 38}, {750, 38}, {745, 52}, {700, 52}}));
        // hora e data
        str(p, QTime::currentTime().toString("HH:mm:ss"), 1115, 36, 64, cWhite, true, Qt::AlignRight);
        str(p, QDate::currentDate().toString("dd/MM/yyyy"), 1115, 104, 44, cGray, false, Qt::AlignRight);

        // ===== CPU =====
        iconCPU(p, 224, 245);
        str(p, QStringLiteral("CPU"), 313, 196, 40, cRed, true);
        str(p, QString::fromStdString(s.cpuName), 313, 238, 32, cSub, false, Qt::AlignLeft, 600);
        str(p, pct(s.cpuUse), 313, 278, 66, cWhite, true);
        graph(p, m_cpu, 472, 276, 466, 80);
        str(p, s.cpuMHz > 0 ? QString::number(s.cpuMHz / 1000.0, 'f', 2) + QStringLiteral(" GHz") : QStringLiteral("--"),
            1113, 226, 36, cSub, false, Qt::AlignRight);
        str(p, degrees(s.cpuTemp), 1113, 288, 44, cWhite, true, Qt::AlignRight);
        divider(p, 368);

        // ===== GPU =====
        iconGPU(p, 224, 422);
        str(p, QStringLiteral("GPU"), 315, 382, 40, cRed, true);
        str(p, QString::fromStdString(s.gpuName), 315, 423, 32, cSub, false, Qt::AlignLeft, 600);
        str(p, s.gpuUse >= 0 ? pct(s.gpuUse) : QStringLiteral("--"), 315, 463, 66, cWhite, true);
        graph(p, m_gpu, 472, 460, 466, 80);
        str(p, s.gpuMHz > 0 ? QString::number(int(std::lround(s.gpuMHz))) + QStringLiteral(" MHz") : QStringLiteral("--"),
            1113, 412, 36, cSub, false, Qt::AlignRight);
        str(p, degrees(s.gpuTemp), 1113, 474, 44, cWhite, true, Qt::AlignRight);
        divider(p, 550);

        // ===== RAM =====
        const double ramFrac = s.ramTotal > 0 ? s.ramUsed / s.ramTotal : 0;
        iconRAM(p, 224, 598);
        str(p, QStringLiteral("RAM"), 316, 563, 38, cRed, true);
        str(p, gb(s.ramTotal, 1) + QStringLiteral(" GB"), 316, 603, 30, cSub);
        str(p, pct(ramFrac), 316, 637, 58, cWhite, true);
        str(p, gb(s.ramUsed, 2) + QStringLiteral(" GB / ") + gb(s.ramTotal, 1) + QStringLiteral(" GB"), 1113, 620, 30,
            cSub, false, Qt::AlignRight);
        bar(p, 478, 662, 632, 26, ramFrac);
        divider(p, 700);

        // ===== Armazenamento =====
        const double diskFrac = s.diskTotal > 0 ? s.diskUsed / s.diskTotal : 0;
        iconDisk(p, 224, 752);
        str(p, QStringLiteral("Armazenamento"), 316, 714, 38, cRed, true);
        str(p, QString::fromStdString(s.diskLabel), 316, 752, 30, cSub);
        str(p, pct(diskFrac), 316, 785, 58, cWhite, true);
        str(p, gb(s.diskUsed, 1) + QStringLiteral(" GB / ") + gb(s.diskTotal, 1) + QStringLiteral(" GB"), 1113, 763, 30,
            cSub, false, Qt::AlignRight);
        bar(p, 482, 805, 628, 26, diskFrac);
        divider(p, 847);

        // ===== Rede =====
        iconWifi(p, 224, 885);
        str(p, QStringLiteral("Rede"), 316, 860, 36, cRed, true);
        str(p, QStringLiteral("Download: ") + rate(s.netDown) + QStringLiteral("  |  Upload: ") + rate(s.netUp), 316,
            894, 32, cSub);
        {
            float peak = 100.f * 1024.f;   // escala minima: 100 KB/s
            for (const float v : m_net) peak = std::max(peak, v);
            std::vector<float> norm;
            norm.reserve(m_net.size());
            for (const float v : m_net) norm.push_back(v / peak);
            graph(p, norm, 310, 925, 803, 40);
            line(p, mkPen(QColor(150, 0, 20), 2), 310, 966, 1113, 966);
        }

        // ===== Rodape =====
        const QString fpsText = s.fps >= 0 ? QString::number(int(std::lround(s.fps))) : QStringLiteral("--");
        iconGamepad(p, 195, 1040);
        str(p, QStringLiteral("FPS"), 255, 1008, 32, cRed, true);
        str(p, fpsText, 255, 1040, 58, cWhite, true);
        str(p, QStringLiteral(" FPS"), 255 + strW(fpsText, 58, true), 1066, 28, cWhite, true);
        const QPen sep = mkPen(QColor(110, 0, 16), 2);
        line(p, sep, 465, 1008, 465, 1102);
        line(p, sep, 817, 1008, 817, 1102);

        iconThermo(p, 520, 1048);
        str(p, QStringLiteral("Temp. GPU"), 588, 1008, 32, cRed, true);
        str(p, degrees(s.gpuTemp), 588, 1040, 58, cWhite, true);

        iconBolt(p, 893, 1045);
        str(p, QStringLiteral("Uso da GPU"), 953, 1008, 32, cRed, true);
        str(p, s.gpuUse >= 0 ? pct(s.gpuUse) : QStringLiteral("--"), 953, 1040, 58, cWhite, true);

        // detalhes do rodape
        const QColor r(190, 0, 20);
        fillPoly(p, r, poly({{195, 1124}, {215, 1124}, {232, 1156}, {212, 1156}}));
        fillPoly(p, r, poly({{222, 1124}, {242, 1124}, {259, 1156}, {239, 1156}}));
        fillPoly(p, r, poly({{1000, 1156}, {1020, 1130}, {1036, 1130}, {1016, 1156}}));
        fillPoly(p, r, poly({{1022, 1156}, {1042, 1130}, {1058, 1130}, {1038, 1156}}));
        line(p, mkPen(QColor(150, 0, 20), 2), 200, 1118, 1070, 1118);
        str(p, QStringLiteral("Cisma OSD - Desempenho em tempo real"), 637, 1128, 27, cRed, false, Qt::AlignHCenter);
    }
};

// ---------------------------------------------------------------- partida
static void usage() {
    std::fputs("uso: cisma-osd                   mostra o painel (rodar de novo fecha)\n"
               "     cisma-osd --dump            imprime os valores lidos e sai\n"
               "     cisma-osd --grab ARQ.png    grava o desenho (valores de exemplo) num PNG e sai\n",
               stderr);
}

int main(int argc, char *argv[]) {
    // --dump e --help nao precisam de tela
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "--dump") {
            osd::Sensors s;
            s.init();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            s.update();
            osd::dump(s);
            return 0;
        }
    }

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("cisma-osd");

    QString grabFile;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--grab" && i + 1 < args.size()) grabFile = args[++i];
        else { usage(); return 2; }
    }

    // desenha num PNG (testes: QT_QPA_PLATFORM=offscreen)
    if (!grabFile.isEmpty()) {
        PerfDisplay w(true);
        return w.grab().save(grabFile) ? 0 : 1;
    }

    // ---- instancia unica por usuario: se ja ha um painel aberto, manda ele fechar (liga/desliga) ----
    const QString name = QStringLiteral("cisma-osd-%1").arg(getuid());
    QLocalServer server;
    if (!server.listen(name)) {
        QLocalSocket sock;
        sock.connectToServer(name);
        if (sock.waitForConnected(300)) {
            sock.write("quit\n");
            sock.waitForBytesWritten(300);
            sock.disconnectFromServer();
            return 0;
        }
        QLocalServer::removeServer(name);   // socket velho de um painel que morreu
        server.listen(name);                // se ainda falhar, o painel aparece mesmo assim
    }
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&server] {
        while (QLocalSocket *c = server.nextPendingConnection()) {
            QObject::connect(c, &QLocalSocket::readyRead, c, [c] {
                while (c->canReadLine())
                    if (c->readLine().trimmed() == "quit") QApplication::quit();
            });
            QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
        }
    });

    PerfDisplay w(false);
    w.show();
    return app.exec();
}
