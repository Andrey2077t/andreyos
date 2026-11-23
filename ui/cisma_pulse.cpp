#include <QApplication>
#include <QDateTime>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QIcon>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPolygonF>
#include <QPen>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QShowEvent>
#include <QCursor>
#include <QVector>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdlib>
#include <unistd.h>

namespace {
constexpr int kBarHeight = 70;
constexpr int kPinButton = 56;
constexpr int kGap = 5;
constexpr int kRightArea = 214;
const QColor kRed(237, 18, 38);

QString configDir() {
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    return QDir::cleanPath((xdg.isEmpty() ? QDir::homePath() + "/.config" : QString::fromLocal8Bit(xdg))
                           + "/cisma-os");
}

QString runtimeDir() {
    const QByteArray dir = qgetenv("XDG_RUNTIME_DIR");
    return dir.isEmpty() ? QStringLiteral("/tmp") : QString::fromLocal8Bit(dir);
}

struct DesktopApp {
    QString id;
    QString name;
    QString iconName;
    QString wmClass;
    QString filePath;

    QIcon icon() const {
        if (!iconName.isEmpty() && QFileInfo(iconName).isAbsolute()) return QIcon(iconName);
        QIcon result = QIcon::fromTheme(iconName);
        if (result.isNull()) result = QIcon::fromTheme("application-x-executable");
        return result;
    }
};

struct ClientWindow {
    Window id = 0;
    QString title;
    QString wmClass;
    QString desktopId;
    QIcon icon;
    bool active = false;
    bool minimized = false;
};

class CismaPulse final : public QWidget {
public:
    explicit CismaPulse(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("Cisma Pulse");
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        loadDesktopApps();
        loadPins();

        display_ = XOpenDisplay(nullptr);
        if (display_) {
            root_ = DefaultRootWindow(display_);
            atoms();
        }

        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect bounds = screen->geometry();
            setGeometry(bounds.x(), bounds.y() + bounds.height() - kBarHeight,
                        bounds.width(), kBarHeight);
        }
        else resize(1280, kBarHeight);
        loadGroupPosition();
        setToolTip("Segure o C estilizado e arraste para mover o grupo de ícones");

        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            if (readMode() != "pulse") { close(); return; }
            refreshWindows();
            update();
        });
        timer->start(600);
        refreshWindows();
    }

    ~CismaPulse() override {
        if (display_) XCloseDisplay(display_);
        const QString pidPath = runtimeDir() + "/cisma-pulse.pid";
        QFile pidFile(pidPath);
        if (pidFile.open(QIODevice::ReadOnly | QIODevice::Text)
            && pidFile.readLine().trimmed().toLongLong() == static_cast<qlonglong>(getpid()))
            QFile::remove(pidPath);
    }

    bool ready() const { return display_ != nullptr; }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        installDockHints();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        drawBackground(p);
        pinRects_.clear();
        taskRects_.clear();

        const int cy = height() / 2;
        const int groupX = groupStartX();
        QRect startRect(groupX, 6, 56, height() - 12);
        drawStartButton(p, startRect);
        int x = startRect.right() + 12;
        for (int i = 0; i < pins_.size(); ++i) {
            QRect r(x, 7, kPinButton, height() - 14);
            pinRects_.append(r);
            drawPin(p, r, pins_[i]);
            x += kPinButton + kGap;
        }
        const int separatorX = x + 1;
        p.setPen(QPen(QColor(205, 198, 198, 175), 1));
        p.drawLine(separatorX, 13, separatorX, height() - 13);
        x += 12;

        const int rightStart = width() - kRightArea;
        QVector<ClientWindow> extra;
        for (const auto &w : windows_) {
            bool represented = false;
            for (const auto &pin : pins_) {
                if (matches(w, pin)) { represented = true; break; }
            }
            if (!represented) extra.append(w);
        }
        int available = qMax(0, rightStart - x - 7);
        pagePrevRect_ = {};
        pageNextRect_ = {};
        int first = 0;
        int visible = extra.size();
        const bool overflow = !extra.isEmpty() && available / 50 < extra.size();
        if (overflow && available >= 58) {
            pagePrevRect_ = QRect(x, 13, 24, height() - 26);
            drawPageArrow(p, pagePrevRect_, "‹", taskPage_ > 0);
            x += 29;
            available -= 58;
            visible = qMax(1, (available + kGap) / 50);
            const int lastPage = (extra.size() - 1) / visible;
            taskPage_ = qBound(0, taskPage_, lastPage);
            first = taskPage_ * visible;
            pageNextRect_ = QRect(rightStart - 24, 13, 24, height() - 26);
            drawPageArrow(p, pageNextRect_, "›", taskPage_ < lastPage);
        } else {
            taskPage_ = 0;
        }
        const int taskAreaRight = overflow ? pageNextRect_.left() - 4 : rightStart;
        available = qMax(0, taskAreaRight - x - 4);
        const int count = qMin(visible, static_cast<int>(extra.size()) - first);
        const int buttonWidth = count > 0 ? qBound(44, available / count - kGap, 210) : 0;
        for (int i = first; i < first + count && x + buttonWidth <= taskAreaRight; ++i) {
            QRect r(x, 8, buttonWidth, height() - 16);
            taskRects_.append({r, extra[i].id});
            drawTask(p, r, extra[i]);
            x += buttonWidth + kGap;
        }

        drawSystemArea(p, cy);
    }

    void mousePressEvent(QMouseEvent *event) override {
        const QPoint pos = event->position().toPoint();
        if (event->button() == Qt::LeftButton) {
            if (QRect(groupStartX(), 6, 56, height() - 12).contains(pos)) {
                draggingStart_ = true;
                dragMoved_ = false;
                dragPressX_ = pos.x();
                setCursor(Qt::SizeHorCursor);
                return;
            }
            if (pagePrevRect_.contains(pos) && taskPage_ > 0) { --taskPage_; update(); return; }
            if (pageNextRect_.contains(pos)) { ++taskPage_; update(); return; }
            for (int i = 0; i < pinRects_.size(); ++i) {
                if (!pinRects_[i].contains(pos)) continue;
                activatePinned(pins_[i]);
                return;
            }
            for (const auto &entry : taskRects_) {
                if (entry.first.contains(pos)) {
                    activateWindow(entry.second);
                    return;
                }
            }
            if (pos.x() >= width() - 215 && pos.x() < width() - 155) {
                QProcess::startDetached("mate-display-properties", {});
                return;
            }
            if (pos.x() >= width() - 155 && pos.x() < width() - 95) {
                QProcess::startDetached("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "toggle"});
                return;
            }
        }
        if (event->button() == Qt::RightButton) showContextMenu(pos);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (!draggingStart_ || !(event->buttons() & Qt::LeftButton)) return;
        const int x = event->position().toPoint().x();
        if (qAbs(x - dragPressX_) > 4) dragMoved_ = true;
        if (dragMoved_) {
            groupCenterRatio_ = qBound(0.0, static_cast<double>(x) / qMax(1, width()), 1.0);
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton || !draggingStart_) return;
        draggingStart_ = false;
        unsetCursor();
        if (dragMoved_) {
            saveGroupPosition();
            update();
        } else {
            QProcess::startDetached("cisma-start-toggle", {});
        }
    }

    void leaveEvent(QEvent *) override { update(); }

private:
    Display *display_ = nullptr;
    Window root_ = 0;
    Atom netClientList_ = None;
    Atom netActiveWindow_ = None;
    Atom netName_ = None;
    Atom netWmType_ = None;
    Atom netWmTypeNormal_ = None;
    Atom netWmState_ = None;
    Atom netWmStateSkipTaskbar_ = None;
    Atom netWmDesktop_ = None;
    Atom utf8String_ = None;
    Atom netWmIcon_ = None;
    Atom netWmStateHidden_ = None;
    Atom netWmStateMaxH_ = None;
    Atom netWmStateMaxV_ = None;
    Atom netWmStateSticky_ = None;
    Atom netWmStateAbove_ = None;
    Atom netWmStateSkipPager_ = None;
    Atom netWmWindowType_ = None;
    Atom netWmWindowTypeDock_ = None;
    Atom netWmStrut_ = None;
    Atom netWmStrutPartial_ = None;
    Atom netWmCloseWindow_ = None;
    QVector<DesktopApp> apps_;
    QVector<DesktopApp> pins_;
    QVector<ClientWindow> windows_;
    QVector<QRect> pinRects_;
    QVector<QPair<QRect, Window>> taskRects_;
    QRect pagePrevRect_;
    QRect pageNextRect_;
    int taskPage_ = 0;
    double groupCenterRatio_ = 0.1125;
    int dragPressX_ = 0;
    bool draggingStart_ = false;
    bool dragMoved_ = false;

    int groupWidth() const {
        return 56 + 12 + static_cast<int>(pins_.size()) * (kPinButton + kGap) + 13;
    }

    int groupStartX() const {
        const int rightStart = width() - kRightArea;
        const int maxStart = qMax(8, rightStart - groupWidth() - 80);
        return qBound(8, qRound(groupCenterRatio_ * width()) - groupWidth() / 2, maxStart);
    }

    void loadGroupPosition() {
        QFile file(configDir() + "/pulse-position");
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        bool ok = false;
        const double value = QString::fromUtf8(file.readLine()).trimmed().toDouble(&ok);
        if (ok) groupCenterRatio_ = qBound(0.0, value, 1.0);
    }

    void saveGroupPosition() const {
        QDir().mkpath(configDir());
        QFile file(configDir() + "/pulse-position");
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            file.write((QString::number(groupCenterRatio_, 'f', 4) + "\n").toUtf8());
    }

    Atom atom(const char *name) { return XInternAtom(display_, name, False); }

    void atoms() {
        netClientList_ = atom("_NET_CLIENT_LIST_STACKING");
        if (netClientList_ == None) netClientList_ = atom("_NET_CLIENT_LIST");
        netActiveWindow_ = atom("_NET_ACTIVE_WINDOW");
        netName_ = atom("_NET_WM_NAME");
        netWmType_ = atom("_NET_WM_WINDOW_TYPE");
        netWmTypeNormal_ = atom("_NET_WM_WINDOW_TYPE_NORMAL");
        netWmState_ = atom("_NET_WM_STATE");
        netWmStateSkipTaskbar_ = atom("_NET_WM_STATE_SKIP_TASKBAR");
        netWmDesktop_ = atom("_NET_WM_DESKTOP");
        utf8String_ = atom("UTF8_STRING");
        netWmIcon_ = atom("_NET_WM_ICON");
        netWmStateHidden_ = atom("_NET_WM_STATE_HIDDEN");
        netWmStateMaxH_ = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
        netWmStateMaxV_ = atom("_NET_WM_STATE_MAXIMIZED_VERT");
        netWmStateSticky_ = atom("_NET_WM_STATE_STICKY");
        netWmStateAbove_ = atom("_NET_WM_STATE_ABOVE");
        netWmStateSkipPager_ = atom("_NET_WM_STATE_SKIP_PAGER");
        netWmWindowType_ = atom("_NET_WM_WINDOW_TYPE");
        netWmWindowTypeDock_ = atom("_NET_WM_WINDOW_TYPE_DOCK");
        netWmStrut_ = atom("_NET_WM_STRUT");
        netWmStrutPartial_ = atom("_NET_WM_STRUT_PARTIAL");
        netWmCloseWindow_ = atom("_NET_CLOSE_WINDOW");
    }

    QVector<unsigned long> property(Window window, Atom name, Atom type, long length = 4096) const {
        QVector<unsigned long> values;
        if (!display_ || name == None) return values;
        Atom actual = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char *data = nullptr;
        const int status = XGetWindowProperty(display_, window, name, 0, length, False,
                                              type, &actual, &format, &count, &remaining, &data);
        if (status == Success && data && format == 32) {
            auto *raw = reinterpret_cast<unsigned long *>(data);
            values.reserve(static_cast<int>(count));
            for (unsigned long i = 0; i < count; ++i) values.append(raw[i]);
        }
        if (data) XFree(data);
        return values;
    }

    QString textProperty(Window window, Atom name) const {
        Atom actual = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char *data = nullptr;
        QString result;
        if (XGetWindowProperty(display_, window, name, 0, 2048, False, utf8String_,
                               &actual, &format, &count, &remaining, &data) == Success
            && data && format == 8) {
            result = QString::fromUtf8(reinterpret_cast<char *>(data), static_cast<int>(count));
        }
        if (data) XFree(data);
        return result;
    }

    QString wmClass(Window window) const {
        XClassHint hint{};
        if (!XGetClassHint(display_, window, &hint)) return {};
        QString result = hint.res_class ? QString::fromLocal8Bit(hint.res_class) : QString();
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        return result;
    }

    QIcon windowIcon(Window window) const {
        const auto raw = property(window, netWmIcon_, XA_CARDINAL, 65536);
        for (int i = 0; i + 2 < raw.size();) {
            const unsigned long w = raw[i++], h = raw[i++];
            if (!w || !h || w > 256 || h > 256 || i + static_cast<qsizetype>(w * h) > raw.size()) break;
            if (w <= 64 && h <= 64) {
                QImage image(static_cast<int>(w), static_cast<int>(h), QImage::Format_ARGB32);
                for (unsigned long y = 0; y < h; ++y) {
                    auto *row = reinterpret_cast<QRgb *>(image.scanLine(static_cast<int>(y)));
                    for (unsigned long x = 0; x < w; ++x)
                        row[x] = static_cast<QRgb>(raw[i + static_cast<qsizetype>(y * w + x)] & 0xffffffffUL);
                }
                return QIcon(QPixmap::fromImage(image));
            }
            i += static_cast<qsizetype>(w * h);
        }
        return {};
    }

    void loadDesktopApps() {
        QStringList roots = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
        roots.prepend(QDir::homePath() + "/.local/share/applications");
        QSet<QString> seen;
        for (const QString &root : roots) {
            QDirIterator it(root, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                const QString id = QFileInfo(path).fileName();
                if (seen.contains(id)) continue;
                seen.insert(id);
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
                DesktopApp app;
                app.id = id;
                app.filePath = path;
                bool inEntry = false;
                while (!file.atEnd()) {
                    const QString line = QString::fromUtf8(file.readLine()).trimmed();
                    if (line.startsWith('[')) { inEntry = line == "[Desktop Entry]"; continue; }
                    if (!inEntry || line.startsWith('#')) continue;
                    const int eq = line.indexOf('=');
                    if (eq <= 0) continue;
                    const QString key = line.left(eq), value = line.mid(eq + 1);
                    if (key == "Name") app.name = value;
                    else if (key == "Icon") app.iconName = value;
                    else if (key == "StartupWMClass") app.wmClass = value;
                }
                if (!app.id.isEmpty() && !app.name.isEmpty()) apps_.append(app);
            }
        }
    }

    void loadPins() {
        QDir().mkpath(configDir());
        QFile file(configDir() + "/pulse-pins");
        QStringList ids;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!file.atEnd()) {
                const QString id = QString::fromUtf8(file.readLine()).trimmed();
                if (!id.isEmpty() && !ids.contains(id)) ids.append(id);
            }
        } else {
            ids = {"cisma-files.desktop", "steam.desktop", "cisma-settings.desktop", "chromium.desktop"};
            savePins(ids);
        }
        pins_.clear();
        for (const QString &id : ids) {
            auto it = std::find_if(apps_.cbegin(), apps_.cend(), [&id](const DesktopApp &a) { return a.id == id; });
            if (it != apps_.cend()) pins_.append(*it);
        }
    }

    void savePins(const QStringList &ids) const {
        QFile file(configDir() + "/pulse-pins");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
        for (const QString &id : ids) file.write((id + "\n").toUtf8());
    }

    static QString readMode() {
        QFile file(configDir() + "/taskbar-mode");
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return "tabs";
        return QString::fromUtf8(file.readLine()).trimmed();
    }

    static bool closeTo(const QString &a, const QString &b) {
        const QString x = a.toLower(), y = b.toLower();
        return !x.isEmpty() && !y.isEmpty() && (x == y || x.contains(y) || y.contains(x));
    }

    bool matches(const ClientWindow &window, const DesktopApp &app) const {
        if (closeTo(window.wmClass, app.wmClass)) return true;
        const QString id = app.id.toLower();
        const QString cls = window.wmClass.toLower();
        if (id == "cisma-files.desktop" && cls.contains("caja")) return true;
        if (id == "cisma-terminal.desktop" && (cls.contains("mate-terminal") || cls.contains("terminal"))) return true;
        if (id == "cisma-settings.desktop" && (cls.contains("mate-control-center") || cls.contains("control-center"))) return true;
        if (id == "firefox-esr.desktop" && (cls.contains("firefox") || cls.contains("navigator"))) return true;
        if (id == "chromium.desktop" && (cls.contains("chromium") || cls.contains("google-chrome"))) return true;
        if (id == "steam.desktop" && cls.contains("steam")) return true;
        QString stem = id;
        stem.remove(QRegularExpression("\\.desktop$"));
        stem.replace('-', ' ');
        return closeTo(window.wmClass, stem);
    }

    bool hasAtom(const QVector<unsigned long> &list, Atom value) const {
        return std::find(list.cbegin(), list.cend(), static_cast<unsigned long>(value)) != list.cend();
    }

    void refreshWindows() {
        if (!display_) return;
        const auto ids = property(root_, netClientList_, XA_WINDOW, 32768);
        const auto active = property(root_, netActiveWindow_, XA_WINDOW, 1);
        const Window activeId = active.isEmpty() ? 0 : static_cast<Window>(active[0]);
        QVector<ClientWindow> updated;
        for (const unsigned long raw : ids) {
            const Window id = static_cast<Window>(raw);
            const auto types = property(id, netWmType_, XA_ATOM, 32);
            if (!hasAtom(types, netWmTypeNormal_)) continue;
            const auto states = property(id, netWmState_, XA_ATOM, 64);
            if (hasAtom(states, netWmStateSkipTaskbar_)) continue;
            ClientWindow w;
            w.id = id;
            w.title = textProperty(id, netName_);
            if (w.title.isEmpty()) {
                char *name = nullptr;
                if (XFetchName(display_, id, &name) && name) {
                    w.title = QString::fromLocal8Bit(name);
                    XFree(name);
                }
            }
            if (w.title.trimmed().isEmpty()) w.title = "Janela";
            w.wmClass = wmClass(id);
            w.active = id == activeId;
            w.minimized = hasAtom(states, netWmStateHidden_);
            w.icon = windowIcon(id);
            for (const auto &app : apps_) {
                if (matches(w, app)) {
                    w.desktopId = app.id;
                    if (w.icon.isNull()) w.icon = app.icon();
                    break;
                }
            }
            if (w.icon.isNull()) w.icon = QIcon::fromTheme("application-x-executable");
            updated.append(w);
        }
        windows_ = updated;
    }

    void installDockHints() {
        if (!display_) return;
        const Window self = static_cast<Window>(winId());
        const Atom type = netWmWindowTypeDock_;
        XChangeProperty(display_, self, netWmWindowType_, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&type), 1);
        const unsigned long state[] = {netWmStateSticky_, netWmStateAbove_, netWmStateSkipPager_};
        XChangeProperty(display_, self, netWmState_, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(state), 3);
        const int bottom = kBarHeight;
        const int x0 = geometry().x();
        const int x1 = x0 + width() - 1;
        const unsigned long strut[] = {0, 0, 0, static_cast<unsigned long>(bottom)};
        XChangeProperty(display_, self, netWmStrut_, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(strut), 4);
        const unsigned long partial[] = {0, 0, 0, static_cast<unsigned long>(bottom),
                                         0, 0, 0, 0, 0, 0,
                                         static_cast<unsigned long>(x0), static_cast<unsigned long>(x1)};
        XChangeProperty(display_, self, netWmStrutPartial_, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(partial), 12);
        XFlush(display_);
    }

    void sendClientMessage(Window target, Atom message, long a0, long a1 = 0, long a2 = 0) {
        if (!display_) return;
        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.window = target;
        e.xclient.message_type = message;
        e.xclient.format = 32;
        e.xclient.data.l[0] = a0;
        e.xclient.data.l[1] = a1;
        e.xclient.data.l[2] = a2;
        XSendEvent(display_, root_, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
        XFlush(display_);
    }

    void activateWindow(Window id) {
        const auto active = property(root_, netActiveWindow_, XA_WINDOW, 1);
        if (!active.isEmpty() && static_cast<Window>(active[0]) == id) {
            XIconifyWindow(display_, id, DefaultScreen(display_));
            XFlush(display_);
            return;
        }
        sendClientMessage(id, netActiveWindow_, 2, CurrentTime, 0);
    }

    void activatePinned(const DesktopApp &app) {
        for (auto it = windows_.crbegin(); it != windows_.crend(); ++it) {
            if (matches(*it, app)) { activateWindow(it->id); return; }
        }
        QString desktopId = app.id;
        if (desktopId.endsWith(".desktop")) desktopId.chop(8);
        QProcess::startDetached("gtk-launch", {desktopId});
    }

    void drawBackground(QPainter &p) {
        const QRectF bar(1, 1, width() - 2, height() - 2);
        QPainterPath path;
        path.addRoundedRect(bar, 28, 28);
        QLinearGradient bg(0, 0, width(), 0);
        bg.setColorAt(0.00, QColor(57, 1, 7, 247));
        bg.setColorAt(0.14, QColor(21, 1, 4, 250));
        bg.setColorAt(0.50, QColor(5, 1, 2, 250));
        bg.setColorAt(0.86, QColor(21, 1, 4, 250));
        bg.setColorAt(1.00, QColor(57, 1, 7, 247));
        p.fillPath(path, bg);
        QRadialGradient left(18, height() / 2, 155);
        left.setColorAt(0, QColor(240, 8, 30, 115));
        left.setColorAt(1, QColor(240, 8, 30, 0));
        p.fillRect(QRect(0, 0, 180, height()), left);
        QRadialGradient right(width() - 18, height() / 2, 145);
        right.setColorAt(0, QColor(240, 8, 30, 92));
        right.setColorAt(1, QColor(240, 8, 30, 0));
        p.fillRect(QRect(width() - 180, 0, 180, height()), right);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(237, 18, 38, 190), 1.5));
        p.drawPath(path);
        p.setPen(QPen(QColor(255, 65, 75, 82), 1));
        p.drawLine(30, 2, width() - 30, 2);
    }

    void drawStartButton(QPainter &p, const QRect &r) {
        const bool hover = r.contains(mapFromGlobal(QCursor::pos()));
        p.setPen(Qt::NoPen);
        p.setBrush(hover ? QColor(110, 4, 16, 220) : QColor(55, 2, 9, 155));
        p.drawRoundedRect(r, 12, 12);
        p.save();
        p.translate(r.center());
        QPolygonF c;
        c << QPointF(17, -16) << QPointF(-7, -16) << QPointF(-18, 0)
          << QPointF(-7, 16) << QPointF(17, 16) << QPointF(17, 7)
          << QPointF(-2, 7) << QPointF(-7, 0) << QPointF(-2, -7)
          << QPointF(17, -7);
        QLinearGradient g(0, -17, 0, 17);
        g.setColorAt(0, QColor(255, 45, 58));
        g.setColorAt(1, QColor(185, 0, 22));
        p.setBrush(g);
        p.drawPolygon(c);
        p.restore();
    }

    void drawPin(QPainter &p, const QRect &r, const DesktopApp &app) {
        bool running = false, active = false;
        for (const auto &w : windows_) {
            if (matches(w, app)) { running = true; active |= w.active; }
        }
        const bool hover = r.contains(mapFromGlobal(QCursor::pos()));
        if (active || hover) {
            p.setPen(QPen(active ? QColor(237, 18, 38, 210) : QColor(150, 40, 50, 110), active ? 1.4 : 1));
            p.setBrush(active ? QColor(110, 0, 12, 178) : QColor(60, 5, 12, 130));
            p.drawRoundedRect(r, 10, 10);
        }
        const QIcon icon = app.icon();
        if (!icon.isNull()) icon.paint(&p, QRect(r.center().x() - 20, r.center().y() - 20, 40, 40));
        if (running) {
            p.setPen(Qt::NoPen);
            p.setBrush(active ? kRed : QColor(230, 225, 225, 205));
            p.drawRoundedRect(QRect(r.center().x() - (active ? 13 : 4), r.bottom() - 5,
                                    active ? 26 : 8, 3), 1, 1);
        }
    }

    void drawTask(QPainter &p, const QRect &r, const ClientWindow &w) {
        const bool hover = r.contains(mapFromGlobal(QCursor::pos()));
        QPainterPath shape;
        shape.addRoundedRect(r, 10, 10);
        if (w.active || hover) {
            p.setPen(QPen(w.active ? QColor(237, 18, 38, 205) : QColor(130, 30, 42, 120), 1.2));
            p.setBrush(w.active ? QColor(80, 4, 12, 205) : QColor(45, 5, 10, 175));
            p.drawPath(shape);
        }
        if (!w.icon.isNull()) w.icon.paint(&p, QRect(r.left() + 9, r.center().y() - 16, 32, 32));
        p.setPen(QColor(245, 240, 240));
        QFont f("Inter");
        f.setPixelSize(13);
        p.setFont(f);
        const QRect textRect(r.left() + 47, r.top() + 5, r.width() - 53, r.height() - 10);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(w.title, Qt::ElideRight, textRect.width()));
        if (w.active) {
            p.setPen(QPen(kRed, 2));
            p.drawLine(r.left() + 12, r.bottom() - 3, r.right() - 12, r.bottom() - 3);
        }
    }

    void drawPageArrow(QPainter &p, const QRect &r, const QString &glyph, bool enabled) {
        p.setPen(QPen(enabled ? QColor(237, 18, 38, 185) : QColor(95, 75, 78, 120), 1));
        p.setBrush(enabled ? QColor(55, 4, 10, 150) : QColor(24, 8, 10, 100));
        p.drawRoundedRect(r, 7, 7);
        p.setPen(enabled ? QColor(245, 235, 235) : QColor(120, 105, 105));
        QFont f("Inter");
        f.setPixelSize(24);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, glyph);
    }

    void drawSystemArea(QPainter &p, int cy) {
        const int x = width() - kRightArea;
        p.setPen(QPen(QColor(210, 200, 200, 165), 1));
        p.drawLine(x + 4, 13, x + 4, height() - 13);
        // Monitor / configurações de tela.
        const int mx = x + 31;
        p.setPen(QPen(QColor(235, 232, 232), 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRect(mx - 12, cy - 12, 24, 17), 2, 2);
        p.drawLine(mx, cy + 5, mx, cy + 10);
        p.drawLine(mx - 7, cy + 11, mx + 7, cy + 11);

        // Volume e mudo refletem PulseAudio; clicar alterna o mute.
        const int vx = x + 92;
        const bool muted = runMuted();
        QPolygonF speaker;
        speaker << QPointF(vx - 14, cy - 5) << QPointF(vx - 8, cy - 5)
                << QPointF(vx + 1, cy - 12) << QPointF(vx + 1, cy + 12)
                << QPointF(vx - 8, cy + 5) << QPointF(vx - 14, cy + 5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(235, 232, 232));
        p.drawPolygon(speaker);
        if (muted) {
            p.setPen(QPen(kRed, 2.1, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(vx + 6, cy - 6, vx + 15, cy + 6);
            p.drawLine(vx + 15, cy - 6, vx + 6, cy + 6);
        } else {
            p.setPen(QPen(QColor(235, 232, 232), 1.7, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(QRect(vx - 2, cy - 9, 14, 18), -50 * 16, 100 * 16);
            p.drawArc(QRect(vx - 2, cy - 14, 22, 28), -50 * 16, 100 * 16);
        }

        const QDateTime now = QDateTime::currentDateTime();
        p.setPen(Qt::white);
        QFont f("Inter");
        f.setPixelSize(13);
        p.setFont(f);
        p.drawText(QRect(width() - 88, cy - 22, 78, 20), Qt::AlignCenter, now.toString("HH:mm"));
        QFont small = f;
        small.setPixelSize(10);
        p.setFont(small);
        p.setPen(QColor(220, 210, 210));
        p.drawText(QRect(width() - 94, cy + 1, 90, 18), Qt::AlignCenter, now.toString("dd/MM/yyyy"));
    }

    bool runMuted() const {
        QProcess p;
        p.start("pactl", {"get-sink-mute", "@DEFAULT_SINK@"});
        if (!p.waitForFinished(500)) return false;
        return QString::fromLocal8Bit(p.readAllStandardOutput()).contains("yes", Qt::CaseInsensitive);
    }

    void showContextMenu(const QPoint &pos) {
        QMenu menu(this);
        for (int i = 0; i < pinRects_.size(); ++i) {
            if (!pinRects_[i].contains(pos)) continue;
            const DesktopApp app = pins_[i];
            QAction *remove = menu.addAction("Desafixar " + app.name);
            QHash<QAction *, QPair<Window, bool>> windowActions;
            for (const auto &window : windows_) {
                if (!matches(window, app)) continue;
                QMenu *windowMenu = menu.addMenu(window.title);
                QAction *minimize = windowMenu->addAction("Minimizar");
                QAction *close = windowMenu->addAction("Fechar janela");
                windowActions.insert(minimize, {window.id, false});
                windowActions.insert(close, {window.id, true});
            }
            QAction *picked = menu.exec(mapToGlobal(pos));
            if (picked == remove) {
                QStringList ids;
                for (int j = 0; j < pins_.size(); ++j) if (j != i) ids << pins_[j].id;
                savePins(ids);
                loadPins();
                update();
            } else if (windowActions.contains(picked)) {
                const auto command = windowActions.value(picked);
                if (command.second) sendClientMessage(command.first, netWmCloseWindow_, CurrentTime, 2, 0);
                else { XIconifyWindow(display_, command.first, DefaultScreen(display_)); XFlush(display_); }
            }
            return;
        }
        for (const auto &entry : taskRects_) {
            if (!entry.first.contains(pos)) continue;
            const Window id = entry.second;
            auto it = std::find_if(windows_.cbegin(), windows_.cend(), [id](const ClientWindow &w) { return w.id == id; });
            if (it == windows_.cend()) return;
            const ClientWindow w = *it;
            QMenu taskMenu(this);
            QAction *pinAction = nullptr;
            if (!w.desktopId.isEmpty()) pinAction = taskMenu.addAction("Fixar no Cisma Pulse");
            QAction *minimize = taskMenu.addAction("Minimizar");
            QAction *close = taskMenu.addAction("Fechar janela");
            QAction *picked = taskMenu.exec(mapToGlobal(pos));
            if (picked == pinAction && pinAction) {
                QStringList ids;
                for (const auto &pin : pins_) ids << pin.id;
                if (!ids.contains(w.desktopId)) ids << w.desktopId;
                savePins(ids);
                loadPins();
                update();
            } else if (picked == minimize) {
                XIconifyWindow(display_, id, DefaultScreen(display_)); XFlush(display_);
            } else if (picked == close) {
                sendClientMessage(id, netWmCloseWindow_, CurrentTime, 2, 0);
            }
            return;
        }
        QAction *style = menu.addAction("Cisma Taskbar...");
        QAction *tabs = menu.addAction("Voltar à barra por abas");
        QAction *picked = menu.exec(mapToGlobal(pos));
        if (picked == style) QProcess::startDetached("cisma-taskbar", {});
        else if (picked == tabs) QProcess::startDetached("cisma-taskbar-mode", {"--set", "tabs"});
    }
};
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Cisma Pulse");
    CismaPulse pulse;
    if (!pulse.ready()) {
        QMessageBox::critical(nullptr, "Cisma Pulse", "Não consegui conectar ao X11. O Cisma Pulse funciona na sessão MATE/Xorg.");
        return 1;
    }
    QDir().mkpath(runtimeDir());
    QFile pidFile(runtimeDir() + "/cisma-pulse.pid");
    if (pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        pidFile.write(QByteArray::number(getpid()) + "\n");
        pidFile.close();
    }
    pulse.show();
    return app.exec();
}
