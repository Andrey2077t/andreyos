// Cisma Taskbar - Display UI (C++ / Qt 6 Widgets)
// Janela "Escolha o estilo da sua barra de tarefas" do Cisma OS.
// Tudo desenhado via QPainter em coordenadas de design (1496x728) e escalado.

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QWindow>
#include <QFontMetricsF>
#include <QTransform>
#include <QProcess>
#include <QMessageBox>
#include <cmath>

namespace {

constexpr double kCanvasW = 1496.0;
constexpr double kCanvasH = 728.0;
constexpr double kMargin  = 20.0;   // espaço para o brilho da borda

const QRectF kWin(0, 0, 1456, 688);
const QRectF kPanel(14, 173, 1428, 394);
const QRectF kOpt1(60, 213, 1337, 149);
const QRectF kOpt2(60, 382, 1337, 138);
const QRectF kBar1(462, 235, 910, 110);
const QRectF kBar2(467, 400, 905, 103);
const QRectF kMinBtn(1245, 18, 64, 64);
const QRectF kCloseBtn(1353, 20, 64, 64);
constexpr double kHeaderH = 172.0;

const QColor kRed(232, 0, 28);
const QColor kRedHi(255, 45, 65);

// ---------- helpers ----------
void glowPath(QPainter &p, const QPainterPath &path, QColor c, double w, int layers = 7, double a = 0.06) {
    p.setBrush(Qt::NoBrush);
    for (int i = layers; i >= 1; --i) {
        QColor g = c;
        g.setAlphaF(a);
        QPen pen(g, w + i * 3.2);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPath(path);
    }
    QPen pen(c, w);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.drawPath(path);
}

QPainterPath rounded(const QRectF &r, double rad) {
    QPainterPath path;
    path.addRoundedRect(r, rad, rad);
    return path;
}

QFont makeFont(double px, QFont::Weight w) {
    QFont f;
    f.setFamilies({"Montserrat", "Segoe UI", "Inter", "Helvetica Neue", "Arial"});
    f.setPixelSize(int(px));
    f.setWeight(w);
    return f;
}

void drawTextV(QPainter &p, double x, double cy, const QString &t, double px,
               QFont::Weight w, const QColor &c) {
    p.setFont(makeFont(px, w));
    p.setPen(c);
    p.drawText(QRectF(x, cy - px, 1200, px * 2), Qt::AlignLeft | Qt::AlignVCenter, t);
}

// ---------- ícones ----------
void drawLogo(QPainter &p, QPointF c, double size) {
    p.save();
    p.translate(c);
    QLinearGradient g(-size / 2, -size / 2, size / 2, size / 2);
    g.setColorAt(0, QColor(255, 40, 55));
    g.setColorAt(1, QColor(170, 0, 20));

    // letra C
    QPen pen(QBrush(g), size * 0.30, Qt::SolidLine, Qt::FlatCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QRectF r(-size * 0.33, -size * 0.36, size * 0.66, size * 0.72);
    p.drawArc(r, 50 * 16, 265 * 16);

    // órbita
    p.rotate(-24);
    QPen ring(QColor(235, 10, 35), size * 0.045);
    p.setPen(ring);
    p.drawEllipse(QPointF(size * 0.04, size * 0.12), size * 0.52, size * 0.17);
    p.restore();
}

void drawGear(QPainter &p, QPointF c, double R) {
    QPainterPath gear;
    gear.setFillRule(Qt::WindingFill);
    for (int i = 0; i < 8; ++i) {
        QTransform t;
        t.rotate(i * 45.0);
        gear.addPolygon(t.map(QPolygonF(QRectF(-R * 0.21, -R, R * 0.42, R * 0.5))));
    }
    gear.addEllipse(QPointF(0, 0), R * 0.80, R * 0.80);
    QPainterPath hole;
    hole.addEllipse(QPointF(0, 0), R * 0.33, R * 0.33);
    gear = gear.subtracted(hole);

    p.save();
    p.translate(c);
    QLinearGradient g(0, -R, 0, R);
    g.setColorAt(0, QColor(230, 230, 232));
    g.setColorAt(1, QColor(125, 125, 130));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawPath(gear);
    p.restore();
}

void drawTrash(QPainter &p, QPointF c, double k) {
    p.save();
    p.translate(c);
    p.scale(k, k);
    QLinearGradient g(0, -40, 0, 40);
    g.setColorAt(0, QColor(255, 35, 50));
    g.setColorAt(1, QColor(180, 0, 18));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    // alça
    QPen h(QColor(235, 10, 35), 6);
    h.setCapStyle(Qt::RoundCap);
    p.setPen(h);
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(-12, -38), QPointF(12, -38));
    p.drawLine(QPointF(-12, -38), QPointF(-12, -32));
    p.drawLine(QPointF(12, -38), QPointF(12, -32));
    // tampa
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(QRectF(-36, -33, 72, 11), 5, 5);
    // corpo
    QPainterPath body;
    body.moveTo(-29, -17);
    body.lineTo(29, -17);
    body.lineTo(24, 36);
    body.quadTo(23, 42, 17, 42);
    body.lineTo(-17, 42);
    body.quadTo(-23, 42, -24, 36);
    body.closeSubpath();
    p.drawPath(body);
    // frestas
    QPen s(QColor(70, 0, 10), 6);
    s.setCapStyle(Qt::RoundCap);
    p.setPen(s);
    for (int x : {-12, 0, 12}) p.drawLine(QPointF(x, -6), QPointF(x, 30));
    p.restore();
}

void drawFolder(QPainter &p, QPointF c, double k, QColor top, QColor bottom) {
    p.save();
    p.translate(c);
    p.scale(k, k);
    QLinearGradient g(0, -34, 0, 30);
    g.setColorAt(0, top);
    g.setColorAt(1, bottom);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(QRectF(-39, -34, 34, 20), 6, 6);   // aba
    p.drawRoundedRect(QRectF(-39, -23, 78, 54), 7, 7);   // corpo
    p.restore();
}

void drawChrome(QPainter &p, QPointF c, double R) {
    p.save();
    p.translate(c);
    QRectF r(-R, -R, 2 * R, 2 * R);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(219, 68, 55));  p.drawPie(r, 30 * 16, 120 * 16);   // vermelho
    p.setBrush(QColor(15, 157, 88));  p.drawPie(r, 150 * 16, 120 * 16);  // verde
    p.setBrush(QColor(244, 180, 0));  p.drawPie(r, 270 * 16, 120 * 16);  // amarelo
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(0, 0), R * 0.47, R * 0.47);
    p.setBrush(QColor(66, 133, 244));
    p.drawEllipse(QPointF(0, 0), R * 0.36, R * 0.36);
    p.restore();
}

