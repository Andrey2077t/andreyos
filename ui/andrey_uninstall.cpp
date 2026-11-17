// Desinstalar Programas - Andrey OS
// UI em C++ / Qt6 (Widgets + Svg).
// Tudo é desenhado numa tela-base de 1672x941 e escalado para o tamanho da janela,
// então o layout fica idêntico ao print em qualquer resolução.
//
// Dados e regras vêm do backend /usr/lib/andrey-os/andrey_uninstall.py (lista, plano de remoção, proteção do
// sistema); quem remove de verdade é o apt-helper, via pkexec. Esta janela não toca no sistema sozinha.
//
// Compilar:  g++ -std=c++17 -O2 -fPIC andrey_uninstall.cpp -o andrey-uninstall $(pkg-config --cflags --libs Qt6Widgets Qt6Svg)
// Uso:       andrey-uninstall [--dry-run]     (--dry-run: não remove nada, só simula)
// Teste fora do sistema instalado: ANDREY_LIB_DIR=pkg/andrey-os-base/root/usr/lib/andrey-os ./andrey-uninstall --dry-run

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFontMetricsF>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRadialGradient>
#include <QScreen>
#include <QSvgRenderer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <cmath>

// --dry-run: só simula (a remoção termina sem chamar o pkexec e a lista é refeita)
static bool gDryRun = false;

// pasta dos scripts do Andrey OS (ANDREY_LIB_DIR serve para testar a partir do repositório)
static QString libDir()
{
    const QByteArray e = qgetenv("ANDREY_LIB_DIR");
    return e.isEmpty() ? QStringLiteral("/usr/lib/andrey-os") : QString::fromLocal8Bit(e);
}

static const double BW = 1672.0, BH = 941.0;

// ----------------------------------------------------------------------------
// Dados
// ----------------------------------------------------------------------------
struct App {
    QString name, desc, size, version, date, longDesc, category, icon, iconPath, pkg;
    bool checked = false;
};