void drawTerminal(QPainter &p, QPointF c) {
    p.save();
    p.translate(c);
    QLinearGradient g(0, -37, 0, 37);
    g.setColorAt(0, QColor(48, 48, 52));
    g.setColorAt(1, QColor(26, 26, 29));
    p.setBrush(g);
    p.setPen(QPen(QColor(95, 95, 100), 2.5));
    p.drawRoundedRect(QRectF(-38, -37, 76, 74), 9, 9);
    QPen w(Qt::white, 5);
    w.setCapStyle(Qt::RoundCap);
    w.setJoinStyle(Qt::RoundJoin);
    p.setPen(w);
    p.setBrush(Qt::NoBrush);
    QPolygonF chev;
    chev << QPointF(-25, -10) << QPointF(-10, 0) << QPointF(-25, 10);
    p.drawPolyline(chev);
    p.drawLine(QPointF(-4, 12), QPointF(20, 12));
    p.restore();
}

void drawGrid(QPainter &p, QPointF c) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(245, 245, 245));
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
            p.drawEllipse(c + QPointF(i * 22, j * 22), 6.8, 6.8);
}

void drawSearch(QPainter &p, QPointF c) {
    QPen w(QColor(240, 240, 240), 4.5);
    w.setCapStyle(Qt::RoundCap);
    p.setPen(w);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c + QPointF(-5, -5), 16, 16);
    p.drawLine(c + QPointF(7, 7), c + QPointF(22, 22));
}

void drawSmallSquare(QPainter &p, QPointF c) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(245, 245, 245));
    p.drawRoundedRect(QRectF(c.x() - 6, c.y() - 6, 12, 12), 2, 2);
}

void drawSeparator(QPainter &p, double x, double cy, double h) {
    p.setPen(QPen(QColor(200, 195, 195, 200), 2.5));
    p.drawLine(QPointF(x, cy - h / 2), QPointF(x, cy + h / 2));
}

}  // namespace

// =====================================================================
class CismaTaskbarWindow : public QWidget {
public:
    CismaTaskbarWindow() {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
        setWindowTitle("Cisma Taskbar");
        setFixedSize(1122, 546);   // 1496x728 * 0.75
        QProcess current;
        current.start("cisma-taskbar-mode", {"--current"});
        if (current.waitForFinished(1500) && current.exitCode() == 0)
            selected_ = current.readAllStandardOutput().trimmed() == "pulse" ? 1 : 0;
    }

    int selectedStyle() const { return selected_; }   // 0 = Padrão, 1 = Cisma Pulse

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                         QPainter::SmoothPixmapTransform);
        p.scale(scale(), scale());
        p.translate(kMargin, kMargin);
        drawWindow(p);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        QPointF d = toDesign(e->position());
        if (kCloseBtn.contains(d)) { close(); return; }
        if (kMinBtn.contains(d))   { showMinimized(); return; }
        if (kOpt1.contains(d))     { selected_ = 0; update(); applySelected(); return; }
        if (kOpt2.contains(d))     { selected_ = 1; update(); applySelected(); return; }
        if (d.y() < kHeaderH && kWin.contains(d)) windowHandle()->startSystemMove();
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        QPointF d = toDesign(e->position());
        int h = 0;
        if (kCloseBtn.contains(d)) h = 1;
        else if (kMinBtn.contains(d)) h = 2;
        else if (kOpt1.contains(d)) h = 3;
        else if (kOpt2.contains(d)) h = 4;
        if (h != hover_) {
            hover_ = h;
            setCursor(h ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void leaveEvent(QEvent *) override { hover_ = 0; update(); }

    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape) close();
        else if (e->key() == Qt::Key_Up)   { selected_ = 0; update(); }
        else if (e->key() == Qt::Key_Down) { selected_ = 1; update(); }
    }