// Roda o backend (python) e devolve a saída. ok=false se não rodou ou saiu com erro.
static bool runBackend(const QStringList &args, QByteArray *out, QString *err, int timeoutMs = 60000)
{
    QProcess pr;
    pr.start("/usr/bin/python3", QStringList{"-I", libDir() + "/andrey_uninstall.py"} + args);
    if (!pr.waitForStarted(5000) || !pr.waitForFinished(timeoutMs)) {
        pr.kill();
        if (err) *err = QStringLiteral("o backend não respondeu");
        return false;
    }
    if (out) *out = pr.readAllStandardOutput();
    if (pr.exitStatus() != QProcess::NormalExit || pr.exitCode() != 0) {
        if (err) *err = QString::fromUtf8(pr.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

// Programas instalados que têm atalho no menu (o backend já tira o que é do sistema).
static QVector<App> loadApps(QString *error)
{
    QVector<App> a;
    QByteArray out;
    QString err;
    if (!runBackend({"list"}, &out, &err)) {
        if (error) *error = QStringLiteral("Não foi possível listar os programas.");
        return a;
    }
    const QJsonArray arr = QJsonDocument::fromJson(out).object().value("apps").toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        App x;
        x.pkg = o.value("pkg").toString();
        if (x.pkg.isEmpty())
            continue;
        x.name = o.value("name").toString();
        x.desc = o.value("desc").toString();
        x.size = o.value("size").toString();
        x.version = o.value("version").toString();
        x.date = o.value("date").toString();
        x.longDesc = o.value("long").toString();
        x.category = o.value("category").toString();
        x.icon = o.value("icon").toString();
        x.iconPath = o.value("iconpath").toString();
        a.push_back(x);
    }
    if (error) error->clear();
    return a;
}

// ----------------------------------------------------------------------------
// Ícones (SVG embutidos)
// ----------------------------------------------------------------------------
static const QHash<QString, QString> &glyphs()
{
    static const QHash<QString, QString> h = {
        {"trash", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path fill="@C" fill-rule="evenodd" d="M9 2.8h6l.9 1.7H21v2.4H3V4.5h5.1zM5.2 8.3h13.6l-1 12.2c-.1.8-.7 1.4-1.5 1.4H7.7c-.8 0-1.4-.6-1.5-1.4zM8.9 11v7.4h1.5V11zm2.35 0v7.4h1.5V11zm2.35 0v7.4h1.5V11z"/></svg>)svg"},
        {"gamepad", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path fill="@C" fill-rule="evenodd" d="M7.5 6.5h9a5.5 5.5 0 0 1 5.4 6.4l-.6 3.6a2.6 2.6 0 0 1-4.5 1.3L14.9 15.3H9.1l-1.9 2.5a2.6 2.6 0 0 1-4.5-1.3l-.6-3.6a5.5 5.5 0 0 1 5.4-6.4zM6.6 9.6v1.5H5.1v1.4h1.5V14H8v-1.5h1.5v-1.4H8V9.6zM16 11.3a1 1 0 1 0 2 0 1 1 0 1 0-2 0zm2.6 2a1 1 0 1 0 2 0 1 1 0 1 0-2 0z"/></svg>)svg"},
        {"palette", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path fill="@C" fill-rule="evenodd" d="M12 2.3c5.4 0 9.7 3.9 9.7 8.7 0 3.1-2.5 4.7-5 4.2-1.5-.3-2.3.9-1.6 2 .8 1.6-.3 3.4-2.2 3.4C6.7 20.6 2.3 16.6 2.3 11.5 2.3 6.5 6.6 2.3 12 2.3zM6.4 10.5a1.3 1.3 0 1 0 2.6 0 1.3 1.3 0 1 0-2.6 0zm3.2-3.7a1.3 1.3 0 1 0 2.6 0 1.3 1.3 0 1 0-2.6 0zm4.5.4a1.3 1.3 0 1 0 2.6 0 1.3 1.3 0 1 0-2.6 0zm2.7 3.6a1.3 1.3 0 1 0 2.6 0 1.3 1.3 0 1 0-2.6 0z"/></svg>)svg"},
        {"globe", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><g fill="none" stroke="@C" stroke-width="1.7" stroke-linecap="round"><circle cx="12" cy="12" r="9.2"/><ellipse cx="12" cy="12" rx="4" ry="9.2"/><path d="M2.8 12h18.4M4.4 7.3h15.2M4.4 16.7h15.2"/></g></svg>)svg"},
        {"gear", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path fill="@C" fill-rule="evenodd" d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 0 0 .12-.61l-1.92-3.32a.49.49 0 0 0-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.48.48 0 0 0-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58a.49.49 0 0 0-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"/></svg>)svg"},
        {"monitor", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><g fill="none" stroke="@C" stroke-width="2" stroke-linecap="round"><rect x="2.6" y="3.4" width="18.8" height="13" rx="1.8"/><path d="M8 20.8h8M12 16.4v4.4"/></g></svg>)svg"},
        {"cube", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path fill="@C" d="M12 2.2l8.6 4.8L12 11.8 3.4 7z"/><path fill="@C" fill-opacity=".7" d="M3.4 8.6L11.2 13v8.4L3.4 17z"/><path fill="@C" fill-opacity=".5" d="M20.6 8.6L12.8 13v8.4l7.8-4.4z"/></svg>)svg"},
        {"info", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path fill="@C" fill-rule="evenodd" d="M12 2a10 10 0 1 0 .01 0zM10.7 7.3a1.3 1.3 0 1 0 2.6 0 1.3 1.3 0 1 0-2.6 0zM10.9 10.4v7.4h2.2v-7.4z"/></svg>)svg"},
        {"warning", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path fill="#ee3b52" fill-rule="evenodd" d="M12 2.6c.7 0 1.4.4 1.8 1.1l8 14c.8 1.4-.2 3.2-1.8 3.2H4c-1.6 0-2.6-1.8-1.8-3.2l8-14c.4-.7 1.1-1.1 1.8-1.1zM10.7 8.6l.4 6.4h1.8l.4-6.4zM10.8 17.7a1.2 1.2 0 1 0 2.4 0 1.2 1.2 0 1 0-2.4 0z"/></svg>)svg"},
        {"search", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><g fill="none" stroke="@C" stroke-width="2" stroke-linecap="round"><circle cx="10.5" cy="10.5" r="6.6"/><path d="M15.6 15.6L21 21"/></g></svg>)svg"},
        {"x", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path d="M5 5L19 19M19 5L5 19" fill="none" stroke="@C" stroke-width="2.2" stroke-linecap="round"/></svg>)svg"},
        {"min", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path d="M4 12h16" fill="none" stroke="@C" stroke-width="2" stroke-linecap="round"/></svg>)svg"},
        {"max", R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><rect x="4.5" y="4.5" width="15" height="15" rx="1.2" fill="none" stroke="@C" stroke-width="2"/></svg>)svg"},
    };
    return h;
}

static QByteArray letterSvg(const QString &bg, const QString &ch)
{
    return QString("<svg viewBox='0 0 100 100' xmlns='http://www.w3.org/2000/svg'><rect width='100' height='100' rx='22' fill='%1'/>"
                   "<text x='50' y='69' font-size='56' font-weight='bold' font-family='sans-serif' text-anchor='middle' fill='#fff'>%2</text></svg>")
        .arg(bg, ch)
        .toUtf8();
}

static QByteArray appIconSvg(const QString &k)
{
    if (k == "chrome")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
<path d="M8.43 26A48 48 0 0 1 91.57 26L50 50Z" fill="#e8453c"/>
<path d="M91.57 26A48 48 0 0 1 50 98L50 50Z" fill="#fbbc05"/>
<path d="M50 98A48 48 0 0 1 8.43 26L50 50Z" fill="#34a853"/>
<circle cx="50" cy="50" r="25" fill="#fff"/><circle cx="50" cy="50" r="19.5" fill="#4285f4"/></svg>)svg";
    if (k == "firefox")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg"><defs>
<linearGradient id="f" x1="0.2" y1="0.95" x2="0.8" y2="0.05"><stop offset="0" stop-color="#ff9500"/><stop offset=".55" stop-color="#ff3750"/><stop offset="1" stop-color="#ffcb00"/></linearGradient>
<radialGradient id="p" cx="0.5" cy="0.45" r="0.6"><stop offset="0" stop-color="#a66bff"/><stop offset="1" stop-color="#4c2bd0"/></radialGradient></defs>
<circle cx="50" cy="54" r="43" fill="url(#f)"/><circle cx="50" cy="57" r="28" fill="url(#p)"/>
<path d="M60 6c7 8 4 16-4 21 11-1 20 6 15 14-6-3-12-2-15 3-5-8-5-20 4-38z" fill="#ffb12b"/>
<path d="M18 44c2-9 8-15 15-18-3 7-1 12 5 14-8 2-13 8-14 15z" fill="#ffd34d"/></svg>)svg";
    if (k == "vlc")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
<path d="M50 6L79 78H21z" fill="#ff8a00"/><path d="M50 6L79 78H50z" fill="#e57400"/>
<path d="M43 30h14l3 9H40z" fill="#fff"/><path d="M35 52h30l3 9H32z" fill="#fff"/>
<path d="M12 80h76l-7 12H19z" fill="#e9e9ee"/><path d="M12 80h76l-2 4H14z" fill="#cfd0d8"/></svg>)svg";
    if (k == "libreoffice")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
<path d="M24 6h36l20 20v64a4 4 0 0 1-4 4H24a4 4 0 0 1-4-4V10a4 4 0 0 1 4-4z" fill="#ececec"/>
<path d="M60 6l20 20H64a4 4 0 0 1-4-4z" fill="#9b9b9b"/>
<rect x="31" y="42" width="38" height="5" rx="2.5" fill="#b3b3b3"/><rect x="31" y="54" width="38" height="5" rx="2.5" fill="#b3b3b3"/>
<rect x="31" y="66" width="26" height="5" rx="2.5" fill="#b3b3b3"/></svg>)svg";
    if (k == "steam")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg"><defs>