private:
    int selected_ = 0;
    int hover_ = 0;
    bool applying_ = false;

    double scale() const { return width() / kCanvasW; }
    QPointF toDesign(const QPointF &pos) const {
        return QPointF(pos.x() / scale() - kMargin, pos.y() / scale() - kMargin);
    }

    void applySelected() {
        if (applying_) return;
        applying_ = true;
        setCursor(Qt::WaitCursor);
        auto *process = new QProcess(this);
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, process](int code, QProcess::ExitStatus status) {
            applying_ = false;
            unsetCursor();
            if (status == QProcess::NormalExit && code == 0) {
                close();
                return;
            }
            QMessageBox::warning(this, "Cisma Taskbar",
                "Não foi possível aplicar este estilo. Confira se o painel MATE está ativo e tente novamente.");
            process->deleteLater();
        });
        process->start("cisma-taskbar-mode", {"--set", selected_ == 1 ? "pulse" : "tabs"});
        if (!process->waitForStarted(2000)) {
            applying_ = false;
            unsetCursor();
            QMessageBox::warning(this, "Cisma Taskbar",
                "O comando cisma-taskbar-mode não foi encontrado.");
            process->deleteLater();
        }
    }

    // ---------------- janela ----------------
    void drawWindow(QPainter &p) {
        QPainterPath winPath = rounded(kWin, 56);

        p.save();
        p.setClipPath(winPath);

        // fundo
        QLinearGradient bg(0, 0, 0, kWin.height());
        bg.setColorAt(0.0, QColor(22, 2, 6));
        bg.setColorAt(1.0, QColor(36, 2, 9));
        p.fillPath(winPath, bg);

        QRadialGradient glowL(120, 640, 520);
        glowL.setColorAt(0, QColor(170, 0, 25, 70));
        glowL.setColorAt(1, QColor(170, 0, 25, 0));
        p.fillRect(kWin, glowL);

        drawHeader(p);
        drawPanel(p);
        drawFooter(p);
        p.restore();

        // borda externa com brilho
        QPainterPath edge = rounded(kWin.adjusted(2, 2, -2, -2), 54);
        glowPath(p, edge, QColor(225, 0, 30), 5.0, 6, 0.07);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(90, 0, 12), 2));
        p.drawPath(rounded(kWin.adjusted(9, 9, -9, -9), 48));
    }

    // ---------------- cabeçalho ----------------
    void drawHeader(QPainter &p) {
        // faixas diagonais
        QLinearGradient sg(880, 0, 1210, 0);
        sg.setColorAt(0, QColor(150, 0, 20, 90));
        sg.setColorAt(1, QColor(240, 10, 35, 230));
        QPolygonF big;
        big << QPointF(1010, 0) << QPointF(1215, 0) << QPointF(1085, kHeaderH) << QPointF(885, kHeaderH);
        p.setPen(Qt::NoPen);
        p.setBrush(sg);
        p.drawPolygon(big);

        QPolygonF thin;
        thin << QPointF(985, 0) << QPointF(1030, 0) << QPointF(912, kHeaderH) << QPointF(872, kHeaderH);
        p.setBrush(QColor(210, 0, 28, 150));
        p.drawPolygon(thin);

        QPolygonF dark;
        dark << QPointF(1060, 0) << QPointF(1130, 0) << QPointF(1000, kHeaderH) << QPointF(940, kHeaderH);
        p.setBrush(QColor(40, 0, 6, 120));
        p.drawPolygon(dark);

        // logo + separador
        drawLogo(p, QPointF(130, 95), 128);
        p.setPen(QPen(QColor(225, 220, 220), 2.5));
        p.drawLine(QPointF(232, 52), QPointF(232, 144));

        // título
        QFont tf = makeFont(66, QFont::DemiBold);
        QFontMetricsF fm(tf);
        QString a = "Cisma ";
        drawTextV(p, 265, 82, a, 66, QFont::DemiBold, Qt::white);
        drawTextV(p, 265 + fm.horizontalAdvance(a), 82, "Taskbar", 66, QFont::DemiBold, kRed);
        drawTextV(p, 267, 140, "Escolha o estilo da sua barra de tarefas.", 31,
                  QFont::Normal, QColor(235, 215, 218));

        // minimizar / fechar
        p.setBrush(Qt::NoBrush);
        QPen mp(hover_ == 2 ? QColor(255, 255, 255) : QColor(240, 240, 240), 5.5);
        mp.setCapStyle(Qt::RoundCap);
        p.setPen(mp);
        p.drawLine(QPointF(1260, 50), QPointF(1294, 50));

        QPen cp(hover_ == 1 ? kRedHi : kRed, 7);
        cp.setCapStyle(Qt::RoundCap);
        p.setPen(cp);
        p.drawLine(QPointF(1368, 36), QPointF(1402, 70));
        p.drawLine(QPointF(1402, 36), QPointF(1368, 70));
    }

    // ---------------- painel central ----------------
    void drawPanel(QPainter &p) {
        QPainterPath pp = rounded(kPanel, 32);
        QLinearGradient g(0, kPanel.top(), 0, kPanel.bottom());
        g.setColorAt(0, QColor(70, 2, 12));
        g.setColorAt(1, QColor(34, 1, 7));
        p.fillPath(pp, g);
        glowPath(p, pp, QColor(185, 10, 30), 2.5, 4, 0.05);

        drawOption(p, kOpt1, kBar1, "Padrão", selected_ == 0, hover_ == 3, true);
        drawOption(p, kOpt2, kBar2, "Cisma Pulse", selected_ == 1, hover_ == 4, false);
    }

    void drawOption(QPainter &p, const QRectF &r, const QRectF &bar, const QString &label,
                    bool sel, bool hov, bool standard) {
        QPainterPath op = rounded(r, 32);
        QLinearGradient g(r.left(), 0, r.right(), 0);
        if (sel) {
            g.setColorAt(0, QColor(150, 6, 24));
            g.setColorAt(0.5, QColor(95, 3, 15));
            g.setColorAt(1, QColor(60, 2, 10));
        } else {
            g.setColorAt(0, QColor(88, 6, 18));
            g.setColorAt(1, QColor(52, 3, 10));
        }
        p.fillPath(op, g);
        if (sel)
            glowPath(p, op, QColor(255, 70, 85), 3.5, 8, 0.07);
        else
            glowPath(p, op, hov ? QColor(190, 25, 45) : QColor(125, 15, 32), 2.2, 2, 0.04);

        // radio
        QPointF rc(r.left() + 64, r.center().y());
        if (sel) {
            p.setBrush(Qt::NoBrush);
            glowPath(p, [&] { QPainterPath q; q.addEllipse(rc, 27, 27); return q; }(),
                     QColor(235, 20, 40), 7, 4, 0.08);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(240, 20, 40));
            p.drawEllipse(rc, 16.5, 16.5);
        } else {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(205, 205, 205), 4));
            p.drawEllipse(rc, 27, 27);
        }

        drawTextV(p, r.left() + 128, r.center().y(), label, 43, QFont::DemiBold, Qt::white);

        if (standard) drawStandardBar(p, bar);
        else          drawPulseBar(p, bar);
    }

    void drawBarBase(QPainter &p, const QRectF &bar, double rad) {
        QPainterPath bp = rounded(bar, rad);
        QLinearGradient g(0, bar.top(), 0, bar.bottom());
        g.setColorAt(0, QColor(5, 0, 2));
        g.setColorAt(0.5, QColor(18, 0, 5));
        g.setColorAt(1, QColor(4, 0, 2));
        p.fillPath(bp, g);
        glowPath(p, bp, QColor(205, 15, 35), 3.0, 3, 0.06);
    }

    void drawStandardBar(QPainter &p, const QRectF &bar) {
        drawBarBase(p, bar, 44);
        double cy = bar.center().y() + 8;
        drawGrid(p, QPointF(572, cy));
        drawTerminal(p, QPointF(706, cy));
        drawChrome(p, QPointF(830, cy + 1), 40);
        drawFolder(p, QPointF(950, cy + 2), 1.0, QColor(255, 20, 40), QColor(200, 0, 22));
        drawGear(p, QPointF(1072, cy), 37);
        drawSeparator(p, 1150, cy, 76);
        drawTrash(p, QPointF(1225, cy), 1.0);
        drawSmallSquare(p, QPointF(1320, cy));
    }

    void drawPulseBar(QPainter &p, const QRectF &bar) {
        drawBarBase(p, bar, 40);

        // detalhe diagonal à esquerda + linha vermelha inferior
        p.save();
        p.setClipPath(rounded(bar, 40));
        QLinearGradient dg(bar.left(), 0, bar.left() + 130, 0);
        dg.setColorAt(0, QColor(190, 0, 25, 190));
        dg.setColorAt(1, QColor(190, 0, 25, 0));
        QPolygonF poly;
        poly << QPointF(bar.left(), bar.top()) << QPointF(bar.left() + 70, bar.top())
             << QPointF(bar.left() + 8, bar.bottom()) << QPointF(bar.left(), bar.bottom());
        p.setPen(Qt::NoPen);
        p.setBrush(dg);
        p.drawPolygon(poly);

        QLinearGradient lg(bar.left(), 0, bar.right(), 0);
        lg.setColorAt(0, QColor(220, 10, 30, 0));
        lg.setColorAt(0.2, QColor(220, 10, 30, 200));
        lg.setColorAt(0.8, QColor(220, 10, 30, 200));
        lg.setColorAt(1, QColor(220, 10, 30, 0));
        p.setPen(QPen(QBrush(lg), 3));
        p.drawLine(QPointF(bar.left() + 20, bar.bottom() - 3), QPointF(bar.right() - 20, bar.bottom() - 3));
        p.restore();

        double cy = bar.center().y();
        drawLogo(p, QPointF(645, cy), 64);
        drawSeparator(p, 750, cy, 54);
        drawSearch(p, QPointF(836, cy));
        drawFolder(p, QPointF(956, cy + 2), 0.68, QColor(250, 250, 250), QColor(205, 205, 205));
        drawGear(p, QPointF(1075, cy), 36);
        drawSeparator(p, 1153, cy, 66);
        drawTrash(p, QPointF(1227, cy), 0.9);
        drawSmallSquare(p, QPointF(1322, cy));
    }

    // ---------------- rodapé ----------------
    void drawFooter(QPainter &p) {
        // aba do canto inferior direito
        QPainterPath tab;
        tab.moveTo(1030, 688);
        tab.lineTo(1100, 592);
        tab.quadTo(1112, 575, 1135, 575);
        tab.lineTo(1456, 575);
        tab.lineTo(1456, 688);
        tab.closeSubpath();
        QLinearGradient tg(1030, 0, 1456, 0);
        tg.setColorAt(0, QColor(70, 2, 12));
        tg.setColorAt(1, QColor(28, 1, 6));
        p.fillPath(tab, tg);

        QPainterPath edge;
        edge.moveTo(1030, 688);
        edge.lineTo(1100, 592);
        edge.quadTo(1112, 575, 1135, 575);
        edge.lineTo(1456, 575);
        glowPath(p, edge, QColor(215, 10, 32), 2.5, 3, 0.05);

        // linha inferior
        p.setPen(QPen(QColor(225, 10, 32), 3));
        p.drawLine(QPointF(60, 683), QPointF(1030, 683));

        // ícone de informação
        QPointF ic(82, 620);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(235, 10, 35));
        p.drawEllipse(ic, 25, 25);
        p.setFont(makeFont(36, QFont::Bold));
        p.setPen(Qt::white);
        p.drawText(QRectF(ic.x() - 25, ic.y() - 25, 50, 50), Qt::AlignCenter, "i");

        drawTextV(p, 137, 604, "Clique no estilo desejado para aplicar a barra de tarefas", 26,
                  QFont::Normal, QColor(238, 228, 228));
        drawTextV(p, 137, 639, "Você pode voltar a esta tela e trocar quando quiser.", 26,
                  QFont::Normal, QColor(238, 228, 228));

        // marca CISMA OS
        drawLogo(p, QPointF(1182, 633), 66);
        drawTextV(p, 1247, 632, "CISMA OS", 37, QFont::Bold, kRed);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    CismaTaskbarWindow w;
    if (QScreen *s = QGuiApplication::primaryScreen()) {
        w.move(s->availableGeometry().center() - QPoint(w.width() / 2, w.height() / 2));
    }
    w.show();
    return app.exec();
}