<linearGradient id="s" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2b78b8"/><stop offset="1" stop-color="#10233a"/></linearGradient></defs>
<circle cx="50" cy="50" r="47" fill="url(#s)"/>
<path d="M52 42L38 59" stroke="#fff" stroke-width="7" stroke-linecap="round"/>
<circle cx="35" cy="67" r="11" fill="#fff"/><circle cx="35" cy="67" r="5" fill="#173a5e"/>
<circle cx="63" cy="38" r="15" fill="#fff"/><circle cx="63" cy="38" r="9" fill="#1a4270"/><circle cx="63" cy="38" r="5" fill="#fff"/></svg>)svg";
    if (k == "discord")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
<rect width="100" height="100" rx="26" fill="#5865f2"/>
<path d="M29 33c6-4 13-6 13-6l2 4c4-.7 8-.7 12 0l2-4s7 2 13 6c6 10 8 22 7 34-5 4-11 6-16 7l-3-5c2-.7 4-1.7 6-3-6 3-16 3-22 0 2 1.3 4 2.3 6 3l-3 5c-5-1-11-3-16-7-1-12 1-24 7-34z" fill="#fff"/>
<circle cx="39" cy="54" r="5.6" fill="#5865f2"/><circle cx="61" cy="54" r="5.6" fill="#5865f2"/></svg>)svg";
    if (k == "whatsapp")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
<circle cx="50" cy="50" r="47" fill="#25d366"/>
<path d="M50 20a30 30 0 0 0-25.7 45.4L20 80l15-4A30 30 0 1 0 50 20z" fill="#fff"/>
<path d="M50 26a24 24 0 1 1-12.4 44.5l-7.6 2 2-7.4A24 24 0 0 1 50 26z" fill="#25d366"/>
<path d="M41 39c1-2 3-2 4 0l2 4c.4 1 0 2-1 3 1.6 3.6 4 6 7.6 7.6 1-1 2-1.4 3-1l4 2c2 1 2 3 0 4.6-3 2.6-7 2-11.4-.2-6-3-10-8-11.6-13-.3-3 .5-5 3.4-7z" fill="#fff"/></svg>)svg";
    if (k == "gimp")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
<ellipse cx="46" cy="54" rx="36" ry="33" fill="#8c7d68"/><ellipse cx="46" cy="60" rx="24" ry="18" fill="#a89a82"/>
<circle cx="34" cy="42" r="10" fill="#fff"/><circle cx="58" cy="42" r="10" fill="#fff"/>
<circle cx="36" cy="43" r="4.5" fill="#222"/><circle cx="56" cy="43" r="4.5" fill="#222"/>
<path d="M62 66L92 84" stroke="#d9c7a3" stroke-width="7" stroke-linecap="round"/><path d="M60 64L52 70" stroke="#5a4a38" stroke-width="5" stroke-linecap="round"/></svg>)svg";
    if (k == "spotify")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg"><circle cx="50" cy="50" r="47" fill="#1db954"/>
<g fill="none" stroke="#fff" stroke-linecap="round"><path d="M24 36c18-6 38-4 54 5" stroke-width="8"/><path d="M27 52c15-5 31-3 44 4" stroke-width="7"/><path d="M30 66c12-3 24-2 34 3" stroke-width="6"/></g></svg>)svg";
    if (k == "telegram")
        return R"svg(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg"><circle cx="50" cy="50" r="47" fill="#2aabee"/>
<path d="M22 49l52-20c2.4-.9 4.5.6 3.7 4.2L69 71c-.6 2.7-2.2 3.3-4.5 2L52 63.8l-6 5.8c-.7.7-1.2 1.2-2.5 1.2l.9-12.6 23-20.8c1-.9-.2-1.4-1.5-.5L37.5 55 25.3 51.2c-2.6-.8-2.6-2.6.7-3.7z" fill="#fff"/></svg>)svg";
    if (k == "obs")
        return letterSvg("#302e31", "O");
    if (k == "blender")
        return letterSvg("#ea7600", "B");
    if (k == "audacity")
        return letterSvg("#1f4fcf", "A");
    if (k == "vscode")
        return letterSvg("#0a7acb", "V");
    if (k == "lutris")
        return letterSvg("#ff9900", "L");
    if (k == "wine")
        return letterSvg("#8c1d3a", "W");
    return letterSvg("#7a1830", "?");
}

// Nome do Icon= do .desktop -> desenho próprio daqui (só usado se o sistema não achar o arquivo do ícone)
static QString builtinKey(const QString &icon)
{
    static const QHash<QString, QString> m = {
        {"google-chrome", "chrome"}, {"firefox", "firefox"}, {"firefox-esr", "firefox"}, {"vlc", "vlc"},
        {"libreoffice-startcenter", "libreoffice"}, {"steam", "steam"}, {"discord", "discord"}, {"gimp", "gimp"},
        {"spotify", "spotify"}, {"spotify-client", "spotify"}, {"telegram", "telegram"}, {"org.telegram.desktop", "telegram"},
        {"obs", "obs"}, {"com.obsproject.Studio", "obs"}, {"blender", "blender"}, {"audacity", "audacity"},
        {"code", "vscode"}, {"lutris", "lutris"}, {"wine", "wine"}};
    return m.value(icon);
}

// Ícone do arquivo indicado pelo sistema (svg ou png); fica em cache. Devolve true se desenhou.
static bool drawFileIcon(QPainter &p, const QString &path, const QRectF &target)
{
    if (path.isEmpty())
        return false;
    if (path.endsWith(".svg", Qt::CaseInsensitive)) {
        static QHash<QString, QSvgRenderer *> cache;
        auto it = cache.find(path);
        if (it == cache.end()) {
            auto *r = new QSvgRenderer(path);
            if (!r->isValid()) { delete r; r = nullptr; }
            it = cache.insert(path, r);
        }
        if (!it.value())
            return false;
        it.value()->render(&p, target);
        return true;
    }
    static QHash<QString, QPixmap> pix;
    auto it = pix.find(path);
    if (it == pix.end())
        it = pix.insert(path, QPixmap(path));
    if (it.value().isNull())
        return false;
    p.drawPixmap(target, it.value(), QRectF(it.value().rect()));
    return true;
}

// Sem ícone: um quadrado com a inicial do programa (a cor sai do nome, para variar)
static QSvgRenderer *letterRenderer(const QString &name)
{
    static QHash<QString, QSvgRenderer *> cache;
    const QString ch = name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper();
    const uint h = qHash(name);
    static const char *cols[] = {"#7a1830", "#1f4fcf", "#0a7acb", "#ea7600", "#1db954", "#8c1d3a", "#5865f2", "#302e31"};
    const QString bg = cols[h % 8];
    const QString key = bg + ch;
    auto it = cache.find(key);
    if (it != cache.end())
        return it.value();
    QString safe = ch;
    safe.replace("&", "&amp;").replace("<", "&lt;");
    auto *r = new QSvgRenderer(letterSvg(bg, safe));
    cache.insert(key, r);
    return r;
}

static QSvgRenderer *glyphRenderer(const QString &key, const QString &color)
{
    static QHash<QString, QSvgRenderer *> cache;
    const QString id = key + "|" + color;
    auto it = cache.find(id);
    if (it != cache.end())
        return it.value();
    QString svg = glyphs().value(key);
    svg.replace("@C", color);
    auto *r = new QSvgRenderer(svg.toUtf8());
    cache.insert(id, r);
    return r;
}

static QSvgRenderer *appRenderer(const QString &key)
{
    static QHash<QString, QSvgRenderer *> cache;
    auto it = cache.find(key);
    if (it != cache.end())
        return it.value();
    auto *r = new QSvgRenderer(appIconSvg(key));
    cache.insert(key, r);
    return r;
}

// ----------------------------------------------------------------------------
// Janela
// ----------------------------------------------------------------------------
class UninstallWindow : public QWidget
{
public:
    UninstallWindow()
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setWindowTitle("Desinstalar Programas");
        setMinimumSize(836, 471);

        apps = loadApps(&loadError);
        cur = -1;

        search = new QLineEdit(this);
        search->setFrame(false);
        search->setPlaceholderText("Buscar por um programa...");
        search->setAttribute(Qt::WA_MacShowFocusRect, false);
        search->installEventFilter(this);
        connect(search, &QLineEdit::textChanged, this, [this] { scroll = 0; rebuild(); });

        rebuild();

        const QRect av = QGuiApplication::primaryScreen()->availableGeometry();
        double k0 = qMin(1.0, qMin(av.height() * 0.88 / BH, av.width() * 0.92 / BW));
        resize(qRound(BW * k0), qRound(BH * k0));
    }

protected:
    // ---------------------------------------------------------------- layout
    void resizeEvent(QResizeEvent *) override
    {
        K = qMin(width() / BW, height() / BH);
        ox = (width() - BW * K) / 2.0;
        oy = (height() - BH * K) / 2.0;

        QRectF r = mapRect(QRectF(446, 100, 700, 47));
        search->setGeometry(r.toRect());
        QFont f = font();
        f.setFamilies(families());
        f.setPixelSize(qMax(8, qRound(22 * K)));
        search->setFont(f);
        QPalette pal = search->palette();
        pal.setColor(QPalette::PlaceholderText, QColor("#b99aa1"));
        search->setPalette(pal);
        search->setStyleSheet("QLineEdit{background:transparent;border:none;color:#f4e6e8;"
                              "selection-background-color:#9b1228;}");
        update();
    }

    // -------------------------------------------------------------- desenho
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

        // fundo (preenche a janela toda)
        {
            QPainterPath bg;
            bg.addRoundedRect(QRectF(rect()), isMaximized() ? 0 : 30 * K, isMaximized() ? 0 : 30 * K);
            QLinearGradient g(0, 0, 0, height());
            g.setColorAt(0, QColor("#250410"));
            g.setColorAt(0.5, QColor("#1b0309"));
            g.setColorAt(1, QColor("#2b0612"));
            p.fillPath(bg, g);
            QRadialGradient rg1(0, height(), width() * 0.5);
            rg1.setColorAt(0, QColor(150, 10, 35, 60));
            rg1.setColorAt(1, QColor(150, 10, 35, 0));
            p.fillPath(bg, rg1);
            QRadialGradient rg2(width(), height(), width() * 0.5);
            rg2.setColorAt(0, QColor(150, 10, 35, 60));
            rg2.setColorAt(1, QColor(150, 10, 35, 0));
            p.fillPath(bg, rg2);
        }

        p.translate(ox, oy);
        p.scale(K, K);

        // moldura
        p.setPen(QPen(QColor("#6c1024"), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(15, 15, 1642, 911), 26, 26);

        drawTitleBar(p);
        drawSidebar(p);

        // divisor vertical
        p.setPen(QPen(QColor("#4a0d1a"), 1.5));
        p.drawLine(QPointF(357, 92), QPointF(357, 925));

        drawSearch(p);
        drawList(p);
        drawDetails(p);
        drawBottom(p);
    }

    // ------------------------------------------------------------- mouse
    void mouseMoveEvent(QMouseEvent *e) override
    {
        QPointF b = toBase(e->position());
        if (draggingThumb) {
            double ms = maxScroll();
            QRectF t = thumbRect();
            double free = sbTrack.height() - t.height();
            if (free > 0)
                scroll = qBound(0.0, (b.y() - dragOff - sbTrack.top()) / free * ms, ms);
            update();
            return;
        }
        Hit h = hit(b);
        if (h.kind != hov.kind || h.idx != hov.idx) {
            hov = h;
            update();
        }
        const bool clickable = h.kind != NONE && h.kind != TITLE && h.kind != TRACK;
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        setFocus();
        QPointF b = toBase(e->position());
        Hit h = hit(b);
        pressed = h;
        switch (h.kind) {
        case TITLE:
            if (windowHandle())
                windowHandle()->startSystemMove();
            break;
        case THUMB:
            draggingThumb = true;
            dragOff = b.y() - thumbRect().top();
            break;
        case TRACK: {
            QRectF t = thumbRect();
            scroll = qBound(0.0, scroll + (b.y() < t.top() ? -VH : VH), maxScroll());
            break;
        }
        case ROW:
            cur = vis[h.idx];
            break;
        case CHECK:
            cur = vis[h.idx];
            apps[cur].checked = !apps[cur].checked;
            break;
        default:
            break;
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        draggingThumb = false;
        Hit h = hit(toBase(e->position()));
        if (h.kind == pressed.kind && h.idx == pressed.idx) {
            switch (h.kind) {
            case MINB: showMinimized(); break;
            case MAXB: isMaximized() ? showNormal() : showMaximized(); break;
            case CLOSEB: close(); break;
            case CAT:
                cat = h.idx;
                scroll = 0;
                rebuild();
                break;
            case CANCEL: close(); break;
            case UNINST: doUninstall(); break;
            default: break;
            }
        }
        pressed = Hit();
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (hit(toBase(e->position())).kind == TITLE)
            isMaximized() ? showNormal() : showMaximized();
    }

    void leaveEvent(QEvent *) override
    {
        hov = Hit();
        update();
    }

    void wheelEvent(QWheelEvent *e) override
    {
        scroll = qBound(0.0, scroll - e->angleDelta().y() / 120.0 * RH * 1.5, maxScroll());
        update();
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        switch (e->key()) {
        case Qt::Key_Down: moveSel(1); break;
        case Qt::Key_Up: moveSel(-1); break;
        case Qt::Key_Space:
            if (cur >= 0) { apps[cur].checked = !apps[cur].checked; update(); }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter: doUninstall(); break;
        case Qt::Key_Escape: close(); break;
        default: QWidget::keyPressEvent(e);
        }
    }

    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == search) {
            if (e->type() == QEvent::FocusIn || e->type() == QEvent::FocusOut)
                update();
            if (e->type() == QEvent::KeyPress) {
                int k = static_cast<QKeyEvent *>(e)->key();
                if (k == Qt::Key_Down) { moveSel(1); return true; }
                if (k == Qt::Key_Up) { moveSel(-1); return true; }
            }
        }
        return QWidget::eventFilter(o, e);
    }

private:
    // ----------------------------------------------------------- constantes
    enum Kind { NONE, TITLE, MINB, MAXB, CLOSEB, CAT, ROW, CHECK, THUMB, TRACK, CANCEL, UNINST };
    struct Hit { int kind = NONE; int idx = -1; };

    const QRectF btnMin{1452, 22, 54, 46}, btnMax{1523, 22, 54, 46}, btnClose{1594, 22, 54, 46};
    const QRectF searchBox{371, 96, 813, 55};
    const QRectF listView{371, 178, 783, 574};
    const QRectF sbTrack{1169, 180, 16, 570};
    const QRectF panel{1201, 90, 444, 666};
    const QRectF warnBox{371, 795, 826, 113};
    const QRectF btnCancel{1221, 817, 197, 70}, btnUninst{1445, 817, 195, 70};
    const double RH = 72.0, VH = 574.0;

    const QStringList catNames{"Todos os Programas", "Jogos", "Multimídia", "Internet", "Utilitários", "Escritório", "Outros"};
    const QStringList catIcons{"trash", "gamepad", "palette", "globe", "gear", "monitor", "cube"};
    const double catY[7] = {142.5, 215, 280, 342, 404, 466, 527};

    // ------------------------------------------------------------- estado
    QVector<App> apps;
    QVector<int> vis;
    int cur = -1, cat = 0;
    double scroll = 0, K = 1, ox = 0, oy = 0, dragOff = 0;
    bool draggingThumb = false;
    Hit hov, pressed;
    QLineEdit *search = nullptr;
    QString loadError;

    // ------------------------------------------------------------ helpers
    static QStringList families() { return {"Noto Sans", "Segoe UI", "Cantarell", "DejaVu Sans", "Sans Serif"}; }

    static QFont F(int px, QFont::Weight w = QFont::Normal)
    {
        QFont f;
        f.setFamilies(families());
        f.setPixelSize(px);
        f.setWeight(w);
        return f;
    }

    QPointF toBase(QPointF d) const { return QPointF((d.x() - ox) / K, (d.y() - oy) / K); }
    QRectF mapRect(const QRectF &r) const { return QRectF(ox + r.x() * K, oy + r.y() * K, r.width() * K, r.height() * K); }

    QRectF catRect(int i) const { return QRectF(30, catY[i] - 33, 313, 66); }
    QRectF rowRect(int vi) const { return QRectF(listView.left(), listView.top() + vi * RH - scroll, listView.width(), RH); }
    double maxScroll() const { return qMax(0.0, vis.size() * RH - VH); }

    QRectF thumbRect() const
    {
        double ms = maxScroll();
        if (ms <= 0)
            return QRectF();
        double th = qMax(40.0, sbTrack.height() * VH / (VH + ms));
        double ty = sbTrack.top() + (sbTrack.height() - th) * scroll / ms;
        return QRectF(sbTrack.left(), ty, sbTrack.width(), th);
    }

    Hit hit(QPointF b) const
    {
        Hit h;
        auto set = [&](int k, int i = -1) { h.kind = k; h.idx = i; return h; };
        if (btnMin.contains(b)) return set(MINB);
        if (btnMax.contains(b)) return set(MAXB);
        if (btnClose.contains(b)) return set(CLOSEB);
        if (b.y() < 76) return set(TITLE);
        for (int i = 0; i < 7; ++i)
            if (catRect(i).contains(b)) return set(CAT, i);
        if (btnCancel.contains(b)) return set(CANCEL);
        if (btnUninst.contains(b)) return set(UNINST);
        if (thumbRect().contains(b)) return set(THUMB);
        if (maxScroll() > 0 && sbTrack.contains(b)) return set(TRACK);
        if (listView.contains(b)) {
            int vi = int((b.y() - listView.top() + scroll) / RH);
            if (vi >= 0 && vi < vis.size())
                return set(b.x() < 432 ? CHECK : ROW, vi);
        }
        return h;
    }

    void rebuild()
    {
        vis.clear();
        const QString q = search ? search->text().trimmed() : QString();
        for (int i = 0; i < apps.size(); ++i) {
            const App &a = apps[i];
            if (cat > 0 && a.category != catNames[cat]) continue;
            if (!q.isEmpty() && !a.name.contains(q, Qt::CaseInsensitive) && !a.desc.contains(q, Qt::CaseInsensitive)) continue;
            vis.push_back(i);
        }
        if (!vis.contains(cur))
            cur = vis.isEmpty() ? -1 : vis.first();
        scroll = qBound(0.0, scroll, maxScroll());
        update();
    }

    void moveSel(int d)
    {
        if (vis.isEmpty()) return;
        int vi = qBound(0, vis.indexOf(cur) + d, int(vis.size()) - 1);
        cur = vis[vi];
        double top = vi * RH;
        if (top < scroll) scroll = top;
        else if (top + RH > scroll + VH) scroll = top + RH - VH;
        update();
    }

    void text(QPainter &p, const QRectF &r, const QString &s, QFont f, const QColor &c,
              int flags = Qt::AlignLeft | Qt::AlignVCenter)
    {
        // encolhe a fonte se o texto não couber (fontes mais largas que o Noto Sans)
        QFontMetricsF fm(f);
        const double w = fm.horizontalAdvance(s);
        if (w > r.width() && r.width() > 0)
            f.setPixelSize(qMax(8, int(f.pixelSize() * r.width() / w)));
        p.setFont(f);
        p.setPen(c);
        p.drawText(r, flags, s);
    }

    void glyph(QPainter &p, const QString &key, const QString &color, const QPointF &center, double size)
    {
        glyphRenderer(key, color)->render(&p, QRectF(center.x() - size / 2, center.y() - size / 2, size, size));
    }

    // 1) ícone real do sistema  2) desenho próprio de apps conhecidos  3) inicial do nome
    void appIcon(QPainter &p, const App &a, const QPointF &center, double size)
    {
        const QRectF r(center.x() - size / 2, center.y() - size / 2, size, size);
        if (drawFileIcon(p, a.iconPath, r))
            return;
        const QString k = builtinKey(a.icon);
        if (!k.isEmpty())
            appRenderer(k)->render(&p, r);
        else
            letterRenderer(a.name)->render(&p, r);
    }

    // -------------------------------------------------------------- partes
    void drawTitleBar(QPainter &p)
    {
        glyph(p, "trash", "#e6cbd0", QPointF(58, 45), 38);
        text(p, QRectF(109, 24, 600, 46), "Desinstalar Programas", F(26, QFont::DemiBold), QColor("#f4e6e8"));

        struct B { QRectF r; const char *g; int kind; };
        const B bs[] = {{btnMin, "min", MINB}, {btnMax, "max", MAXB}, {btnClose, "x", CLOSEB}};
        for (const B &b : bs) {
            if (hov.kind == b.kind) {
                p.setPen(Qt::NoPen);
                p.setBrush(b.kind == CLOSEB ? QColor(200, 20, 45, 190) : QColor(255, 255, 255, 25));
                p.drawRoundedRect(b.r.adjusted(4, 2, -4, -2), 8, 8);
            }
            glyph(p, b.g, "#f1e4e6", b.r.center(), b.kind == MAXB ? 24 : 28);
        }
    }

    void drawSidebar(QPainter &p)
    {
        for (int i = 0; i < 7; ++i) {
            const bool sel = (cat == i);
            const bool hv = (hov.kind == CAT && hov.idx == i);
            if (sel) {
                QRectF r(30, catY[i] - 35.5, 313, 71);
                QLinearGradient g(r.topLeft(), r.bottomLeft());
                g.setColorAt(0, QColor("#82091f"));
                g.setColorAt(1, QColor("#6d0719"));
                p.setPen(QPen(QColor("#c8182f"), 2));
                p.setBrush(g);
                p.drawRoundedRect(r, 15, 15);
            } else if (hv) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 14));
                p.drawRoundedRect(QRectF(30, catY[i] - 31, 313, 62), 14, 14);
            }
            glyph(p, catIcons[i], sel ? "#ffffff" : "#dcd3d5", QPointF(70, catY[i]), 40);
            text(p, QRectF(118, catY[i] - 20, 220, 40), catNames[i], F(22, sel ? QFont::DemiBold : QFont::Normal),
                 QColor(sel ? "#ffffff" : "#f0e0e3"));
        }

        p.setPen(QPen(QColor("#4a0d1a"), 1.5));
        p.drawLine(QPointF(33, 570), QPointF(340, 570));

        glyph(p, "info", "#e6cbd0", QPointF(70, 617), 38);
        text(p, QRectF(114, 598, 220, 38), "Andrey OS", F(25, QFont::Bold), QColor("#f4e6e8"));
        text(p, QRectF(114, 640, 230, 28), "Mais controle.", F(20), QColor("#e4b9c0"));
        text(p, QRectF(114, 666, 230, 28), "Mais liberdade.", F(20), QColor("#e4b9c0"));

        // marca d'água (espiral estilo Debian)
        QPainterPath sp;
        bool first = true;
        for (double t = 0; t <= 1.0; t += 0.008) {
            double ang = 0.6 + t * 2 * M_PI * 1.55;
            double r = 10 + t * 92;
            QPointF pt(190 + r * std::cos(ang), 812 + r * std::sin(ang));
            if (first) sp.moveTo(pt); else sp.lineTo(pt);
            first = false;
        }
        p.setBrush(Qt::NoBrush);
        QPen pen(QColor(135, 14, 38, 80), 14);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawPath(sp);
    }

    void drawSearch(QPainter &p)
    {
        const bool focus = search && search->hasFocus();
        p.setBrush(QColor("#1a0207"));
        p.setPen(QPen(QColor(focus ? "#b3162d" : "#4e0a18"), focus ? 2 : 1.5));
        p.drawRoundedRect(searchBox, 9, 9);
        glyph(p, "search", "#e8d8db", QPointF(401, 124), 32);
    }

    void drawCheck(QPainter &p, const QRectF &r, bool on, bool hv)
    {
        p.setBrush(on ? QColor("#c93550") : QColor(0, 0, 0, 0));
        p.setPen(QPen(on ? QColor("#f3d6da") : QColor(hv ? "#b79097" : "#8d6a72"), 2));
        p.drawRoundedRect(r, 5, 5);
        if (on) {
            QPainterPath c;
            c.moveTo(r.left() + r.width() * 0.22, r.top() + r.height() * 0.52);
            c.lineTo(r.left() + r.width() * 0.42, r.top() + r.height() * 0.72);
            c.lineTo(r.left() + r.width() * 0.79, r.top() + r.height() * 0.29);
            QPen pen(QColor("#ffffff"), 3.4);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            p.setBrush(Qt::NoBrush);
            p.setPen(pen);
            p.drawPath(c);
        }
    }

    void drawList(QPainter &p)
    {
        p.save();
        p.setClipRect(listView);
        for (int vi = 0; vi < vis.size(); ++vi) {
            QRectF r = rowRect(vi);
            if (r.bottom() < listView.top() || r.top() > listView.bottom()) continue;
            const App &a = apps[vis[vi]];
            const bool sel = (vis[vi] == cur);
            const bool hv = (hov.idx == vi && (hov.kind == ROW || hov.kind == CHECK));

            if (sel) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#76061a"));
                p.drawRoundedRect(r, 4, 4);
            } else if (hv) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 12));
                p.drawRoundedRect(r, 4, 4);
            }
            drawCheck(p, QRectF(390, r.center().y() - 14, 28, 28), a.checked, hov.kind == CHECK && hov.idx == vi);
            appIcon(p, a, QPointF(472, r.center().y()), 58);
            text(p, QRectF(531, r.top() + 8, 480, 32), a.name, F(22, QFont::DemiBold), QColor("#ffffff"));
            text(p, QRectF(531, r.top() + 36, 480, 28), a.desc, F(19), QColor("#d3b9bf"));
            text(p, QRectF(960, r.top() + 10, 156, 52), a.size, F(21), QColor("#eddde0"),
                 Qt::AlignRight | Qt::AlignVCenter);
        }
        if (vis.isEmpty())
            text(p, listView, loadError.isEmpty() ? QStringLiteral("Nenhum programa encontrado") : loadError, F(22),
                 QColor("#b99aa1"), Qt::AlignCenter);
        p.restore();

        // barra de rolagem
        if (maxScroll() > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 10));
            p.drawRoundedRect(sbTrack, 7, 7);
            p.setBrush(QColor(hov.kind == THUMB || draggingThumb ? "#8a4350" : "#6a2f3b"));
            p.drawRoundedRect(thumbRect().adjusted(1, 0, -1, 0), 6, 6);
        }
        p.setPen(QPen(QColor("#3a0813"), 1.5));
        p.drawLine(QPointF(359, 757), QPointF(1196, 757));
    }

    void drawDetails(QPainter &p)
    {
        p.setBrush(QColor("#1c0308"));
        p.setPen(QPen(QColor("#5b0c1c"), 1.5));
        p.drawRoundedRect(panel, 18, 18);

        if (cur < 0) {
            text(p, panel, "Selecione um programa", F(22), QColor("#b99aa1"), Qt::AlignCenter);
            return;
        }
        const App &a = apps[cur];
        appIcon(p, a, QPointF(1430, 207), 152);
        text(p, QRectF(1215, 302, 430 - 14, 48), a.name, F(31, QFont::Bold), QColor("#ffffff"), Qt::AlignCenter);
        text(p, QRectF(1215, 350, 430 - 14, 36), a.desc, F(22), QColor("#ead9dc"), Qt::AlignCenter);

        p.setPen(QPen(QColor("#4a0d1a"), 1.5));
        p.drawLine(QPointF(1222, 425), QPointF(1625, 425));

        const QString labels[3] = {"Versão:", "Tamanho:", "Instalado em:"};
        const QString values[3] = {a.version, a.size, a.date};
        for (int i = 0; i < 3; ++i) {
            double y = 464 + i * 40;
            text(p, QRectF(1222, y - 17, 160, 34), labels[i], F(23), QColor("#dcc6ca"));
            text(p, QRectF(1383, y - 17, 250, 34), values[i], F(23), QColor("#f6eaec"));
        }

        p.drawLine(QPointF(1222, 587), QPointF(1625, 587));
        p.setFont(F(24));
        p.setPen(QColor("#e9d6da"));
        p.drawText(QRectF(1222, 606, 408, 140), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, a.longDesc);
    }

    void drawBottom(QPainter &p)
    {
        // aviso
        p.setBrush(QColor("#1a0207"));
        p.setPen(QPen(QColor("#5a0a1a"), 1.5));
        p.drawRoundedRect(warnBox, 9, 9);
        glyph(p, "warning", "#ee3b52", QPointF(431, 851), 76);
        text(p, QRectF(503, 808, 600, 30), "Atenção!", F(23, QFont::Bold), QColor("#f7e8ea"));
        text(p, QRectF(503, 838, 680, 28), "A desinstalação de um programa não pode ser desfeita.", F(19), QColor("#e8d3d7"));
        text(p, QRectF(503, 863, 680, 28), "Certifique-se de que deseja remover o programa selecionado.", F(19), QColor("#e8d3d7"));

        // cancelar
        const bool hc = hov.kind == CANCEL;
        p.setBrush(QColor(hc ? "#2b0510" : "#200308"));
        p.setPen(QPen(QColor(hc ? "#8c1a30" : "#5e0d1c"), 1.5));
        p.drawRoundedRect(btnCancel, 9, 9);
        glyph(p, "x", "#f1e4e6", QPointF(1267, 852), 28);
        text(p, QRectF(1297, 830, 120, 44), "Cancelar", F(22), QColor("#f4e6e8"));

        // desinstalar
        const bool hu = hov.kind == UNINST;
        const bool pr = pressed.kind == UNINST && hu;
        QLinearGradient g(btnUninst.topLeft(), btnUninst.bottomLeft());
        g.setColorAt(0, QColor(pr ? "#a90f27" : hu ? "#d41a36" : "#c3142e"));
        g.setColorAt(1, QColor(pr ? "#8a0b1f" : hu ? "#b30f27" : "#a50e24"));
        p.setBrush(g);
        p.setPen(QPen(QColor("#e53a52"), 1.8));
        p.drawRoundedRect(btnUninst, 9, 9);
        glyph(p, "trash", "#ffffff", QPointF(1481, 852), 36);
        text(p, QRectF(1509, 830, 130, 44), "Desinstalar", F(22, QFont::DemiBold), QColor("#ffffff"));
    }

    // ---------------------------------------------------------------- ação
    void styleBox(QMessageBox &m)
    {
        m.setStyleSheet("QMessageBox{background:#1f0309;} QLabel{color:#f4e6e8;font-size:14px;}"
                        "QPushButton{background:#a50e24;color:#fff;border:1px solid #e53a52;border-radius:7px;padding:7px 22px;}"
                        "QPushButton:hover{background:#c3142e;}");
    }

    void info(QMessageBox::Icon ic, const QString &text)
    {
        QMessageBox m(ic, "Desinstalar Programas", text, QMessageBox::Ok, this);
        styleBox(m);
        m.exec();
    }

    // Roda a remoção com o pkexec (pede a senha) sem congelar a janela. Devolve o código de saída.
    int runRemoval(const QStringList &pkgs, QString *errText)
    {
        if (gDryRun)
            return 0;
        QMessageBox busy(QMessageBox::Information, "Desinstalar Programas",
                         "Removendo… isso pode pedir sua senha e levar alguns instantes.", QMessageBox::NoButton, this);
        styleBox(busy);
        busy.setStandardButtons(QMessageBox::NoButton);
        busy.show();

        QProcess pr;
        setEnabled(false);
        pr.start("pkexec", QStringList{libDir() + "/apt-helper", "remove"} + pkgs);
        const bool started = pr.waitForStarted(10000);
        // mantém a janela viva (repinta, mexe) enquanto o apt trabalha; sem limite de tempo: remover pode demorar
        while (started && pr.state() != QProcess::NotRunning) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            pr.waitForFinished(100);
        }
        setEnabled(true);
        busy.close();

        if (!started) {
            *errText = QStringLiteral("Não consegui chamar o pkexec.");
            return 127;
        }
        QString msg = QString::fromUtf8(pr.readAllStandardError()).trimmed();
        if (msg.isEmpty())
            msg = QString::fromUtf8(pr.readAllStandardOutput()).trimmed();
        if (msg.size() > 600)
            msg = "…" + msg.right(600);
        *errText = msg;
        return pr.exitStatus() == QProcess::NormalExit ? pr.exitCode() : 1;
    }

    void doUninstall()
    {
        QStringList pkgs, nomes;
        for (const App &a : apps)
            if (a.checked) { pkgs << a.pkg; nomes << a.name; }

        if (pkgs.isEmpty()) {
            info(QMessageBox::Information, "Marque pelo menos um programa na lista.");
            return;
        }

        // o backend simula a remoção: diz o que sairia junto e recusa se atingir o sistema
        QByteArray out;
        QString err;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        runBackend(QStringList{"plan"} + pkgs, &out, &err, 120000);
        QApplication::restoreOverrideCursor();
        const QJsonObject plan = QJsonDocument::fromJson(out).object();
        if (plan.isEmpty()) {
            info(QMessageBox::Warning, "Não consegui verificar o que a remoção afetaria.\nNada foi removido.");
            return;
        }
        if (!plan.value("ok").toBool()) {
            const QString why = plan.value("error").toString();
            info(QMessageBox::Warning, "Não dá para remover essa seleção.\n\n"
                                       + (why.isEmpty() ? QStringLiteral("O apt recusou a operação.") : why)
                                       + "\n\nNada foi removido.");
            return;
        }
        QStringList extra;
        for (const QJsonValue &v : plan.value("extra").toArray())
            extra << v.toString();

        QString txt = QString("Remover %1 programa(s)?\n\n%2").arg(pkgs.size()).arg(nomes.join(", "));
        if (!extra.isEmpty()) {
            const int total = int(extra.size());
            if (total > 12)
                extra = extra.mid(0, 12) << QString("…e mais %1").arg(total - 12);
            txt += "\n\nTambém serão removidos (dependem deles):\n" + extra.join(", ");
        }
        txt += "\n\nEsta ação não pode ser desfeita.";
        QMessageBox m(QMessageBox::Warning, "Desinstalar Programas", txt, QMessageBox::NoButton, this);
        QPushButton *ok = m.addButton("Desinstalar", QMessageBox::AcceptRole);
        m.addButton("Cancelar", QMessageBox::RejectRole);
        styleBox(m);
        m.exec();
        if (m.clickedButton() != ok)
            return;

        QString errText;
        const int rc = runRemoval(pkgs, &errText);

        if (rc == 0) {
            if (gDryRun)
                apps.erase(std::remove_if(apps.begin(), apps.end(), [](const App &a) { return a.checked; }), apps.end());
            else
                apps = loadApps(&loadError);   // o que realmente saiu (inclui o que foi junto)
            cur = -1;
            rebuild();
            info(QMessageBox::Information, gDryRun ? QStringLiteral("Simulação: nada foi removido de verdade.")
                                                   : QStringLiteral("Programa(s) removido(s)."));
            return;
        }
        // pkexec: 126 = senha cancelada/não autorizado, 127 = erro ao autenticar
        if (rc == 126 || rc == 127) {
            info(QMessageBox::Information, "Remoção cancelada. Nada foi alterado.");
            return;
        }
        info(QMessageBox::Critical, "A remoção falhou.\n\n" + (errText.isEmpty() ? QStringLiteral("(sem detalhes)") : errText));
        apps = loadApps(&loadError);
        cur = -1;
        rebuild();
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QGuiApplication::setApplicationName("andrey-uninstall");
    QGuiApplication::setDesktopFileName("andrey-uninstall");
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == "--dry-run")
            gDryRun = true;
    UninstallWindow w;
    w.show();
    return app.exec();
}