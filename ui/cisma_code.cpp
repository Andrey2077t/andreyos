// Cisma Code - editor de codigo do Cisma OS, em C++ / Qt6 (Widgets)
//
// O visual e o Display UI original do Cisma Code (tema vinho/vermelho). Aqui ele virou um editor de verdade:
//   - abas de arquivos, explorador de pastas, buscar nos arquivos, Git e painel Executar
//   - editor com numeros de linha, destaque de sintaxe, guias de indentacao, auto-indentar, fechar parenteses/aspas,
//     autocompletar, localizar/substituir, comentar linha, mover/duplicar/excluir linhas, zoom (Ctrl+roda)
//   - painel inferior: PROBLEMAS (Python/JSON), SAIDA (executar com F5), CONSOLE (Python), TERMINAL e PORTAS
//   - instancia unica: abrir um arquivo com o editor ja aberto so traz a janela para a frente
//
//   cisma-code [ARQUIVO[:LINHA] | PASTA ...]     abre arquivos e/ou uma pasta
//   cisma-code --version | --help
//   cisma-code --grab ARQ.png [ARQUIVO ...]      grava a janela num PNG e sai (testes)
//   cisma-code --selftest                        testa o editor sem tela (QT_QPA_PLATFORM=offscreen)
//
// Sem Q_OBJECT de proposito: o arquivo compila com um unico g++ (sem moc), igual aos outros apps Qt do Cisma OS.
//
// Compilar:
//   g++ -std=c++17 -O2 -fPIC -Wall -Wextra cisma_code.cpp -o cisma-code $(pkg-config --cflags --libs Qt6Widgets Qt6Network)

#include <QtWidgets>
#include <QtNetwork>
#include <algorithm>
#include <cmath>
#include <functional>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <cstdlib>
#include <cstring>
#include <deque>

static const char *APP_VERSION = "1.0.0";

static QColor hx(unsigned v, int a = 255) { return QColor((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, a); }

// ---------- Paleta (a mesma do Display UI) ----------
namespace Pal {
static const QColor BG_TITLE = hx(0x2c0510), BG_ACT = hx(0x330611), BG_SIDE = hx(0x2b050e), BG_EDITOR = hx(0x1f0408),
                    BG_PANEL = hx(0x240509), BG_STATUS = hx(0x3a0612), BG_TABBAR = hx(0x1a0306), BG_SEL = hx(0x5a0b1c),
                    ACCENT = hx(0xc4122f), TXT = hx(0xe8dcdc), TXT_DIM = hx(0xb9a3a7), ICON_DIM = hx(0xa84a5a),
                    KW = hx(0xff5a6a), FN = hx(0xf0e060), STR = hx(0xf2c2a0), GREEN = hx(0x7ccf6a), LINE_NUM = hx(0x9a8a8e),
                    COMMENT = hx(0x8f7a80), NUM = hx(0xf0a35e), TYPE = hx(0x6ec6d9), PRE = hx(0xd68fe0);
}

static QString cssColor(const QColor &c) { return c.name(QColor::HexRgb); }

// ============================================================================
//  Linguagens
// ============================================================================
enum class Kind { Code, Html, Markdown, Plain };

struct Lang {
    QString id, name;
    Kind kind = Kind::Code;
    QStringList exts;                // extensoes (sem ponto) ou nomes de arquivo inteiros, em minusculas
    QSet<QString> kw, types, defKw;  // palavras-chave, tipos/builtins, palavras apos as quais vem um nome (def, class...)
    QString lineC, blockS, blockE, quotes = QStringLiteral("\"'");
    QString keyChars;                // chave seguida de um destes (YAML ':', INI '=') vira "propriedade"
    bool tripleQ = false, backtick = false, preproc = false, tabs = false, ci = false, json = false, css = false;
    bool shellVars = false, decorators = false;
    QString run;                     // comando para F5; %f arquivo, %d pasta, %b nome sem extensao
};

static QSet<QString> S(const char *words) {
    const QStringList l = QString::fromLatin1(words).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return QSet<QString>(l.begin(), l.end());
}

static const QList<Lang> &allLangs() {
    static QList<Lang> L;
    if (!L.isEmpty()) return L;
    auto add = [&](Lang l) { L.append(l); };
    const char *jsKw = "break case catch class const continue debugger default delete do else export extends finally for function if "
                       "import in instanceof let new of return super switch this throw try typeof var void while with yield async await "
                       "static get set null undefined true false";
    const char *jsTypes = "console window document Math JSON Object Array String Number Boolean Promise Map Set Date RegExp Error "
                          "require module process exports";
    {
        Lang l; l.id = "python"; l.name = "Python"; l.exts = {"py", "pyw", "pyi"};
        l.kw = S("False None True and as assert async await break class continue def del elif else except finally for from global if "
                 "import in is lambda nonlocal not or pass raise return try while with yield match case");
        l.types = S("int str float bool list dict set tuple bytes object type print len range self cls super open isinstance enumerate "
                    "zip map filter sorted min max sum abs input Exception ValueError TypeError KeyError");
        l.defKw = S("def class"); l.lineC = "#"; l.tripleQ = true; l.decorators = true;
        l.run = "python3 -u \"%f\""; add(l);
    }
    {
        Lang l; l.id = "javascript"; l.name = "JavaScript"; l.exts = {"js", "mjs", "cjs", "jsx"};
        l.kw = S(jsKw); l.types = S(jsTypes); l.defKw = S("function class"); l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/";
        l.quotes = "\"'`"; l.backtick = true; l.run = "node \"%f\""; add(l);
    }
    {
        Lang l; l.id = "typescript"; l.name = "TypeScript"; l.exts = {"ts", "tsx", "mts", "cts"};
        l.kw = S(jsKw) + S("interface type enum implements namespace declare readonly private public protected abstract as is keyof");
        l.types = S(jsTypes) + S("string number boolean any unknown never void"); l.defKw = S("function class interface type enum");
        l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/"; l.quotes = "\"'`"; l.backtick = true; l.decorators = true; add(l);
    }
    {
        Lang l; l.id = "json"; l.name = "JSON"; l.exts = {"json", "jsonc", "webmanifest"}; l.kw = S("true false null");
        l.quotes = "\""; l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/"; l.json = true; add(l);
    }
    {
        Lang l; l.id = "html"; l.name = "HTML"; l.kind = Kind::Html; l.exts = {"html", "htm", "xhtml", "xml", "svg", "xsl", "vue"};
        l.run = "xdg-open \"%f\""; add(l);
    }
    {
        Lang l; l.id = "css"; l.name = "CSS"; l.exts = {"css", "scss", "less"}; l.blockS = "/*"; l.blockE = "*/"; l.css = true;
        l.kw = S("important"); l.run = "xdg-open \"%f\""; add(l);
    }
    {
        Lang l; l.id = "c"; l.name = "C"; l.exts = {"c", "h"};
        l.kw = S("auto break case char const continue default do double else enum extern float for goto if inline int long register "
                 "restrict return short signed sizeof static struct switch typedef union unsigned void volatile while NULL true false");
        l.types = S("size_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t FILE bool");
        l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/"; l.preproc = true;
        l.run = "gcc \"%f\" -o \"/tmp/cisma-code-%b\" && \"/tmp/cisma-code-%b\""; add(l);
    }
    {
        Lang l; l.id = "cpp"; l.name = "C++"; l.exts = {"cpp", "cc", "cxx", "hpp", "hh", "hxx", "ino"};
        l.kw = S("alignas alignof and asm auto bool break case catch char class const constexpr const_cast continue decltype default "
                 "delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable "
                 "namespace new noexcept nullptr operator or private protected public register reinterpret_cast return short signed "
                 "sizeof static static_assert static_cast struct switch template this throw true try typedef typeid typename union "
                 "unsigned using virtual void volatile while override final");
        l.types = S("std string vector map set unordered_map size_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t "
                    "shared_ptr unique_ptr optional array QString QWidget QObject");
        l.defKw = S("class struct enum namespace"); l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/"; l.preproc = true;
        l.run = "g++ -std=c++17 -O2 \"%f\" -o \"/tmp/cisma-code-%b\" && \"/tmp/cisma-code-%b\""; add(l);
    }
    {
        Lang l; l.id = "java"; l.name = "Java"; l.exts = {"java"};
        l.kw = S("abstract assert boolean break byte case catch char class const continue default do double else enum extends final "
                 "finally float for goto if implements import instanceof int interface long native new package private protected public "
                 "return short static strictfp super switch synchronized this throw throws transient try void volatile while true false null var record");
        l.types = S("String Integer Long Double Boolean List Map Set Object System ArrayList HashMap Exception");
        l.defKw = S("class interface enum"); l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/"; l.decorators = true;
        l.run = "java \"%f\""; add(l);
    }
    {
        Lang l; l.id = "kotlin"; l.name = "Kotlin"; l.exts = {"kt", "kts"};
        l.kw = S("as break class continue do else false for fun if in interface is null object package return super this throw true try "
                 "typealias val var when while by catch constructor finally import init abstract companion const data enum final inner "
                 "internal lateinit open operator override private protected public sealed suspend vararg");
        l.types = S("Int String Boolean Long Double Float Char Unit Any Nothing List Map Set Array");
        l.defKw = S("fun class interface object"); l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/"; l.decorators = true; add(l);
    }
    {
        Lang l; l.id = "lua"; l.name = "Lua"; l.exts = {"lua"};
        l.kw = S("and break do else elseif end false for function goto if in local nil not or repeat return then true until while");
        l.types = S("print require pairs ipairs table string math os io type tostring tonumber setmetatable getmetatable pcall error "
                    "assert outputChatBox addEventHandler triggerServerEvent triggerClientEvent dxDrawRectangle dxDrawText getElementData setElementData");
        l.defKw = S("function"); l.lineC = "--"; l.blockS = "--[["; l.blockE = "]]"; l.run = "lua \"%f\""; add(l);
    }
    {
        Lang l; l.id = "shell"; l.name = "Shell"; l.exts = {"sh", "bash", "zsh", "bashrc", ".bashrc", "profile", ".profile", "zshrc"};
        l.kw = S("if then else elif fi for while until do done case esac in function select time return exit break continue local "
                 "export readonly declare unset shift source alias");
        l.types = S("echo cd ls cat grep sed awk printf read test mkdir rm cp mv chmod sudo apt git curl");
        l.defKw = S("function"); l.lineC = "#"; l.shellVars = true; l.run = "bash \"%f\""; add(l);
    }
    {
        Lang l; l.id = "rust"; l.name = "Rust"; l.exts = {"rs"};
        l.kw = S("as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub "
                 "ref return self Self static struct super trait true type unsafe use where while");
        l.types = S("i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box Some None Ok Err");
        l.defKw = S("fn struct enum trait"); l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/";
        l.run = "rustc \"%f\" -o \"/tmp/cisma-code-%b\" && \"/tmp/cisma-code-%b\""; add(l);
    }
    {
        Lang l; l.id = "go"; l.name = "Go"; l.exts = {"go"};
        l.kw = S("break case chan const continue default defer else fallthrough for func go goto if import interface map package range "
                 "return select struct switch type var true false nil");
        l.types = S("int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 uintptr float32 float64 string bool byte rune error any fmt");
        l.defKw = S("func type"); l.lineC = "//"; l.blockS = "/*"; l.blockE = "*/"; l.quotes = "\"'`"; l.backtick = true; l.tabs = true;
        l.run = "go run \"%f\""; add(l);
    }
    {
        Lang l; l.id = "sql"; l.name = "SQL"; l.exts = {"sql"}; l.ci = true;
        l.kw = S("select from where insert into values update set delete create table drop alter add join left right inner outer on group "
                 "by order having limit as and or not null primary key foreign references index unique default distinct union all case "
                 "when then else end like in is between exists");
        l.types = S("int integer text varchar char real float boolean date datetime timestamp blob");
        l.lineC = "--"; l.blockS = "/*"; l.blockE = "*/"; add(l);
    }
    {
        Lang l; l.id = "markdown"; l.name = "Markdown"; l.kind = Kind::Markdown; l.exts = {"md", "markdown"}; add(l);
    }
    {
        Lang l; l.id = "yaml"; l.name = "YAML"; l.exts = {"yml", "yaml"}; l.kw = S("true false null yes no"); l.lineC = "#"; l.keyChars = ":"; add(l);
    }
    {
        Lang l; l.id = "ini"; l.name = "INI / TOML"; l.exts = {"ini", "toml", "cfg", "conf", "desktop", "service"}; l.kw = S("true false");
        l.lineC = "#"; l.keyChars = "="; add(l);
    }
    {
        Lang l; l.id = "make"; l.name = "Makefile"; l.exts = {"makefile", "mk", "gnumakefile"};
        l.kw = S("ifeq ifneq ifdef ifndef else endif include define endef"); l.lineC = "#"; l.tabs = true; l.shellVars = true; add(l);
    }
    {
        Lang l; l.id = "plain"; l.name = "Texto"; l.kind = Kind::Plain; l.exts = {"txt", "log", "text"}; add(l);
    }
    return L;
}

static const Lang *langById(const QString &id) {
    for (const Lang &l : allLangs()) if (l.id == id) return &l;
    return nullptr;
}
static const Lang *plainLang() { return langById("plain"); }

static const Lang *langForFile(const QString &path) {
    const QString base = QFileInfo(path).fileName().toLower();
    const QString ext = QFileInfo(path).suffix().toLower();
    for (const Lang &l : allLangs())
        if (l.exts.contains(base)) return &l;
    for (const Lang &l : allLangs())
        if (!ext.isEmpty() && l.exts.contains(ext)) return &l;
    return plainLang();
}

// ============================================================================
//  Icones desenhados em codigo (nada de arquivo de imagem)
// ============================================================================
static QPixmap newPm(int s, qreal dpr = 2.0) {
    QPixmap pm(int(s * dpr), int(s * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    return pm;
}

static QPainterPath logoPath(qreal cx, qreal top, qreal w, qreal h) {
    const qreal l = cx - w / 2, r = cx + w / 2, b = top + h;
    QPainterPath p;
    p.moveTo(cx, top);
    p.lineTo(r, b);
    p.lineTo(cx + w * 0.22, b);
    p.lineTo(cx + w * 0.1, b - h * 0.18);
    p.lineTo(cx - w * 0.05, b - h * 0.18);
    p.lineTo(cx - w * 0.12, b - h * 0.45);
    p.lineTo(cx - w * 0.30, b);
    p.lineTo(l, b);
    p.closeSubpath();
    return p;
}

static QIcon logoIcon() {
    static QIcon ic;
    if (ic.isNull()) {
        for (int s : {16, 24, 32, 48, 64, 128}) {
            QPixmap pm = newPm(s, 1.0);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(hx(0xed1226));
            p.drawPath(logoPath(s / 2.0, s * 0.06, s * 0.92, s * 0.88));
            p.end();
            ic.addPixmap(pm);
        }
    }
    return ic;
}

enum ActKind { ActFiles, ActSearch, ActGit, ActRun, ActGear };
static QPixmap actPixmap(ActKind k, const QColor &c) {
    QPixmap pm = newPm(28);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(c, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (k) {
    case ActFiles:
        p.drawRoundedRect(QRectF(5.5, 7.5, 11, 14), 1.5, 1.5);
        p.drawPolyline(QPolygonF({QPointF(10, 4.5), QPointF(19, 4.5), QPointF(22, 7.5), QPointF(22, 19)}));
        break;
    case ActSearch:
        p.drawEllipse(QPointF(12.5, 12.5), 6.5, 6.5);
        p.drawLine(QPointF(17.5, 17.5), QPointF(23, 23));
        break;
    case ActGit:
        p.drawEllipse(QPointF(8, 6), 2.8, 2.8);
        p.drawEllipse(QPointF(8, 22), 2.8, 2.8);
        p.drawEllipse(QPointF(20, 11), 2.8, 2.8);
        p.drawLine(QPointF(8, 8.8), QPointF(8, 19.2));
        {
            QPainterPath cp;
            cp.moveTo(20, 13.8);
            cp.cubicTo(20, 18, 8, 15, 8, 19.2);
            p.drawPath(cp);
        }
        break;
    case ActRun:
        p.drawPolygon(QPolygonF({QPointF(9, 5.5), QPointF(22, 14), QPointF(9, 22.5)}));
        break;
    case ActGear:
        for (int i = 0; i < 8; ++i) {
            const double a = i * M_PI / 4;
            p.drawLine(QPointF(14 + 8.2 * std::cos(a), 14 + 8.2 * std::sin(a)), QPointF(14 + 11.5 * std::cos(a), 14 + 11.5 * std::sin(a)));
        }
        p.drawEllipse(QPointF(14, 14), 7.5, 7.5);
        p.drawEllipse(QPointF(14, 14), 3.2, 3.2);
        break;
    }
    return pm;
}
static QIcon actIcon(ActKind k) {
    QIcon ic;
    ic.addPixmap(actPixmap(k, Pal::ICON_DIM), QIcon::Normal, QIcon::Off);
    ic.addPixmap(actPixmap(k, Pal::ACCENT), QIcon::Normal, QIcon::On);
    return ic;
}

static QIcon folderIcon() {
    static QIcon ic;
    if (ic.isNull()) {
        QPixmap pm = newPm(18);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(hx(0x8fb4d8));
        QPainterPath path;
        path.moveTo(2, 4); path.lineTo(7, 4); path.lineTo(9, 6.5); path.lineTo(16, 6.5);
        path.lineTo(16, 14.5); path.lineTo(2, 14.5); path.closeSubpath();
        p.drawPath(path);
        ic = QIcon(pm);
    }
    return ic;
}

static QIcon fileTypeIcon(const QString &name) {
    static QHash<QString, QIcon> cache;
    const QString ext = QFileInfo(name).suffix().toLower();
    const QString key = ext.isEmpty() ? QFileInfo(name).fileName().toLower() : ext;
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return it.value();
    QPixmap pm = newPm(18);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    auto badge = [&](const QColor &bg, const QString &txt, const QColor &fg, int px) {
        p.setPen(Qt::NoPen); p.setBrush(bg);
        p.drawRoundedRect(QRectF(2, 2, 14, 14), 2, 2);
        QFont f; f.setBold(true); f.setPixelSize(px); p.setFont(f); p.setPen(fg);
        p.drawText(QRectF(2, 2, 14, 14), Qt::AlignCenter, txt);
    };
    if (ext == "py" || ext == "pyw") {
        p.setPen(Qt::NoPen);
        p.setBrush(hx(0x3b7bbf)); p.drawRoundedRect(QRectF(2.5, 2.5, 9.5, 8.5), 3, 3);
        p.setBrush(hx(0xffd43b)); p.drawRoundedRect(QRectF(6, 7, 9.5, 8.5), 3, 3);
        p.setBrush(Qt::white); p.drawEllipse(QPointF(5.5, 5), 1, 1); p.drawEllipse(QPointF(12.5, 13), 1, 1);
    } else if (ext == "js" || ext == "mjs" || ext == "cjs" || ext == "jsx") badge(hx(0xf0db4f), "JS", hx(0x222222), 8);
    else if (ext == "ts" || ext == "tsx") badge(hx(0x3178c6), "TS", Qt::white, 8);
    else if (ext == "json" || ext == "jsonc") { QFont f; f.setBold(true); f.setPixelSize(13); p.setFont(f); p.setPen(hx(0xf0c040)); p.drawText(QRectF(0, 0, 18, 17), Qt::AlignCenter, "{}"); }
    else if (ext == "md" || ext == "markdown") {
        p.setPen(QPen(hx(0x4fa8ff), 1.4)); p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(9, 9), 6.3, 6.3);
        p.drawLine(QPointF(9, 8), QPointF(9, 12.5)); p.setPen(Qt::NoPen); p.setBrush(hx(0x4fa8ff)); p.drawEllipse(QPointF(9, 5.7), 0.9, 0.9);
    } else if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "webp" || ext == "bmp" || ext == "ico") {
        p.setPen(QPen(hx(0x5aa6e8), 1.3)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(2.5, 3.5, 13, 11), 1.5, 1.5);
        p.setPen(Qt::NoPen); p.setBrush(hx(0x5aa6e8));
        p.drawPolygon(QPolygonF({QPointF(4, 13), QPointF(7.5, 8.5), QPointF(10, 11.5), QPointF(12, 9.5), QPointF(14.5, 13)}));
    } else if (ext == "html" || ext == "htm" || ext == "xml" || ext == "svg") badge(hx(0xe44d26), "<>", Qt::white, 8);
    else if (ext == "css" || ext == "scss" || ext == "less") badge(hx(0x2965f1), "#", Qt::white, 10);
    else if (ext == "c" || ext == "h") badge(hx(0x5c6bc0), "C", Qt::white, 9);
    else if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "hpp") badge(hx(0x00599c), "C+", Qt::white, 7);
    else if (ext == "java") badge(hx(0xe76f00), "J", Qt::white, 9);
    else if (ext == "kt" || ext == "kts") badge(hx(0x7f52ff), "K", Qt::white, 9);
    else if (ext == "lua") badge(hx(0x000080), "L", Qt::white, 9);
    else if (ext == "rs") badge(hx(0xb7410e), "R", Qt::white, 9);
    else if (ext == "go") badge(hx(0x00add8), "Go", Qt::white, 7);
    else if (ext == "sh" || ext == "bash" || ext == "zsh") badge(hx(0x3a8f3a), ">_", Qt::white, 7);
    else if (ext == "yml" || ext == "yaml" || ext == "toml" || ext == "ini") badge(hx(0x8a6d70), "=", Qt::white, 10);
    else {
        p.setPen(QPen(hx(0xb9a3a7), 1.3)); p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF({QPointF(4, 2.5), QPointF(10.5, 2.5), QPointF(14.5, 6.5), QPointF(14.5, 15.5), QPointF(4, 15.5)}));
        p.drawLine(QPointF(10.5, 2.5), QPointF(10.5, 6.5)); p.drawLine(QPointF(10.5, 6.5), QPointF(14.5, 6.5));
    }
    p.end();
    QIcon ic(pm);
    cache.insert(key, ic);
    return ic;
}

class CismaIcons : public QFileIconProvider {
public:
    QIcon icon(const QFileInfo &info) const override {
        if (info.isDir()) return folderIcon();
        return fileTypeIcon(info.fileName());
    }
    QIcon icon(IconType t) const override {
        if (t == Folder) return folderIcon();
        return fileTypeIcon(QString());
    }
};

// pastas que nunca entram em buscas, no Ir para arquivo e no explorador
static bool ignoredDirName(const QString &n) {
    static const QSet<QString> s = {".git", "__pycache__", "node_modules", ".venv", "venv", ".mypy_cache", ".pytest_cache", ".idea", ".gradle", "build"};
    return s.contains(n);
}

// ============================================================================
//  Destaque de sintaxe
// ============================================================================
class Highlighter : public QSyntaxHighlighter {
public:
    explicit Highlighter(QTextDocument *d) : QSyntaxHighlighter(d), m_lang(plainLang()) {
        auto mk = [](const QColor &c, bool italic = false, bool bold = false) {
            QTextCharFormat f; f.setForeground(c); f.setFontItalic(italic); if (bold) f.setFontWeight(QFont::Bold); return f;
        };
        fKw = mk(Pal::KW); fFn = mk(Pal::FN); fStr = mk(Pal::STR); fCom = mk(Pal::COMMENT, true);
        fNum = mk(Pal::NUM); fType = mk(Pal::TYPE); fPre = mk(Pal::PRE); fHead = mk(Pal::KW, false, true);
    }
    void setLang(const Lang *l) { m_lang = l ? l : plainLang(); rehighlight(); }

protected:
    void highlightBlock(const QString &t) override {
        switch (m_lang->kind) {
        case Kind::Plain: setCurrentBlockState(0); break;
        case Kind::Html: hlHtml(t); break;
        case Kind::Markdown: hlMarkdown(t); break;
        default: hlCode(t); break;
        }
    }

private:
    const Lang *m_lang;
    QTextCharFormat fKw, fFn, fStr, fCom, fNum, fType, fPre, fHead;

    static bool isWord(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }
    int nextNonSpace(const QString &t, int i) const {
        while (i < t.size() && t[i].isSpace()) ++i;
        return i;
    }

    void hlCode(const QString &t) {
        const Lang &L = *m_lang;
        int state = std::max(previousBlockState(), 0);
        const int n = t.size();
        int i = 0;
        bool wantName = false;
        const bool dashIdent = L.css || !L.keyChars.isEmpty();

        if (L.preproc && state == 0) {
            int k = nextNonSpace(t, 0);
            if (k < n && t[k] == QLatin1Char('#')) {
                int e = nextNonSpace(t, k + 1);
                const int ws = e;
                while (e < n && isWord(t[e])) ++e;
                setFormat(k, e - k, fPre);
                if (t.mid(ws, e - ws) == QLatin1String("include")) {
                    const int a = t.indexOf(QLatin1Char('<'), e), b = a >= 0 ? t.indexOf(QLatin1Char('>'), a) : -1;
                    if (a >= 0 && b > a) setFormat(a, b - a + 1, fStr);
                }
                i = e;
            }
        }

        while (i < n) {
            if (state == 1) {  // comentario de bloco
                const int e = t.indexOf(L.blockE, i);
                if (e < 0) { setFormat(i, n - i, fCom); setCurrentBlockState(1); return; }
                setFormat(i, e + L.blockE.size() - i, fCom);
                i = e + L.blockE.size(); state = 0; continue;
            }
            if (state >= 2 && state <= 4) {  // string de varias linhas: """ ''' `
                const QString term = state == 2 ? QStringLiteral("\"\"\"") : state == 3 ? QStringLiteral("'''") : QStringLiteral("`");
                int j = i;
                bool found = false;
                while (j < n) {
                    if (t[j] == QLatin1Char('\\')) { j += 2; continue; }
                    if (t.mid(j, term.size()) == term) { found = true; break; }
                    ++j;
                }
                if (!found) { setFormat(i, n - i, fStr); setCurrentBlockState(state); return; }
                setFormat(i, j + term.size() - i, fStr);
                i = j + term.size(); state = 0; continue;
            }
            const QChar c = t[i];
            if (!L.blockS.isEmpty() && t.mid(i, L.blockS.size()) == L.blockS) {
                state = 1; setFormat(i, L.blockS.size(), fCom); i += L.blockS.size(); continue;
            }
            if (!L.lineC.isEmpty() && t.mid(i, L.lineC.size()) == L.lineC) {
                if (L.lineC != QLatin1String("#") || !L.css) { setFormat(i, n - i, fCom); setCurrentBlockState(0); return; }
            }
            if (L.quotes.contains(c)) {
                if (L.tripleQ && (c == QLatin1Char('"') || c == QLatin1Char('\'')) && t.mid(i, 3) == QString(3, c)) {
                    state = c == QLatin1Char('"') ? 2 : 3; setFormat(i, 3, fStr); i += 3; continue;
                }
                if (c == QLatin1Char('`') && L.backtick) { state = 4; setFormat(i, 1, fStr); ++i; continue; }
                int j = i + 1;
                while (j < n && t[j] != c) { if (t[j] == QLatin1Char('\\')) ++j; ++j; }
                j = std::min(j + 1, n);
                bool isKey = false;
                if (L.json) { const int k = nextNonSpace(t, j); isKey = k < n && t[k] == QLatin1Char(':'); }
                setFormat(i, j - i, isKey ? fFn : fStr);
                i = j; continue;
            }
            if (L.decorators && c == QLatin1Char('@') && i + 1 < n && t[i + 1].isLetter()) {
                int j = i + 1;
                while (j < n && (isWord(t[j]) || t[j] == QLatin1Char('.'))) ++j;
                setFormat(i, j - i, fPre); i = j; continue;
            }
            if (L.css) {
                if (c == QLatin1Char('@') && i + 1 < n && t[i + 1].isLetter()) {
                    int j = i + 1; while (j < n && (isWord(t[j]) || t[j] == QLatin1Char('-'))) ++j;
                    setFormat(i, j - i, fKw); i = j; continue;
                }
                if (c == QLatin1Char('#') && i + 1 < n && isWord(t[i + 1])) {
                    int j = i + 1; while (j < n && isWord(t[j])) ++j;
                    const int len = j - i - 1;
                    bool hex = len == 3 || len == 4 || len == 6 || len == 8;
                    for (int k = i + 1; k < j && hex; ++k) hex = QString::fromLatin1("0123456789abcdefABCDEF").contains(t[k]);
                    setFormat(i, j - i, hex ? fNum : fType); i = j; continue;
                }
                if (c == QLatin1Char('.') && i + 1 < n && t[i + 1].isLetter()) {
                    int j = i + 1; while (j < n && (isWord(t[j]) || t[j] == QLatin1Char('-'))) ++j;
                    setFormat(i, j - i, fType); i = j; continue;
                }
            }
            if (L.shellVars && c == QLatin1Char('$') && i + 1 < n) {
                int j = i + 1;
                if (t[j] == QLatin1Char('{') || t[j] == QLatin1Char('(')) {
                    const QChar close = t[j] == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(')');
                    const int e = t.indexOf(close, j); j = e < 0 ? n : e + 1;
                } else while (j < n && isWord(t[j])) ++j;
                setFormat(i, j - i, fType); i = j; continue;
            }
            if ((c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && t[i + 1].isDigit())) && (i == 0 || !isWord(t[i - 1]))) {
                int j = i;
                while (j < n && (t[j].isLetterOrNumber() || t[j] == QLatin1Char('.') || t[j] == QLatin1Char('_'))) ++j;
                setFormat(i, j - i, fNum); i = j; continue;
            }
            if (c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$')) {
                int j = i;
                while (j < n && (isWord(t[j]) || t[j] == QLatin1Char('$') || (dashIdent && t[j] == QLatin1Char('-') && j + 1 < n && isWord(t[j + 1])))) ++j;
                const QString w = t.mid(i, j - i);
                const QString key = L.ci ? w.toLower() : w;
                const int nx = nextNonSpace(t, j);
                if (wantName) { setFormat(i, j - i, fFn); wantName = false; }
                else if (L.kw.contains(key)) { setFormat(i, j - i, fKw); wantName = L.defKw.contains(key); }
                else if (L.types.contains(key)) setFormat(i, j - i, fType);
                else if (!L.keyChars.isEmpty() && nx < n && L.keyChars.contains(t[nx]) &&
                         (t.left(i).trimmed().isEmpty() || t.left(i).trimmed() == QLatin1String("-")))
                    setFormat(i, j - i, fFn);
                else if (L.css && nx < n && t[nx] == QLatin1Char(':') && !t.contains(QLatin1Char('{')) && t.left(i).trimmed().isEmpty())
                    setFormat(i, j - i, fFn);
                else if (nx < n && t[nx] == QLatin1Char('(') && !L.css) setFormat(i, j - i, fFn);
                i = j; continue;
            }
            ++i;
        }
        setCurrentBlockState(state);
    }

    void hlHtml(const QString &t) {
        int state = std::max(previousBlockState(), 0);
        const int n = t.size();
        int i = 0;
        while (i < n) {
            if (state == 1) {
                const int e = t.indexOf(QLatin1String("-->"), i);
                if (e < 0) { setFormat(i, n - i, fCom); setCurrentBlockState(1); return; }
                setFormat(i, e + 3 - i, fCom); i = e + 3; state = 0; continue;
            }
            const QChar c = t[i];
            if (state == 2) {  // dentro de uma tag
                if (c == QLatin1Char('>')) { setFormat(i, 1, fKw); ++i; state = 0; continue; }
                if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                    int j = t.indexOf(c, i + 1); if (j < 0) j = n - 1;
                    setFormat(i, j - i + 1, fStr); i = j + 1; continue;
                }
                if (c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char(':') || c == QLatin1Char('@')) {
                    int j = i;
                    while (j < n && (t[j].isLetterOrNumber() || QString::fromLatin1("_:-@.").contains(t[j]))) ++j;
                    setFormat(i, j - i, fFn); i = j; continue;
                }
                ++i; continue;
            }
            if (t.mid(i, 4) == QLatin1String("<!--")) { state = 1; setFormat(i, 4, fCom); i += 4; continue; }
            if (c == QLatin1Char('<') && i + 1 < n && (t[i + 1].isLetter() || QString::fromLatin1("/!?").contains(t[i + 1]))) {
                int j = i + 1;
                if (QString::fromLatin1("/!?").contains(t[j])) ++j;
                while (j < n && (t[j].isLetterOrNumber() || QString::fromLatin1("-:.").contains(t[j]))) ++j;
                setFormat(i, j - i, fKw); i = j; state = 2; continue;
            }
            if (c == QLatin1Char('&')) {
                const int e = t.indexOf(QLatin1Char(';'), i);
                if (e > i && e - i < 10) { setFormat(i, e - i + 1, fNum); i = e + 1; continue; }
            }
            ++i;
        }
        setCurrentBlockState(state);
    }

    void hlMarkdown(const QString &t) {
        const int prev = std::max(previousBlockState(), 0);
        const bool fence = t.trimmed().startsWith(QLatin1String("```"));
        if (fence) { setFormat(0, t.size(), fStr); setCurrentBlockState(prev == 1 ? 0 : 1); return; }
        if (prev == 1) { setFormat(0, t.size(), fStr); setCurrentBlockState(1); return; }
        setCurrentBlockState(0);
        const QString tr = t.trimmed();
        if (tr.startsWith(QLatin1Char('#'))) { setFormat(0, t.size(), fHead); return; }
        if (tr.startsWith(QLatin1Char('>'))) { setFormat(0, t.size(), fCom); return; }
        static const QRegularExpression list(QStringLiteral("^\\s*([-*+]|\\d+[.)])\\s"));
        const QRegularExpressionMatch m = list.match(t);
        if (m.hasMatch()) setFormat(m.capturedStart(1), m.capturedLength(1), fFn);
        static const QRegularExpression code(QStringLiteral("`[^`]+`")), bold(QStringLiteral("\\*\\*[^*]+\\*\\*")), link(QStringLiteral("\\[[^\\]]+\\]\\([^)]+\\)"));
        for (auto it = code.globalMatch(t); it.hasNext();) { const auto mm = it.next(); setFormat(mm.capturedStart(), mm.capturedLength(), fStr); }
        for (auto it = bold.globalMatch(t); it.hasNext();) { const auto mm = it.next(); setFormat(mm.capturedStart(), mm.capturedLength(), fType); }
        for (auto it = link.globalMatch(t); it.hasNext();) { const auto mm = it.next(); setFormat(mm.capturedStart(), mm.capturedLength(), fNum); }
    }
};

// ============================================================================
//  Editor de texto
// ============================================================================
class CodeEditor;

class LineNumbers : public QWidget {
public:
    explicit LineNumbers(CodeEditor *e);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
private:
    CodeEditor *ed;
};

// marca de ponto de parada: vai no "user data" do bloco, entao acompanha a linha quando o texto acima muda
struct BpData : QTextBlockUserData {};

// minimapa: o arquivo inteiro em miniatura (uma linha = 2 px, uma letra = 1 px), com a janela visivel marcada
class Minimap : public QWidget {
public:
    static constexpr int W = 100;
    explicit Minimap(CodeEditor *e);
    QSize sizeHint() const override { return QSize(W, 0); }
protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
private:
    CodeEditor *ed;
    int firstLine() const;
    int linesFit() const { return std::max(1, height() / 2); }
    void scrollToY(int y);
};

class CodeEditor : public QPlainTextEdit {
public:
    QString filePath;                 // vazio = arquivo novo ("Sem titulo")
    const Lang *lang = plainLang();
    bool crlf = false;
    QString enc = QStringLiteral("UTF-8");
    QDateTime diskTime;
    int tabSize = 4;
    bool useSpaces = true;
    std::function<void(int)> onZoom;  // +1 / -1 (Ctrl+roda)
    Highlighter *hl = nullptr;
    int debugLine = -1;                          // linha (1-based) onde o depurador parou; -1 = nenhuma
    std::function<void()> onBreakpointsChanged;  // pontos de parada mudaram
    std::function<void()> onCursorsChanged;      // numero de cursores mudou
    QList<QTextCursor> extra;                    // cursores adicionais (varios cursores); o principal e textCursor()

    explicit CodeEditor(QWidget *parent = nullptr) : QPlainTextEdit(parent) {
        hl = new Highlighter(document());
        numbers = new LineNumbers(this);
        minimap = new Minimap(this);
        minimapTimer.setSingleShot(true);
        minimapTimer.setInterval(150);
        connect(&minimapTimer, &QTimer::timeout, this, [this] { minimap->update(); });
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { minimap->update(); });
        connect(document(), &QTextDocument::contentsChanged, this, [this] { minimap->update(); minimapTimer.start(); });
        setFrameShape(QFrame::NoFrame);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setUndoRedoEnabled(true);
        setCursorWidth(2);
        setTabChangesFocus(false);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateMargin(); });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &r, int dy) {
            if (dy) numbers->scroll(0, dy); else numbers->update(0, r.y(), numbers->width(), r.height());
            if (r.contains(viewport()->rect())) updateMargin();
        });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { refreshSelections(); });
        comp = new QCompleter(this);
        cmodel = new QStringListModel(comp);
        comp->setModel(cmodel);
        comp->setWidget(this);
        comp->setCompletionMode(QCompleter::PopupCompletion);
        comp->setCaseSensitivity(Qt::CaseInsensitive);
        comp->setMaxVisibleItems(9);
        connect(comp, QOverload<const QString &>::of(&QCompleter::activated), this, [this](const QString &s) { insertCompletion(s); });
        updateMargin();
        refreshSelections();
    }

    void setLanguage(const Lang *l) {
        lang = l ? l : plainLang();
        hl->setLang(lang);
        if (lang->tabs) useSpaces = false;
    }
    void applyFont(const QFont &f) {
        setFont(f);
        applyTabStop();
        numbers->update();
        updateMargin();
    }
    void setTabSize(int n) { tabSize = n; applyTabStop(); viewport()->update(); }
    void applyTabStop() { setTabStopDistance(tabSize * fontMetrics().horizontalAdvance(QLatin1Char(' '))); }
    void setSearchSelections(const QList<QTextEdit::ExtraSelection> &l) { findSel = l; refreshSelections(); }

    QString indentUnit() const { return useSpaces ? QString(tabSize, QLatin1Char(' ')) : QStringLiteral("\t"); }

    int numbersWidth() const {
        int digits = 2;
        for (int m = std::max(1, blockCount()); m >= 100; m /= 10) ++digits;
        return 24 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    }
    void updateMargin() {
        setViewportMargins(numbersWidth(), 0, minimapOn ? Minimap::W : 0, 0);
        numbers->updateGeometry();
        layoutExtras();
    }
    void layoutExtras() {
        const QRect cr = contentsRect();
        numbers->setGeometry(QRect(cr.left(), cr.top(), numbersWidth(), cr.height()));
        minimap->setVisible(minimapOn);
        if (minimapOn) {
            const QRect vr = viewport()->geometry();
            minimap->setGeometry(QRect(vr.right() + 1, vr.top(), Minimap::W, vr.height()));
        }
    }
    void setMinimapVisible(bool on) { minimapOn = on; updateMargin(); }
    bool minimapVisible() const { return minimapOn; }

    void numbersPaint(QPaintEvent *ev) {
        QPainter p(numbers);
        p.fillRect(ev->rect(), hx(0x1c0407));
        QTextBlock b = firstVisibleBlock();
        int bn = b.blockNumber();
        int top = int(blockBoundingGeometry(b).translated(contentOffset()).top());
        int bottom = top + int(blockBoundingRect(b).height());
        const int cur = textCursor().blockNumber();
        p.setFont(font());
        while (b.isValid() && top <= ev->rect().bottom()) {
            if (b.isVisible() && bottom >= ev->rect().top()) {
                p.setPen(bn == cur ? Pal::TXT : Pal::LINE_NUM);
                p.drawText(0, top, numbers->width() - 10, fontMetrics().height(), Qt::AlignRight | Qt::AlignVCenter, QString::number(bn + 1));
                const int fh = fontMetrics().height();
                if (hasBreakpoint(b)) {
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(Qt::NoPen);
                    p.setBrush(hx(0xe5334a));
                    p.drawEllipse(QRectF(5, top + (fh - 10) / 2.0, 10, 10));
                }
                if (bn + 1 == debugLine) {
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(Qt::NoPen);
                    p.setBrush(Pal::FN);
                    const qreal y = top + fh / 2.0;
                    const QPointF tri[3] = {QPointF(3, y - 6), QPointF(3, y + 6), QPointF(15, y)};
                    p.drawPolygon(tri, 3);
                }
                p.setRenderHint(QPainter::Antialiasing, false);
            }
            b = b.next(); top = bottom; bottom = top + int(blockBoundingRect(b).height()); ++bn;
        }
    }
    // clique no gutter: a faixa da esquerda poe/tira ponto de parada, o resto seleciona a linha
    void gutterClick(int x, int y) {
        if (x < 18) { toggleBreakpoint(cursorForPosition(QPoint(0, y)).blockNumber() + 1); return; }
        numbersClick(y);
    }
    void numbersClick(int y) {
        QTextCursor c = cursorForPosition(QPoint(0, y));
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        setTextCursor(c);
    }

    // ---------- pontos de parada ----------
    static bool hasBreakpoint(const QTextBlock &b) { return dynamic_cast<BpData *>(b.userData()) != nullptr; }
    QList<int> breakpoints() const {   // linhas 1-based
        QList<int> l;
        for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) if (hasBreakpoint(b)) l << b.blockNumber() + 1;
        return l;
    }
    void setBreakpoints(const QList<int> &lines) {
        for (int ln : lines) {
            QTextBlock b = document()->findBlockByNumber(ln - 1);
            if (b.isValid()) b.setUserData(new BpData);
        }
        numbers->update();
        minimap->update();
    }
    void toggleBreakpoint(int line = -1) {
        QTextBlock b = line > 0 ? document()->findBlockByNumber(line - 1) : textCursor().block();
        if (!b.isValid()) return;
        if (hasBreakpoint(b)) b.setUserData(nullptr); else b.setUserData(new BpData);
        numbers->update();
        minimap->update();
        if (onBreakpointsChanged) onBreakpointsChanged();
    }
    void setDebugLine(int line) {
        debugLine = line;
        numbers->update();
        minimap->update();
        refreshSelections();
        if (line > 0) gotoLine(line, 1);
    }

    // ---------- varios cursores ----------
    int firstVisibleLine() const { return firstVisibleBlock().blockNumber(); }
    int cursorCount() const { return 1 + int(extra.size()); }
    void clearExtraCursors() {
        if (extra.isEmpty()) return;
        extra.clear();
        refreshSelections();
        viewport()->update();
        if (onCursorsChanged) onCursorsChanged();
    }
    static bool sameCursor(const QTextCursor &a, const QTextCursor &b) { return a.position() == b.position() && a.anchor() == b.anchor(); }
    // junta cursores iguais ou que se sobrepoem (o principal tem preferencia)
    void dedupeCursors(QTextCursor &prim) {
        struct It { QTextCursor c; bool pr; };
        QList<It> v;
        v.append({prim, true});
        for (const QTextCursor &x : extra) v.append({x, false});
        std::stable_sort(v.begin(), v.end(), [](const It &a, const It &b) { return a.c.selectionStart() < b.c.selectionStart(); });
        QList<It> out;
        for (const It &it : v) {
            if (!out.isEmpty()) {
                const It &last = out.last();
                const bool dup = it.c.selectionStart() < last.c.selectionEnd() ||
                                 (it.c.selectionStart() == last.c.selectionEnd() && !it.c.hasSelection() && !last.c.hasSelection());
                if (dup) { if (it.pr) out.last() = it; continue; }
            }
            out.append(it);
        }
        extra.clear();
        for (const It &it : out) { if (it.pr) prim = it.c; else extra.append(it.c); }
    }
    void finishMulti(QTextCursor &p) {
        dedupeCursors(p);
        setTextCursor(p);
        refreshSelections();
        viewport()->update();
        if (onCursorsChanged) onCursorsChanged();
    }
    // aplica a mesma edicao em todos os cursores, num unico "desfazer"; os cursores acompanham as mudancas sozinhos
    void editAll(const std::function<void(QTextCursor &, int)> &f) {
        QTextCursor p = textCursor();
        QVector<QTextCursor *> v;
        v << &p;
        for (QTextCursor &c : extra) v << &c;
        std::stable_sort(v.begin(), v.end(), [](QTextCursor *a, QTextCursor *b) { return a->selectionStart() < b->selectionStart(); });
        p.beginEditBlock();
        for (int i = 0; i < v.size(); ++i) f(*v[i], i);
        p.endEditBlock();
        finishMulti(p);
    }
    void moveAll(QTextCursor::MoveOperation op, bool keep) {
        QTextCursor p = textCursor();
        p.movePosition(op, keep ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        for (QTextCursor &c : extra) c.movePosition(op, keep ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        finishMulti(p);
        ensureCursorVisible();
    }
    void revealCursor(const QTextCursor &c) {
        const QRect r = cursorRect(c);
        const int fh = std::max(1, fontMetrics().height());
        QScrollBar *sb = verticalScrollBar();
        if (r.top() < 0) sb->setValue(sb->value() + std::min(-1, r.top() / fh));
        else if (r.bottom() > viewport()->height()) sb->setValue(sb->value() + (r.bottom() - viewport()->height()) / fh + 1);
    }
    void addCursorVertical(int dir) {   // Ctrl+Alt+Cima/Baixo
        QTextCursor ref = textCursor();
        for (const QTextCursor &c : extra)
            if (dir > 0 ? c.position() > ref.position() : c.position() < ref.position()) ref = c;
        QTextBlock nb = dir > 0 ? ref.block().next() : ref.block().previous();
        if (!nb.isValid()) return;
        QTextCursor nc(document());
        nc.setPosition(nb.position() + std::min(ref.positionInBlock(), nb.length() - 1));
        QTextCursor p = textCursor();
        if (sameCursor(nc, p)) return;
        for (const QTextCursor &c : extra) if (sameCursor(nc, c)) return;
        extra.append(nc);
        refreshSelections();
        revealCursor(nc);
        viewport()->update();
        if (onCursorsChanged) onCursorsChanged();
    }
    void addNextOccurrence() {          // Ctrl+D
        QTextCursor p = textCursor();
        if (!p.hasSelection()) {
            p.select(QTextCursor::WordUnderCursor);
            if (p.hasSelection()) setTextCursor(p);
            return;
        }
        const QString needle = p.selectedText();
        if (needle.contains(QChar::ParagraphSeparator)) return;
        int from = p.selectionEnd();
        for (const QTextCursor &c : extra) from = std::max(from, c.selectionEnd());
        QTextCursor f = document()->find(needle, from, QTextDocument::FindCaseSensitively);
        if (f.isNull()) f = document()->find(needle, 0, QTextDocument::FindCaseSensitively);
        if (f.isNull() || sameCursor(f, p)) return;
        for (const QTextCursor &c : extra) if (c.selectionStart() == f.selectionStart()) return;
        extra.append(p);
        setTextCursor(f);
        centerCursor();
        refreshSelections();
        if (onCursorsChanged) onCursorsChanged();
    }
    void selectAllOccurrences() {       // Ctrl+Shift+L
        QTextCursor p = textCursor();
        if (!p.hasSelection()) p.select(QTextCursor::WordUnderCursor);
        if (!p.hasSelection()) return;
        const QString needle = p.selectedText();
        if (needle.contains(QChar::ParagraphSeparator)) return;
        QList<QTextCursor> all;
        for (QTextCursor f = document()->find(needle, 0, QTextDocument::FindCaseSensitively); !f.isNull();
             f = document()->find(needle, f, QTextDocument::FindCaseSensitively)) {
            all.append(f);
            if (all.size() >= 5000) break;
        }
        if (all.isEmpty()) return;
        extra = all.mid(1);
        setTextCursor(all.first());
        refreshSelections();
        viewport()->update();
        if (onCursorsChanged) onCursorsChanged();
    }
    // tecla com varios cursores: devolve true se tratou
    bool multiKey(QKeyEvent *e) {
        const Qt::KeyboardModifiers m = e->modifiers();
        const bool ctrl = m & Qt::ControlModifier, alt = m & Qt::AltModifier, shift = m & Qt::ShiftModifier;
        const int k = e->key();
        if (k == Qt::Key_Shift || k == Qt::Key_Control || k == Qt::Key_Alt || k == Qt::Key_Meta || k == Qt::Key_AltGr || k == Qt::Key_CapsLock) return true;
        if (k == Qt::Key_Escape) { clearExtraCursors(); return true; }
        const QString txt = e->text();
        if (!txt.isEmpty() && txt[0].isPrint() && ((!ctrl && !alt) || (ctrl && alt))) {
            editAll([&](QTextCursor &c, int) { c.insertText(txt); });
            return true;
        }
        if (k == Qt::Key_Backspace && !alt) {
            editAll([&](QTextCursor &c, int) {
                if (c.hasSelection()) c.removeSelectedText();
                else if (ctrl) c.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor), c.removeSelectedText();
                else c.deletePreviousChar();
            });
            return true;
        }
        if (k == Qt::Key_Delete && !alt) {
            editAll([&](QTextCursor &c, int) {
                if (c.hasSelection()) c.removeSelectedText();
                else if (ctrl) c.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor), c.removeSelectedText();
                else c.deleteChar();
            });
            return true;
        }
        if ((k == Qt::Key_Return || k == Qt::Key_Enter) && !ctrl && !alt) {
            editAll([&](QTextCursor &c, int) {
                if (c.hasSelection()) c.removeSelectedText();
                const QString line = c.block().text();
                QString indent;
                for (QChar ch : line) { if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t')) indent += ch; else break; }
                const QString before = line.left(c.positionInBlock()).trimmed();
                const bool more = !before.isEmpty() && (QStringLiteral("{[(").contains(before[before.size() - 1]) ||
                                  (before.endsWith(QLatin1Char(':')) && (lang->id == QLatin1String("python") || lang->id == QLatin1String("yaml"))));
                c.insertText(QStringLiteral("\n") + indent.left(c.positionInBlock()) + (more ? indentUnit() : QString()));
            });
            return true;
        }
        if (k == Qt::Key_Tab && !ctrl && !alt) {
            editAll([&](QTextCursor &c, int) {
                if (useSpaces) c.insertText(QString(tabSize - (c.positionInBlock() % tabSize), QLatin1Char(' ')));
                else c.insertText(QStringLiteral("\t"));
            });
            return true;
        }
        QTextCursor::MoveOperation op = QTextCursor::NoMove;
        switch (k) {
        case Qt::Key_Left: op = ctrl ? QTextCursor::PreviousWord : QTextCursor::Left; break;
        case Qt::Key_Right: op = ctrl ? QTextCursor::NextWord : QTextCursor::Right; break;
        case Qt::Key_Up: op = QTextCursor::Up; break;
        case Qt::Key_Down: op = QTextCursor::Down; break;
        case Qt::Key_Home: op = QTextCursor::StartOfBlock; break;
        case Qt::Key_End: op = QTextCursor::EndOfBlock; break;
        default: break;
        }
        if (op != QTextCursor::NoMove && !alt) { moveAll(op, shift); return true; }
        if (ctrl && !alt && (k == Qt::Key_C || k == Qt::Key_X)) {
            QList<QTextCursor> all;
            all << textCursor();
            all << extra;
            std::sort(all.begin(), all.end(), [](const QTextCursor &a, const QTextCursor &b) { return a.selectionStart() < b.selectionStart(); });
            QStringList parts;
            for (const QTextCursor &c : all) if (c.hasSelection()) { QString t = c.selectedText(); t.replace(QChar::ParagraphSeparator, QLatin1Char('\n')); parts << t; }
            if (!parts.isEmpty()) QGuiApplication::clipboard()->setText(parts.join(QLatin1Char('\n')));
            if (k == Qt::Key_X) editAll([&](QTextCursor &c, int) { if (c.hasSelection()) c.removeSelectedText(); });
            return true;
        }
        if (ctrl && !alt && k == Qt::Key_V) {
            const QStringList lines = QGuiApplication::clipboard()->text().split(QLatin1Char('\n'));
            const bool spread = lines.size() == cursorCount();
            const QString whole = QGuiApplication::clipboard()->text();
            editAll([&](QTextCursor &c, int i) { c.insertText(spread ? lines[i] : whole); });
            return true;
        }
        return false;   // qualquer outra tecla: volta a um cursor so e segue o caminho normal
    }

    // ---------- operacoes de linha (usadas pelo menu Editar) ----------
    void blockRange(int &a, int &b) const {
        QTextCursor c = textCursor();
        const int s = c.selectionStart(), e = c.selectionEnd();
        QTextCursor t(document());
        t.setPosition(s); a = t.blockNumber();
        t.setPosition(e); b = t.blockNumber();
        if (e > s && t.atBlockStart()) --b;
        if (b < a) b = a;
    }
    void selectBlocks(int a, int b) {
        QTextCursor c(document());
        c.setPosition(document()->findBlockByNumber(a).position());
        QTextBlock lb = document()->findBlockByNumber(b);
        c.setPosition(lb.position() + lb.length() - 1, QTextCursor::KeepAnchor);
        setTextCursor(c);
    }
    void moveLines(int dir) {
        int a, b; blockRange(a, b);
        if ((dir < 0 && a == 0) || (dir > 0 && b >= blockCount() - 1)) return;
        QTextCursor c(document());
        c.beginEditBlock();
        QTextBlock first = document()->findBlockByNumber(a), last = document()->findBlockByNumber(b);
        if (dir < 0) {
            QTextBlock nb = first.previous();
            const QString ntext = nb.text();
            c.setPosition(nb.position()); c.setPosition(first.position(), QTextCursor::KeepAnchor); c.removeSelectedText();
            QTextBlock lastNew = document()->findBlockByNumber(b - 1);
            c.setPosition(lastNew.position() + lastNew.length() - 1);
            c.insertText(QStringLiteral("\n") + ntext);
        } else {
            QTextBlock nb = last.next();
            const QString ntext = nb.text();
            const int endLast = last.position() + last.length() - 1;
            c.setPosition(endLast); c.setPosition(nb.position() + nb.length() - 1, QTextCursor::KeepAnchor); c.removeSelectedText();
            c.setPosition(first.position());
            c.insertText(ntext + QStringLiteral("\n"));
        }
        c.endEditBlock();
        selectBlocks(a + dir, b + dir);
    }
    void duplicateLines(bool down) {
        int a, b; blockRange(a, b);
        QStringList lines;
        for (int i = a; i <= b; ++i) lines << document()->findBlockByNumber(i).text();
        QTextCursor c(document());
        QTextBlock last = document()->findBlockByNumber(b);
        c.setPosition(last.position() + last.length() - 1);
        c.insertText(QStringLiteral("\n") + lines.join(QLatin1Char('\n')));
        const int n = b - a + 1;
        if (down) selectBlocks(a + n, b + n); else selectBlocks(a, b);
    }
    void deleteLines() {
        int a, b; blockRange(a, b);
        QTextCursor c(document());
        c.beginEditBlock();
        QTextBlock first = document()->findBlockByNumber(a), last = document()->findBlockByNumber(b);
        if (b >= blockCount() - 1 && a > 0) {
            c.setPosition(first.position() - 1);
            c.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        } else {
            c.setPosition(first.position());
            c.setPosition(std::min(last.position() + last.length(), document()->characterCount() - 1), QTextCursor::KeepAnchor);
        }
        c.removeSelectedText();
        c.endEditBlock();
        setTextCursor(c);
    }
    void toggleComment() {
        QString ls = lang->lineC;
        int a, b; blockRange(a, b);
        QTextCursor c(document());
        c.beginEditBlock();
        if (ls.isEmpty()) {
            if (lang->kind == Kind::Html || lang->css || !lang->blockS.isEmpty()) {
                const QString bs = lang->kind == Kind::Html ? QStringLiteral("<!--") : lang->blockS;
                const QString be = lang->kind == Kind::Html ? QStringLiteral("-->") : lang->blockE;
                for (int i = a; i <= b; ++i) {
                    QTextBlock blk = document()->findBlockByNumber(i);
                    const QString t = blk.text();
                    if (t.trimmed().isEmpty()) continue;
                    c.setPosition(blk.position());
                    if (t.trimmed().startsWith(bs) && t.trimmed().endsWith(be)) {
                        const int s = t.indexOf(bs);
                        int e = t.lastIndexOf(be);
                        if (e > 0 && t[e - 1] == QLatin1Char(' ')) --e;  // tira o espaco antes do fechamento
                        c.setPosition(blk.position() + e); c.setPosition(blk.position() + t.lastIndexOf(be) + be.size(), QTextCursor::KeepAnchor); c.removeSelectedText();
                        c.setPosition(blk.position() + s); c.setPosition(blk.position() + s + bs.size() + (t.mid(s + bs.size(), 1) == QLatin1String(" ") ? 1 : 0), QTextCursor::KeepAnchor); c.removeSelectedText();
                    } else {
                        c.setPosition(blk.position() + blk.length() - 1); c.insertText(QStringLiteral(" ") + be);
                        int ws = 0;
                        while (ws < t.size() && t[ws].isSpace()) ++ws;
                        c.setPosition(blk.position() + ws); c.insertText(bs + QStringLiteral(" "));
                    }
                }
            }
            c.endEditBlock();
            return;
        }
        bool allCommented = true; int minIndent = 1 << 20; bool any = false;
        for (int i = a; i <= b; ++i) {
            const QString t = document()->findBlockByNumber(i).text();
            if (t.trimmed().isEmpty()) continue;
            any = true;
            if (!t.trimmed().startsWith(ls)) allCommented = false;
            int ind = 0;
            while (ind < t.size() && t[ind].isSpace()) ++ind;
            minIndent = std::min(minIndent, ind);
        }
        if (!any) { c.endEditBlock(); return; }
        for (int i = a; i <= b; ++i) {
            QTextBlock blk = document()->findBlockByNumber(i);
            const QString t = blk.text();
            if (t.trimmed().isEmpty()) continue;
            if (allCommented) {
                const int s = t.indexOf(ls);
                c.setPosition(blk.position() + s);
                c.setPosition(blk.position() + s + ls.size() + (t.mid(s + ls.size(), 1) == QLatin1String(" ") ? 1 : 0), QTextCursor::KeepAnchor);
                c.removeSelectedText();
            } else {
                c.setPosition(blk.position() + std::min(minIndent, int(t.size())));
                c.insertText(ls + QStringLiteral(" "));
            }
        }
        c.endEditBlock();
    }
    void indentSelection(bool inc) {
        int a, b; blockRange(a, b);
        QTextCursor c(document());
        c.beginEditBlock();
        for (int i = a; i <= b; ++i) {
            QTextBlock blk = document()->findBlockByNumber(i);
            const QString t = blk.text();
            c.setPosition(blk.position());
            if (inc) {
                if (!t.isEmpty()) c.insertText(indentUnit());
            } else {
                int rm = 0;
                if (t.startsWith(QLatin1Char('\t'))) rm = 1;
                else while (rm < tabSize && rm < t.size() && t[rm] == QLatin1Char(' ')) ++rm;
                if (rm) { c.setPosition(blk.position() + rm, QTextCursor::KeepAnchor); c.removeSelectedText(); }
            }
        }
        c.endEditBlock();
        selectBlocks(a, b);
    }
    void gotoLine(int line, int col = 1) {
        QTextBlock blk = document()->findBlockByNumber(std::max(0, std::min(line - 1, blockCount() - 1)));
        QTextCursor c(document());
        c.setPosition(blk.position() + std::max(0, std::min(col - 1, blk.length() - 1)));
        setTextCursor(c);
        centerCursor();
    }
    int indentOf(const QTextBlock &b) const {
        const QString t = b.text();
        int col = 0; bool blank = true;
        for (QChar ch : t) {
            if (ch == QLatin1Char(' ')) ++col;
            else if (ch == QLatin1Char('\t')) col += tabSize - (col % tabSize);
            else { blank = false; break; }
        }
        return blank ? -1 : col;
    }
    QString prefixAtCursor() const {
        const QTextCursor c = textCursor();
        const QString line = c.block().text();
        const int col = c.positionInBlock();
        int s = col;
        while (s > 0 && (line[s - 1].isLetterOrNumber() || line[s - 1] == QLatin1Char('_'))) --s;
        return line.mid(s, col - s);
    }

    // ---------- autocompletar ----------
    void showCompletion(bool force) {
        const QString pre = prefixAtCursor();
        if (!force && pre.size() < 2) { comp->popup()->hide(); return; }
        const int rev = document()->revision();
        const int pos = textCursor().position();
        if (rev != wordsRev || pos != wordsPos || wordsLang != lang) {
            QSet<QString> words;
            for (const QString &w : lang->kw) words.insert(w);
            for (const QString &w : lang->types) words.insert(w);
            if (document()->characterCount() < 300000) {
                static const QRegularExpression re(QStringLiteral("[A-Za-z_][A-Za-z0-9_]{2,}"));
                const QString all = toPlainText();
                for (auto it = re.globalMatch(all); it.hasNext();) {
                    const auto m = it.next();
                    if (m.capturedStart() <= pos && pos <= m.capturedEnd()) continue;  // a palavra que esta sendo digitada
                    words.insert(m.captured());
                }
            }
            QStringList l(words.begin(), words.end());
            std::sort(l.begin(), l.end(), [](const QString &x, const QString &y) { return x.compare(y, Qt::CaseInsensitive) < 0; });
            cmodel->setStringList(l);
            wordsRev = rev; wordsPos = pos; wordsLang = lang;
        }
        if (pre != comp->completionPrefix()) {
            comp->setCompletionPrefix(pre);
            comp->popup()->setCurrentIndex(comp->completionModel()->index(0, 0));
        }
        if (comp->completionCount() == 0 || (comp->completionCount() == 1 && comp->currentCompletion() == pre)) { comp->popup()->hide(); return; }
        QRect cr = cursorRect();
        cr.setWidth(comp->popup()->sizeHintForColumn(0) + comp->popup()->verticalScrollBar()->sizeHint().width() + 8);
        comp->complete(cr);
    }
    void insertCompletion(const QString &s) {
        QTextCursor c = textCursor();
        const int n = prefixAtCursor().size();
        c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, n);
        c.insertText(s);
        setTextCursor(c);
    }

protected:
    void resizeEvent(QResizeEvent *e) override {
        QPlainTextEdit::resizeEvent(e);
        layoutExtras();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)) {   // Alt+clique: mais um cursor
            const QTextCursor nc = cursorForPosition(e->position().toPoint());
            const QTextCursor p = textCursor();
            bool dup = sameCursor(nc, p);
            for (const QTextCursor &c : extra) if (sameCursor(c, nc)) dup = true;
            if (!dup) { extra.append(p); setTextCursor(nc); refreshSelections(); if (onCursorsChanged) onCursorsChanged(); }
            e->accept();
            return;
        }
        if (!extra.isEmpty()) clearExtraCursors();
        QPlainTextEdit::mousePressEvent(e);
    }

    void wheelEvent(QWheelEvent *e) override {
        if ((e->modifiers() & Qt::ControlModifier) && onZoom) {
            const int d = e->angleDelta().y();
            if (d != 0) onZoom(d > 0 ? 1 : -1);
            e->accept();
            return;
        }
        QPlainTextEdit::wheelEvent(e);
    }

    void paintEvent(QPaintEvent *e) override {
        QPlainTextEdit::paintEvent(e);
        QPainter p(viewport());
        for (const QTextCursor &c : extra) {   // cursores adicionais
            const QRect r = cursorRect(c);
            if (r.bottom() >= e->rect().top() && r.top() <= e->rect().bottom()) p.fillRect(r.x(), r.y(), 2, r.height(), Pal::TXT);
        }
        p.setPen(QColor(255, 255, 255, 26));
        const qreal sw = fontMetrics().horizontalAdvance(QLatin1Char(' '));
        const qreal x0 = contentOffset().x() + document()->documentMargin();
        QTextBlock b = firstVisibleBlock();
        qreal top = blockBoundingGeometry(b).translated(contentOffset()).top();
        const int vh = viewport()->height();
        int prevInd = 0;
        while (b.isValid() && top <= vh) {
            const qreal bottom = top + blockBoundingRect(b).height();
            if (b.isVisible() && bottom >= e->rect().top()) {
                int ind = indentOf(b);
                if (ind < 0) {
                    int nx = -1, guard = 0;
                    for (QTextBlock nb = b.next(); nb.isValid() && guard < 200 && nx < 0; nb = nb.next(), ++guard) nx = indentOf(nb);
                    ind = std::min(prevInd, std::max(nx, 0));
                } else prevInd = ind;
                for (int lv = 0; lv < ind; lv += tabSize) {
                    const qreal x = std::floor(x0 + lv * sw) + 0.5;
                    p.drawLine(QPointF(x, top), QPointF(x, bottom));
                }
            }
            top = bottom;
            b = b.next();
        }
    }

    void keyPressEvent(QKeyEvent *e) override {
        if (comp->popup()->isVisible()) {
            switch (e->key()) {
            case Qt::Key_Enter: case Qt::Key_Return: case Qt::Key_Escape: case Qt::Key_Tab: case Qt::Key_Backtab:
                e->ignore(); return;
            default: break;
            }
        }
        if (!extra.isEmpty()) {
            if (multiKey(e)) return;
            clearExtraCursors();
        }
        const Qt::KeyboardModifiers m = e->modifiers();
        const bool ctrl = m & Qt::ControlModifier, alt = m & Qt::AltModifier, shift = m & Qt::ShiftModifier;
        const int k = e->key();

        if (ctrl && k == Qt::Key_Space) { showCompletion(true); return; }
        if (isReadOnly()) { QPlainTextEdit::keyPressEvent(e); return; }

        if (k == Qt::Key_Tab && !ctrl && !alt) {
            QTextCursor c = textCursor();
            int a, b; blockRange(a, b);
            if (c.hasSelection() && a != b) { indentSelection(true); return; }
            if (useSpaces) c.insertText(QString(tabSize - (c.positionInBlock() % tabSize), QLatin1Char(' ')));
            else c.insertText(QStringLiteral("\t"));
            setTextCursor(c);
            return;
        }
        if (k == Qt::Key_Backtab) { indentSelection(false); return; }
        if ((k == Qt::Key_Return || k == Qt::Key_Enter) && !ctrl && !alt) { smartNewline(); return; }
        if (k == Qt::Key_Home && !ctrl && !alt) {
            QTextCursor c = textCursor();
            const QString line = c.block().text();
            int first = 0;
            while (first < line.size() && line[first].isSpace()) ++first;
            const int col = c.positionInBlock();
            const int target = (col == first || col == 0) ? (col == first ? 0 : first) : first;
            c.setPosition(c.block().position() + target, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
            setTextCursor(c);
            return;
        }
        if (k == Qt::Key_Backspace && !ctrl && !alt) {
            QTextCursor c = textCursor();
            if (!c.hasSelection()) {
                const QString line = c.block().text();
                const int col = c.positionInBlock();
                const QChar prev = col > 0 ? line[col - 1] : QChar(), next = col < line.size() ? line[col] : QChar();
                const QString pairs = QStringLiteral("()[]{}\"\"''``");
                if (!prev.isNull() && !next.isNull()) {
                    const int pi = pairs.indexOf(prev);
                    if (pi >= 0 && pi % 2 == 0 && pairs[pi + 1] == next) {
                        c.beginEditBlock(); c.deletePreviousChar(); c.deleteChar(); c.endEditBlock(); setTextCursor(c); return;
                    }
                }
                if (useSpaces && col > 0 && line.left(col).trimmed().isEmpty() && line[col - 1] == QLatin1Char(' ')) {
                    int n = col % tabSize == 0 ? tabSize : col % tabSize;
                    n = std::min(n, col);
                    c.beginEditBlock();
                    for (int i = 0; i < n; ++i) c.deletePreviousChar();
                    c.endEditBlock(); setTextCursor(c); return;
                }
            }
        }
        const QString txt = e->text();
        if (txt.size() == 1 && !ctrl && !alt && lang->kind != Kind::Plain && handleTyped(txt[0])) return;

        QPlainTextEdit::keyPressEvent(e);

        if (comp->popup()->isVisible() && k == Qt::Key_Backspace) { showCompletion(false); return; }
        if (!txt.isEmpty() && !ctrl && !alt && lang->kind != Kind::Plain && (txt[0].isLetterOrNumber() || txt[0] == QLatin1Char('_'))) showCompletion(false);
        else if (comp->popup()->isVisible() && !(k == Qt::Key_Shift || k == Qt::Key_Control || k == Qt::Key_Alt)) comp->popup()->hide();
    }

private:
    LineNumbers *numbers;
    Minimap *minimap = nullptr;
    QTimer minimapTimer;
    bool minimapOn = true;
    QCompleter *comp;
    QStringListModel *cmodel;
    QList<QTextEdit::ExtraSelection> findSel;
    int wordsRev = -1, wordsPos = -1;
    const Lang *wordsLang = nullptr;

    QChar charAt(int pos) const { return pos < 0 ? QChar() : document()->characterAt(pos); }

    bool handleTyped(QChar ch) {
        QTextCursor c = textCursor();
        const QString openers = QStringLiteral("([{"), closers = QStringLiteral(")]}");
        const bool isQuote = ch == QLatin1Char('"') || ch == QLatin1Char('\'') || ch == QLatin1Char('`');
        if (isQuote && !lang->quotes.contains(ch) && lang->kind != Kind::Html) return false;
        const QChar next = charAt(c.position()), prev = charAt(c.position() - 1);
        const int oi = openers.indexOf(ch);
        if ((oi >= 0 || isQuote) && c.hasSelection()) {  // envolve a selecao
            const QChar close = isQuote ? ch : closers[oi];
            const QString sel = c.selectedText();
            const int s = c.selectionStart();
            c.beginEditBlock();
            c.insertText(QString(ch) + sel + QString(close));
            c.setPosition(s + 1); c.setPosition(s + 1 + sel.size(), QTextCursor::KeepAnchor);
            c.endEditBlock();
            setTextCursor(c);
            return true;
        }
        if ((closers.contains(ch) || isQuote) && next == ch) {  // anda por cima do fechamento ja existente
            c.movePosition(QTextCursor::NextCharacter); setTextCursor(c); return true;
        }
        const bool nextOk = next.isNull() || next.isSpace() || QStringLiteral(")]}>,;:.").contains(next) || next == QChar::ParagraphSeparator;
        if (oi >= 0 && nextOk) {
            c.beginEditBlock(); c.insertText(QString(ch) + QString(closers[oi])); c.movePosition(QTextCursor::Left); c.endEditBlock();
            setTextCursor(c); return true;
        }
        if (isQuote && nextOk && !(prev.isLetterOrNumber() || prev == QLatin1Char('_') || prev == QLatin1Char('\\') || prev == ch)) {
            c.beginEditBlock(); c.insertText(QString(ch) + QString(ch)); c.movePosition(QTextCursor::Left); c.endEditBlock();
            setTextCursor(c); return true;
        }
        return false;
    }

    void smartNewline() {
        QTextCursor c = textCursor();
        c.beginEditBlock();
        if (c.hasSelection()) c.removeSelectedText();
        const QString line = c.block().text();
        const int col = c.positionInBlock();
        const QString before = line.left(col), after = line.mid(col);
        QString indent;
        for (QChar ch : line) { if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t')) indent += ch; else break; }
        if (indent.size() > col) indent = indent.left(col);
        const QString bt = before.trimmed();
        const QChar last = bt.isEmpty() ? QChar() : bt[bt.size() - 1];
        bool extra = QStringLiteral("{[(").contains(last) && !last.isNull();
        if (!extra && last == QLatin1Char(':') && (lang->id == QLatin1String("python") || lang->id == QLatin1String("yaml"))) extra = true;
        if (!extra && lang->id == QLatin1String("lua") && (bt.endsWith(QLatin1String("then")) || bt.endsWith(QLatin1String("do")) || bt.startsWith(QLatin1String("function")) || bt.contains(QRegularExpression(QStringLiteral("\\bfunction\\s*\\("))))) extra = true;
        const QString unit = indentUnit();
        const QString at = after.trimmed();
        const int closeIdx = QStringLiteral("{[(").indexOf(last);
        if (extra && !last.isNull() && closeIdx >= 0 && !at.isEmpty() && at[0] == QStringLiteral("}])")[closeIdx]) {
            const int start = c.position();
            c.insertText(QStringLiteral("\n") + indent + unit + QStringLiteral("\n") + indent);
            c.setPosition(start + 1 + indent.size() + unit.size());
        } else {
            c.insertText(QStringLiteral("\n") + indent + (extra ? unit : QString()));
        }
        c.endEditBlock();
        setTextCursor(c);
        ensureCursorVisible();
    }

    // ---------- selecoes extras: linha atual, parenteses combinando, achados da busca ----------
    void refreshSelections() {
        QList<QTextEdit::ExtraSelection> list = findSel;
        QTextEdit::ExtraSelection cur;
        cur.format.setBackground(QColor(255, 255, 255, 12));
        cur.format.setProperty(QTextFormat::FullWidthSelection, true);
        cur.cursor = textCursor();
        cur.cursor.clearSelection();
        list.prepend(cur);
        if (debugLine > 0) {   // linha onde o depurador parou
            QTextEdit::ExtraSelection d;
            d.format.setBackground(QColor(240, 224, 96, 46));
            d.format.setProperty(QTextFormat::FullWidthSelection, true);
            d.cursor = QTextCursor(document()->findBlockByNumber(debugLine - 1));
            if (d.cursor.block().isValid()) list.prepend(d);
        }
        for (const QTextCursor &xc : extra) {
            if (!xc.hasSelection()) continue;
            QTextEdit::ExtraSelection xs;
            xs.format.setBackground(QColor(196, 18, 47, 120));
            xs.cursor = xc;
            list.append(xs);
        }
        const int p = textCursor().position();
        const QString open = QStringLiteral("([{"), close = QStringLiteral(")]}");
        int at = -1;
        QChar ch = charAt(p);
        if (!ch.isNull() && (open.contains(ch) || close.contains(ch))) at = p;
        else { ch = charAt(p - 1); if (!ch.isNull() && (open.contains(ch) || close.contains(ch))) at = p - 1; }
        if (at >= 0) {
            const bool fwd = open.contains(ch);
            const QChar mate = fwd ? close[open.indexOf(ch)] : open[close.indexOf(ch)];
            int depth = 0, pos = at;
            const int limit = 30000;
            for (int step = 0; step < limit; ++step) {
                pos += fwd ? 1 : -1;
                if (pos < 0 || pos >= document()->characterCount()) break;
                const QChar x = charAt(pos);
                if (x == ch) ++depth;
                else if (x == mate) { if (depth == 0) {
                    for (int q : {at, pos}) {
                        QTextEdit::ExtraSelection s;
                        s.format.setBackground(QColor(196, 18, 47, 90));
                        s.cursor = QTextCursor(document());
                        s.cursor.setPosition(q); s.cursor.setPosition(q + 1, QTextCursor::KeepAnchor);
                        list.append(s);
                    }
                    break; } --depth; }
            }
        }
        setExtraSelections(list);
        numbers->update();
    }
};

inline LineNumbers::LineNumbers(CodeEditor *e) : QWidget(e), ed(e) {}
inline QSize LineNumbers::sizeHint() const { return QSize(ed->numbersWidth(), 0); }
inline void LineNumbers::paintEvent(QPaintEvent *ev) { ed->numbersPaint(ev); }
inline void LineNumbers::mousePressEvent(QMouseEvent *ev) { ed->gutterClick(int(ev->position().x()), int(ev->position().y())); }

inline Minimap::Minimap(CodeEditor *e) : QWidget(e), ed(e) { setMouseTracking(false); setCursor(Qt::ArrowCursor); }
inline int Minimap::firstLine() const {
    const int total = ed->blockCount(), fit = linesFit();
    if (total <= fit) return 0;
    QScrollBar *sb = ed->verticalScrollBar();
    const double frac = sb->maximum() > 0 ? double(sb->value()) / sb->maximum() : 0.0;
    return int((total - fit) * frac);
}
inline void Minimap::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), hx(0x1c0407));
    p.setPen(QColor(255, 255, 255, 20));
    p.drawLine(0, 0, 0, height());
    const int first = firstLine(), total = ed->blockCount(), last = std::min(total, first + linesFit() + 1);
    const int tab = std::max(1, ed->tabSize);
    QTextBlock b = ed->document()->findBlockByNumber(first);
    for (int i = first; i < last && b.isValid(); ++i, b = b.next()) {
        const int y = (i - first) * 2;
        const QString text = b.text();
        const int n = std::min<int>(text.size(), 200);
        QVector<QRgb> cols(n, 0);
        if (b.layout()) for (const auto &fr : b.layout()->formats()) {
            if (fr.format.foreground().style() == Qt::NoBrush) continue;
            const QRgb c = fr.format.foreground().color().rgba();
            for (int k = fr.start; k < std::min(n, fr.start + fr.length); ++k) cols[k] = c;
        }
        int x = 4;
        for (int j = 0; j < n && x < W - 2; ++j) {
            const QChar ch = text[j];
            if (ch == QLatin1Char('\t')) { x += tab; continue; }
            if (ch.isSpace()) { ++x; continue; }
            QColor c = cols[j] ? QColor::fromRgba(cols[j]) : Pal::TXT_DIM;
            c.setAlpha(205);
            p.fillRect(x, y, 1, 1, c);
            ++x;
        }
        if (CodeEditor::hasBreakpoint(b)) p.fillRect(0, y, 3, 2, hx(0xe5334a));
        if (i + 1 == ed->debugLine) p.fillRect(0, y, W, 2, QColor(240, 224, 96, 150));
    }
    // a janela visivel
    const int fv = ed->firstVisibleLine();
    const int vis = std::max(1, ed->viewport()->height() / std::max(1, ed->fontMetrics().height()));
    const int sy = (fv - first) * 2, sh = std::max(8, vis * 2);
    p.fillRect(QRect(1, sy, W - 1, sh), QColor(255, 255, 255, 30));
    p.setPen(QColor(255, 255, 255, 50));
    p.drawRect(QRect(1, sy, W - 2, sh));
}
inline void Minimap::scrollToY(int y) {
    const int line = std::max(0, std::min(ed->blockCount() - 1, firstLine() + y / 2));
    QScrollBar *sb = ed->verticalScrollBar();
    const int vis = std::max(1, ed->viewport()->height() / std::max(1, ed->fontMetrics().height()));
    if (ed->lineWrapMode() == QPlainTextEdit::NoWrap) sb->setValue(std::max(0, line - vis / 2));
    else sb->setValue(int(double(line) / std::max(1, ed->blockCount()) * sb->maximum()));
}
inline void Minimap::mousePressEvent(QMouseEvent *ev) { if (ev->button() == Qt::LeftButton) scrollToY(int(ev->position().y())); }
inline void Minimap::mouseMoveEvent(QMouseEvent *ev) { if (ev->buttons() & Qt::LeftButton) scrollToY(int(ev->position().y())); }
inline void Minimap::wheelEvent(QWheelEvent *ev) { ed->verticalScrollBar()->setValue(ed->verticalScrollBar()->value() - ev->angleDelta().y() / 40); }

// ============================================================================
//  Leitura / gravacao de arquivos de texto
// ============================================================================
struct LoadResult { bool ok = false; QString text, err, enc = QStringLiteral("UTF-8"); bool crlf = false; };

static LoadResult loadTextFile(const QString &path) {
    LoadResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { r.err = f.errorString(); return r; }
    QByteArray data = f.readAll();
    if (data.left(8192).contains('\0')) { r.err = QStringLiteral("o arquivo parece binário"); return r; }
    const bool bom = data.startsWith("\xEF\xBB\xBF");
    if (bom) data.remove(0, 3);
    QStringDecoder dec(QStringConverter::Utf8, QStringConverter::Flag::Stateless);  // Stateless: byte solto no fim tambem vira erro
    QString s = dec.decode(data);
    if (dec.hasError()) { s = QString::fromLatin1(data); r.enc = QStringLiteral("ISO-8859-1"); }
    else r.enc = bom ? QStringLiteral("UTF-8 com BOM") : QStringLiteral("UTF-8");
    r.crlf = s.contains(QLatin1String("\r\n"));
    if (r.crlf) s.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    r.text = s;
    r.ok = true;
    return r;
}

static bool saveTextFile(const QString &path, QString text, bool crlf, const QString &enc, QString &err) {
    if (crlf) text.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    QByteArray out;
    if (enc == QLatin1String("ISO-8859-1")) out = text.toLatin1();
    else { out = text.toUtf8(); if (enc.contains(QLatin1String("BOM"))) out.prepend("\xEF\xBB\xBF"); }
    if (QFileInfo(path).isSymLink()) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(out) != out.size()) { err = f.errorString(); return false; }
        return true;
    }
    QSaveFile f(path);
    f.setDirectWriteFallback(true);
    if (!f.open(QIODevice::WriteOnly)) { err = f.errorString(); return false; }
    f.write(out);
    if (!f.commit()) { err = f.errorString(); return false; }
    return true;
}

// acha "arquivo:linha" numa linha de saida (traceback do Python, gcc, node, pyflakes...)
static bool parseLocation(const QString &line, QString &file, int &ln, int &col) {
    static const QRegularExpression py(QStringLiteral("File \"(.+?)\", line (\\d+)"));
    static const QRegularExpression gen(QStringLiteral("([\\w./~+\\-]+\\.[A-Za-z0-9]+):(\\d+)(?::(\\d+))?"));
    auto m = py.match(line);
    if (m.hasMatch()) { file = m.captured(1); ln = m.captured(2).toInt(); col = 1; return true; }
    m = gen.match(line);
    if (m.hasMatch()) { file = m.captured(1); ln = m.captured(2).toInt(); col = m.captured(3).isEmpty() ? 1 : m.captured(3).toInt(); return true; }
    return false;
}

static int fuzzyScore(const QString &hay, const QString &needle) {
    const QString h = hay.toLower(), n = needle.toLower();
    int score = 0, hi = 0, last = -2;
    for (int i = 0; i < n.size(); ++i) {
        const int idx = h.indexOf(n[i], hi);
        if (idx < 0) return -1;
        score += 10;
        if (idx == last + 1) score += 15;
        if (idx == 0 || QStringLiteral("/_-.").contains(h[idx - 1])) score += 10;
        last = idx; hi = idx + 1;
    }
    if (h.mid(h.lastIndexOf(QLatin1Char('/')) + 1).contains(n)) score += 50;
    return score - h.size() / 4;
}

static QFont monoFont(int px) {
    QFont f;
    f.setFamilies({"JetBrains Mono", "Cascadia Code", "DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "monospace"});
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    return f;
}

// ============================================================================
//  Terminal de verdade: um pty + um emulador de terminal (xterm-256color)
//  Roda bash/vim/nano/top/python -i... como num terminal de qualquer outro editor.
// ============================================================================
struct TCell { char32_t ch = U' '; quint32 fg = 0, bg = 0; quint8 at = 0; };   // fg/bg: 0 = cor padrao, senao 0xFFRRGGBB
enum { TA_BOLD = 1, TA_UL = 2, TA_REV = 4, TA_DIM = 8, TA_ITAL = 16 };

class PtyTerm : public QWidget {
public:
    std::function<void(const QString &)> onOpenLine;   // duplo clique numa linha (arquivo:linha)

    PtyTerm(const QString &program, const QStringList &args, QWidget *parent = nullptr) : QWidget(parent), prog(program), argv(args) {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::IBeamCursor);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setMono(monoFont(13));
        resetAll(80, 24);
    }
    ~PtyTerm() override { killChild(true); }

    // ---------- API usada pela janela ----------
    void setMono(const QFont &f) {
        fnt = f;
        bold = f;
        bold.setBold(true);
        const QFontMetricsF fm(f);
        cw = std::max<qreal>(4.0, fm.horizontalAdvance(QLatin1Char('M')));
        chh = std::max<qreal>(8.0, std::ceil(fm.height()));
        asc = fm.ascent();
        fitToWidget();
        update();
    }
    void setWorkDir(const QString &d) { workDir = d; }
    bool running() const { return pid > 0; }
    void focusTerminal() { setFocus(); }
    void start() {
        if (pid > 0) return;
        closePty();
        launch();
    }
    void restart() {
        killChild(true);
        resetAll(cols, rows);
        launch();
    }
    void clear() {
        hist.clear();
        scrollOffset = 0;
        if (!useAlt) {
            for (auto &r : scr) r.assign(size_t(cols), TCell());
            if (pid > 0) { cx = cy = 0; writePty("\x0c"); }
        }
        clearSel();
        update();
    }
    void interrupt() { writePty("\x03"); }
    void sendLine(const QString &s) {
        if (pid <= 0) start();
        writePty(s.toUtf8() + "\r");
    }
    void writePty(const QByteArray &b) {
        if (master < 0 || b.isEmpty()) return;
        const char *p = b.constData();
        qsizetype left = b.size();
        int guard = 0;
        while (left > 0 && guard < 1000) {
            const ssize_t n = ::write(master, p, size_t(left));
            if (n > 0) { p += n; left -= n; }
            else if (n < 0 && (errno == EAGAIN || errno == EINTR)) { ++guard; QThread::usleep(500); }
            else break;
        }
    }

    // ---------- acesso para testes ----------
    void feed(const QByteArray &b) { feedBytes(b); update(); }
    const TCell &cellAt(int r, int c) const { return grid()[size_t(r)][size_t(c)]; }
    int curX() const { return cx; }
    int curY() const { return cy; }
    int nCols() const { return cols; }
    int nRows() const { return rows; }
    bool altScreen() const { return useAlt; }
    int histSize() const { return int(hist.size()); }
    QString lineText(int r) const {
        QString s;
        for (const TCell &c : grid()[size_t(r)]) s += QString::fromUcs4(&c.ch, 1);
        while (s.endsWith(QLatin1Char(' '))) s.chop(1);
        return s;
    }
    QString allText() const {
        QString s;
        for (int r = 0; r < rows; ++r) s += lineText(r) + QLatin1Char('\n');
        return s;
    }
    static QColor paletteColor(int i) {
        static const unsigned ansi[16] = {0x4a3a3e, 0xe5334a, 0x7ccf6a, 0xf0e060, 0x6aa0f0, 0xd68fe0, 0x6ec6d9, 0xd8cccc,
                                          0x7a6a6e, 0xff5a6a, 0x9be08a, 0xfff080, 0x8cc0ff, 0xf0aaf8, 0x90e0f0, 0xffffff};
        if (i < 16) return hx(ansi[std::max(0, i)]);
        if (i < 232) {
            static const int lv[6] = {0, 95, 135, 175, 215, 255};
            const int n = i - 16;
            return QColor(lv[n / 36], lv[(n / 6) % 6], lv[n % 6]);
        }
        const int g = 8 + 10 * (std::min(255, i) - 232);
        return QColor(g, g, g);
    }

protected:
    bool focusNextPrevChild(bool) override { return false; }   // Tab e do shell
    bool event(QEvent *e) override {
        if (e->type() == QEvent::ShortcutOverride) {
            auto *k = static_cast<QKeyEvent *>(e);
            const Qt::KeyboardModifiers m = k->modifiers();
            const bool ctrl = m & Qt::ControlModifier, shift = m & Qt::ShiftModifier;
            const int key = k->key();
            bool pass = false;
            if (ctrl && key == Qt::Key_QuoteLeft) pass = true;                                   // abrir/fechar o terminal
            else if (ctrl && !shift && (key == Qt::Key_J || key == Qt::Key_B || key == Qt::Key_Q)) pass = true;
            else if (ctrl && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) pass = true;     // trocar de aba
            else if (ctrl && shift && key != Qt::Key_C && key != Qt::Key_V) pass = true;           // Ctrl+Shift+E/F/G/D/M...
            else if (key >= Qt::Key_F5 && key <= Qt::Key_F12 && !useAlt) pass = true;               // F5 executar, F6 depurar...
            if (!pass) { e->accept(); return true; }
        }
        return QWidget::event(e);
    }

    void resizeEvent(QResizeEvent *e) override { QWidget::resizeEvent(e); fitToWidget(); }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Pal::BG_PANEL);
        p.setFont(fnt);
        const int base = useAlt ? 0 : int(hist.size()) - scrollOffset;
        const QColor defFg = Pal::TXT, defBg = Pal::BG_PANEL;
        for (int r = 0; r < rows; ++r) {
            const std::vector<TCell> *line = lineAbs(base + r);
            if (!line) continue;
            const qreal y = r * chh;
            for (int c = 0; c < cols && size_t(c) < line->size(); ++c) {
                const TCell &cell = (*line)[size_t(c)];
                QColor fg = cell.fg ? QColor::fromRgba(cell.fg) : defFg, bg = cell.bg ? QColor::fromRgba(cell.bg) : defBg;
                if (cell.at & TA_REV) std::swap(fg, bg);
                const bool sel = isSelected(base + r, c);
                const bool isCur = curVisible && scrollOffset == 0 && r == cy && c == cx;
                if (sel) { bg = Pal::BG_SEL; fg = Pal::TXT; }
                if (isCur && hasFocus()) { bg = Pal::ACCENT; fg = Qt::white; }
                if (bg != defBg) p.fillRect(QRectF(c * cw, y, std::ceil(cw) + 0.5, chh), bg);
                if (isCur && !hasFocus()) { p.setPen(Pal::ACCENT); p.setBrush(Qt::NoBrush); p.drawRect(QRectF(c * cw + 0.5, y + 0.5, cw - 1, chh - 1)); }
                if (cell.ch != U' ' && cell.ch != 0) {
                    if (cell.at & TA_DIM) fg.setAlpha(150);
                    p.setPen(fg);
                    p.setFont(cell.at & TA_BOLD ? bold : fnt);
                    p.drawText(QPointF(c * cw, y + asc), QString::fromUcs4(&cell.ch, 1));
                }
                if (cell.at & TA_UL) { p.setPen(fg); p.drawLine(QPointF(c * cw, y + chh - 2), QPointF((c + 1) * cw, y + chh - 2)); }
            }
        }
        if (scrollOffset > 0) {   // aviso de que esta olhando o historico
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            const QString t = QStringLiteral(" ↑ %1 linhas acima ").arg(scrollOffset);
            const QFontMetrics fm(fnt);
            const QRect br(width() - fm.horizontalAdvance(t) - 14, 4, fm.horizontalAdvance(t) + 8, fm.height() + 2);
            p.drawRoundedRect(br, 4, 4);
            p.setPen(Pal::TXT_DIM);
            p.drawText(br, Qt::AlignCenter, t);
        }
    }

    void keyPressEvent(QKeyEvent *e) override {
        const Qt::KeyboardModifiers m = e->modifiers();
        const bool ctrl = m & Qt::ControlModifier, alt = m & Qt::AltModifier, shift = m & Qt::ShiftModifier;
        const int k = e->key();
        if (k == Qt::Key_Shift || k == Qt::Key_Control || k == Qt::Key_Alt || k == Qt::Key_Meta || k == Qt::Key_AltGr || k == Qt::Key_CapsLock) return;
        if (pid <= 0) { if (k == Qt::Key_Return || k == Qt::Key_Enter) restart(); return; }
        if (shift && !ctrl && (k == Qt::Key_PageUp || k == Qt::Key_PageDown) && !useAlt) { scrollBy(k == Qt::Key_PageUp ? rows - 1 : -(rows - 1)); return; }
        if ((ctrl && shift && k == Qt::Key_C) || (ctrl && !shift && k == Qt::Key_Insert)) { copySel(); return; }
        if ((ctrl && shift && k == Qt::Key_V) || (shift && !ctrl && k == Qt::Key_Insert)) { pasteClip(QClipboard::Clipboard); return; }
        if (ctrl && !shift && !alt && k == Qt::Key_C && hasSel()) { copySel(); clearSel(); update(); return; }
        if (ctrl && !shift && !alt && k == Qt::Key_V) { pasteClip(QClipboard::Clipboard); return; }
        scrollOffset = 0;
        if (hasSel()) { clearSel(); }
        const int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
        QByteArray out;
        auto csiKey = [&](char fin) {
            if (mod > 1) out = QByteArray("\x1b[1;") + QByteArray::number(mod) + fin;
            else out = (appCursor ? QByteArray("\x1bO") : QByteArray("\x1b[")) + fin;
        };
        auto tilde = [&](int n) {
            out = QByteArray("\x1b[") + QByteArray::number(n) + (mod > 1 ? QByteArray(";") + QByteArray::number(mod) : QByteArray()) + "~";
        };
        switch (k) {
        case Qt::Key_Return: case Qt::Key_Enter: out = "\r"; break;
        case Qt::Key_Backspace: out = ctrl ? "\x17" : "\x7f"; break;
        case Qt::Key_Tab: out = "\t"; break;
        case Qt::Key_Backtab: out = "\x1b[Z"; break;
        case Qt::Key_Escape: out = "\x1b"; break;
        case Qt::Key_Up: csiKey('A'); break;
        case Qt::Key_Down: csiKey('B'); break;
        case Qt::Key_Right: csiKey('C'); break;
        case Qt::Key_Left: csiKey('D'); break;
        case Qt::Key_Home: csiKey('H'); break;
        case Qt::Key_End: csiKey('F'); break;
        case Qt::Key_PageUp: tilde(5); break;
        case Qt::Key_PageDown: tilde(6); break;
        case Qt::Key_Insert: tilde(2); break;
        case Qt::Key_Delete: tilde(3); break;
        case Qt::Key_F1: case Qt::Key_F2: case Qt::Key_F3: case Qt::Key_F4: {
            const char f = "PQRS"[k - Qt::Key_F1];
            out = mod > 1 ? QByteArray("\x1b[1;") + QByteArray::number(mod) + f : QByteArray("\x1bO") + f;
            break;
        }
        case Qt::Key_F5: tilde(15); break;
        case Qt::Key_F6: tilde(17); break;
        case Qt::Key_F7: tilde(18); break;
        case Qt::Key_F8: tilde(19); break;
        case Qt::Key_F9: tilde(20); break;
        case Qt::Key_F10: tilde(21); break;
        case Qt::Key_F11: tilde(23); break;
        case Qt::Key_F12: tilde(24); break;
        default: break;
        }
        if (out.isEmpty() && ctrl && !alt) {
            if (k >= Qt::Key_A && k <= Qt::Key_Z) out = QByteArray(1, char(k - Qt::Key_A + 1));
            else if (k == Qt::Key_Space || k == Qt::Key_At) out = QByteArray(1, '\0');
            else if (k == Qt::Key_BracketLeft) out = "\x1b";
            else if (k == Qt::Key_Backslash) out = "\x1c";
            else if (k == Qt::Key_BracketRight) out = "\x1d";
            else if (k == Qt::Key_Slash || k == Qt::Key_Underscore) out = "\x1f";
        }
        if (out.isEmpty()) {
            const QString t = e->text();
            if (!t.isEmpty() && (t[0].unicode() >= 0x20 || t[0] == QLatin1Char('\t'))) {
                out = t.toUtf8();
                if (alt && !ctrl) out.prepend('\x1b');
            }
        }
        if (!out.isEmpty()) writePty(out);
        update();
    }

    void mousePressEvent(QMouseEvent *e) override {
        setFocus();
        if (e->button() == Qt::LeftButton) {
            const auto pt = cellPos(e->position());
            selA = selB = pt;
            selecting = true;
            selActive = false;
            update();
        } else if (e->button() == Qt::MiddleButton) {
            pasteClip(QClipboard::Selection);
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (!selecting) return;
        selB = cellPos(e->position());
        selActive = selA != selB;
        if (e->position().y() < 0) scrollBy(1);
        else if (e->position().y() > height()) scrollBy(-1);
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        selecting = false;
        if (selActive) QGuiApplication::clipboard()->setText(selText(), QClipboard::Selection);
        else clearSel();
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        const auto pt = cellPos(e->position());
        const std::vector<TCell> *line = lineAbs(pt.first);
        if (!line) return;
        QString txt;
        for (const TCell &c : *line) txt += QString::fromUcs4(&c.ch, 1);
        int a = pt.second, b = pt.second;
        auto isW = [&](int i) { return i >= 0 && i < txt.size() && !txt[i].isSpace(); };
        while (isW(a - 1)) --a;
        while (isW(b + 1)) ++b;
        selA = {pt.first, a};
        selB = {pt.first, b + 1};
        selActive = true;
        selecting = false;
        update();
        if (onOpenLine) onOpenLine(txt.trimmed());
    }
    void wheelEvent(QWheelEvent *e) override {
        const int d = e->angleDelta().y() / 40;   // 3 por "clique"
        if (d == 0) return;
        if (useAlt) writePty((d > 0 ? QByteArray(appCursor ? "\x1bOA" : "\x1b[A") : QByteArray(appCursor ? "\x1bOB" : "\x1b[B")).repeated(std::abs(d)));
        else scrollBy(d);
        e->accept();
    }
    void contextMenuEvent(QContextMenuEvent *e) override {
        QMenu m(this);
        QAction *cp = m.addAction(QStringLiteral("Copiar"));
        cp->setEnabled(hasSel());
        QAction *ps = m.addAction(QStringLiteral("Colar"));
        m.addSeparator();
        QAction *cl = m.addAction(QStringLiteral("Limpar"));
        QAction *r = m.exec(e->globalPos());
        if (r == cp) copySel();
        else if (r == ps) pasteClip(QClipboard::Clipboard);
        else if (r == cl) clear();
    }
    void focusInEvent(QFocusEvent *e) override { QWidget::focusInEvent(e); update(); }
    void focusOutEvent(QFocusEvent *e) override { QWidget::focusOutEvent(e); update(); }

private:
    enum State { Ground, Esc, Csi, Osc, OscEsc, Skip1 };
    QString prog;
    QStringList argv;
    QString workDir;
    QFont fnt, bold;
    qreal cw = 8, chh = 16, asc = 12;
    int cols = 80, rows = 24;
    std::vector<std::vector<TCell>> scr, alt;
    std::deque<std::vector<TCell>> hist;
    size_t histMax = 5000;
    bool useAlt = false;
    int cx = 0, cy = 0, scx = 0, scy = 0, top = 0, bot = 23, scrollOffset = 0;
    bool wrapNext = false, autoWrap = true, appCursor = false, curVisible = true, bracketed = false;
    quint32 curFg = 0, curBg = 0;
    quint8 curAt = 0;
    State state = Ground;
    QByteArray csiBuf;
    QStringDecoder dec{QStringConverter::Utf8};
    int master = -1;
    pid_t pid = -1;
    QSocketNotifier *sn = nullptr;
    bool selecting = false, selActive = false;
    std::pair<int, int> selA{0, 0}, selB{0, 0};

    // ---------- tela ----------
    std::vector<std::vector<TCell>> &grid() { return useAlt ? alt : scr; }
    const std::vector<std::vector<TCell>> &grid() const { return useAlt ? alt : scr; }
    std::vector<TCell> blankRow() const { TCell c; c.bg = curBg; return std::vector<TCell>(size_t(cols), c); }
    const std::vector<TCell> *lineAbs(int a) const {
        if (a < 0) return nullptr;
        if (useAlt) return a < rows ? &alt[size_t(a)] : nullptr;
        if (size_t(a) < hist.size()) return &hist[size_t(a)];
        const size_t i = size_t(a) - hist.size();
        return i < scr.size() ? &scr[i] : nullptr;
    }
    void resetAll(int c, int r) {
        cols = std::max(2, c); rows = std::max(1, r);
        scr.assign(size_t(rows), std::vector<TCell>(size_t(cols)));
        alt = scr;
        hist.clear();
        cx = cy = scx = scy = 0;
        top = 0; bot = rows - 1;
        useAlt = false; wrapNext = false; autoWrap = true; appCursor = false; curVisible = true; bracketed = false;
        curFg = curBg = 0; curAt = 0;
        state = Ground; csiBuf.clear();
        scrollOffset = 0;
        clearSel();
    }
    void fitToWidget() {
        const int nc = std::max(2, int(width() / cw)), nr = std::max(1, int(height() / chh));
        if (nc != cols || nr != rows) resizeGrid(nc, nr);
    }
    void resizeGrid(int nc, int nr) {
        for (int pass = 0; pass < 2; ++pass) {
            auto &g = pass == 0 ? scr : alt;
            while (int(g.size()) > nr) {
                if (pass == 0 && cy > 0) { hist.push_back(g.front()); if (hist.size() > histMax) hist.pop_front(); g.erase(g.begin()); --cy; }
                else g.pop_back();
            }
            while (int(g.size()) < nr) g.push_back(std::vector<TCell>(size_t(nc)));
            for (auto &row : g) row.resize(size_t(nc));
        }
        cols = nc; rows = nr;
        top = 0; bot = rows - 1;
        cx = std::min(cx, cols - 1); cy = std::min(cy, rows - 1);
        wrapNext = false;
        if (master >= 0) {
            struct winsize ws {};
            ws.ws_col = ushort(cols); ws.ws_row = ushort(rows);
            ws.ws_xpixel = ushort(width()); ws.ws_ypixel = ushort(height());
            ::ioctl(master, TIOCSWINSZ, &ws);
        }
        clearSel();
        update();
    }
    void scrollUp(int n) {
        auto &g = grid();
        for (int i = 0; i < n; ++i) {
            if (!useAlt && top == 0) { hist.push_back(g[0]); if (hist.size() > histMax) hist.pop_front(); else if (scrollOffset > 0) ++scrollOffset; }
            g.erase(g.begin() + top);
            g.insert(g.begin() + bot, blankRow());
        }
        scrollOffset = std::min(scrollOffset, int(hist.size()));
    }
    void scrollDown(int n) {
        auto &g = grid();
        for (int i = 0; i < n; ++i) { g.erase(g.begin() + bot); g.insert(g.begin() + top, blankRow()); }
    }
    void lineFeed() { if (cy == bot) scrollUp(1); else if (cy < rows - 1) ++cy; }
    void put(char32_t c) {
        if (wrapNext && autoWrap) { cx = 0; lineFeed(); }
        wrapNext = false;
        TCell &cell = grid()[size_t(cy)][size_t(cx)];
        cell.ch = c; cell.fg = curFg; cell.bg = curBg; cell.at = curAt;
        if (cx == cols - 1) wrapNext = true; else ++cx;
    }
    void eraseCells(int r, int a, int b) {   // [a,b)
        auto &row = grid()[size_t(r)];
        for (int i = std::max(0, a); i < std::min(b, cols); ++i) { TCell c; c.bg = curBg; row[size_t(i)] = c; }
    }

    // ---------- analisador de sequencias ----------
    void feedBytes(const QByteArray &b) {
        const QString s = dec.decode(b);
        for (char32_t cp : s.toUcs4()) step(cp);
    }
    void step(char32_t c) {
        switch (state) {
        case Ground:
            if (c == 0x1b) state = Esc;
            else if (c < 0x20) control(c);
            else if (c != 0x7f) put(c);
            break;
        case Esc:
            state = Ground;
            switch (c) {
            case '[': state = Csi; csiBuf.clear(); break;
            case ']': case 'P': case '_': case '^': case 'X': state = Osc; break;
            case '(': case ')': case '*': case '+': case '#': case '%': state = Skip1; break;
            case '7': scx = cx; scy = cy; break;
            case '8': cx = std::min(scx, cols - 1); cy = std::min(scy, rows - 1); wrapNext = false; break;
            case 'D': lineFeed(); break;
            case 'E': cx = 0; lineFeed(); wrapNext = false; break;
            case 'M': if (cy == top) scrollDown(1); else if (cy > 0) --cy; break;
            case 'c': resetAll(cols, rows); break;
            case 0x1b: state = Esc; break;
            default: break;
            }
            break;
        case Csi:
            if (c >= 0x30 && c <= 0x3f) { if (csiBuf.size() < 256) csiBuf.append(char(c)); }
            else if (c >= 0x20 && c <= 0x2f) { if (csiBuf.size() < 256) csiBuf.append(char(c)); }
            else if (c >= 0x40 && c <= 0x7e) { state = Ground; csi(char(c)); }
            else if (c == 0x1b) state = Esc;
            else if (c < 0x20) control(c);
            else state = Ground;
            break;
        case Osc:
            if (c == 0x07) state = Ground;
            else if (c == 0x1b) state = OscEsc;
            break;
        case OscEsc: state = Ground; break;
        case Skip1: state = Ground; break;
        }
    }
    void control(char32_t c) {
        switch (c) {
        case '\n': case 0x0b: case 0x0c: lineFeed(); break;
        case '\r': cx = 0; wrapNext = false; break;
        case 0x08: if (cx > 0) --cx; wrapNext = false; break;
        case '\t': { int n = (cx / 8 + 1) * 8; cx = std::min(n, cols - 1); wrapNext = false; break; }
        default: break;
        }
    }
    QList<int> params(int &priv) const {
        QList<int> v;
        QByteArray b = csiBuf;
        priv = 0;
        if (!b.isEmpty() && (b[0] == '?' || b[0] == '>' || b[0] == '=' || b[0] == '<')) { priv = b[0]; b.remove(0, 1); }
        while (!b.isEmpty() && !(b.back() >= '0' && b.back() <= '9') && b.back() != ';' && b.back() != ':') b.chop(1);   // intermediarios
        if (b.isEmpty()) return v;
        for (const QByteArray &part : b.split(';')) {
            bool ok = false;
            const int n = part.split(':').first().toInt(&ok);
            v << (ok ? n : -1);
        }
        return v;
    }
    void csi(char fin) {
        int priv = 0;
        const QList<int> P = params(priv);
        auto arg = [&](int i, int def) { return i < P.size() && P[i] > 0 ? P[i] : def; };
        auto a0 = [&](int i, int def) { return i < P.size() && P[i] >= 0 ? P[i] : def; };
        const int n = arg(0, 1);
        switch (fin) {
        case 'A': cy = std::max(cy >= top ? top : 0, cy - n); wrapNext = false; break;
        case 'B': cy = std::min(cy <= bot ? bot : rows - 1, cy + n); wrapNext = false; break;
        case 'C': case 'a': cx = std::min(cols - 1, cx + n); wrapNext = false; break;
        case 'D': cx = std::max(0, cx - n); wrapNext = false; break;
        case 'E': cx = 0; cy = std::min(rows - 1, cy + n); wrapNext = false; break;
        case 'F': cx = 0; cy = std::max(0, cy - n); wrapNext = false; break;
        case 'G': case '`': cx = std::max(0, std::min(cols - 1, arg(0, 1) - 1)); wrapNext = false; break;
        case 'd': cy = std::max(0, std::min(rows - 1, arg(0, 1) - 1)); wrapNext = false; break;
        case 'H': case 'f': cy = std::max(0, std::min(rows - 1, arg(0, 1) - 1)); cx = std::max(0, std::min(cols - 1, arg(1, 1) - 1)); wrapNext = false; break;
        case 'J': {
            const int m = a0(0, 0);
            if (m == 0) { eraseCells(cy, cx, cols); for (int r = cy + 1; r < rows; ++r) eraseCells(r, 0, cols); }
            else if (m == 1) { for (int r = 0; r < cy; ++r) eraseCells(r, 0, cols); eraseCells(cy, 0, cx + 1); }
            else { for (int r = 0; r < rows; ++r) eraseCells(r, 0, cols); if (m == 3) { hist.clear(); scrollOffset = 0; } }
            break;
        }
        case 'K': {
            const int m = a0(0, 0);
            if (m == 0) eraseCells(cy, cx, cols); else if (m == 1) eraseCells(cy, 0, cx + 1); else eraseCells(cy, 0, cols);
            break;
        }
        case 'L': if (cy >= top && cy <= bot) { auto &g = grid(); for (int i = 0; i < std::min(n, bot - cy + 1); ++i) { g.erase(g.begin() + bot); g.insert(g.begin() + cy, blankRow()); } } cx = 0; wrapNext = false; break;
        case 'M': if (cy >= top && cy <= bot) { auto &g = grid(); for (int i = 0; i < std::min(n, bot - cy + 1); ++i) { g.erase(g.begin() + cy); g.insert(g.begin() + bot, blankRow()); } } cx = 0; wrapNext = false; break;
        case 'P': { auto &row = grid()[size_t(cy)]; const int k = std::min(n, cols - cx); row.erase(row.begin() + cx, row.begin() + cx + k); TCell e; e.bg = curBg; row.insert(row.end(), size_t(k), e); break; }
        case '@': { auto &row = grid()[size_t(cy)]; const int k = std::min(n, cols - cx); TCell e; e.bg = curBg; row.insert(row.begin() + cx, size_t(k), e); row.resize(size_t(cols)); break; }
        case 'X': eraseCells(cy, cx, cx + n); break;
        case 'S': scrollUp(n); break;
        case 'T': scrollDown(n); break;
        case 'm': sgr(P); break;
        case 'r': {
            const int t = arg(0, 1) - 1, b = arg(1, rows) - 1;
            if (t < b && b < rows) { top = t; bot = b; } else { top = 0; bot = rows - 1; }
            cx = cy = 0; wrapNext = false;
            break;
        }
        case 's': if (!priv) { scx = cx; scy = cy; } break;
        case 'u': if (!priv) { cx = std::min(scx, cols - 1); cy = std::min(scy, rows - 1); wrapNext = false; } break;
        case 'h': case 'l': if (priv == '?') for (int mode : P) setMode(mode, fin == 'h'); break;
        case 'n': {
            if (a0(0, 0) == 6) writePty(QByteArray("\x1b[") + QByteArray::number(cy + 1) + ";" + QByteArray::number(cx + 1) + "R");
            else if (a0(0, 0) == 5) writePty("\x1b[0n");
            break;
        }
        case 'c': if (priv == 0) writePty("\x1b[?1;2c"); else if (priv == '>') writePty("\x1b[>0;0;0c"); break;
        default: break;
        }
    }
    void setMode(int mode, bool on) {
        switch (mode) {
        case 1: appCursor = on; break;
        case 7: autoWrap = on; break;
        case 25: curVisible = on; break;
        case 2004: bracketed = on; break;
        case 47: case 1047: case 1049:
            if (on && !useAlt) {
                if (mode == 1049) { scx = cx; scy = cy; }
                useAlt = true;
                for (auto &r : alt) r.assign(size_t(cols), TCell());
                scrollOffset = 0;
                clearSel();
            } else if (!on && useAlt) {
                useAlt = false;
                if (mode == 1049) { cx = std::min(scx, cols - 1); cy = std::min(scy, rows - 1); }
                top = 0; bot = rows - 1; wrapNext = false;
                clearSel();
            }
            break;
        default: break;
        }
    }
    void sgr(const QList<int> &P) {
        if (P.isEmpty()) { curFg = curBg = 0; curAt = 0; return; }
        auto rgb = [](int r, int g, int b) { return 0xFF000000u | (quint32(r & 255) << 16) | (quint32(g & 255) << 8) | quint32(b & 255); };
        for (int i = 0; i < P.size(); ++i) {
            const int v = P[i] < 0 ? 0 : P[i];
            if (v == 0) { curFg = curBg = 0; curAt = 0; }
            else if (v == 1) curAt |= TA_BOLD;
            else if (v == 2) curAt |= TA_DIM;
            else if (v == 3) curAt |= TA_ITAL;
            else if (v == 4) curAt |= TA_UL;
            else if (v == 7) curAt |= TA_REV;
            else if (v == 22) curAt &= quint8(~(TA_BOLD | TA_DIM));
            else if (v == 23) curAt &= quint8(~TA_ITAL);
            else if (v == 24) curAt &= quint8(~TA_UL);
            else if (v == 27) curAt &= quint8(~TA_REV);
            else if (v >= 30 && v <= 37) curFg = paletteColor(v - 30).rgba();
            else if (v >= 90 && v <= 97) curFg = paletteColor(v - 90 + 8).rgba();
            else if (v >= 40 && v <= 47) curBg = paletteColor(v - 40).rgba();
            else if (v >= 100 && v <= 107) curBg = paletteColor(v - 100 + 8).rgba();
            else if (v == 39) curFg = 0;
            else if (v == 49) curBg = 0;
            else if (v == 38 || v == 48) {
                quint32 col = 0;
                if (i + 1 < P.size() && P[i + 1] == 5 && i + 2 < P.size()) { col = paletteColor(std::max(0, std::min(255, P[i + 2]))).rgba(); i += 2; }
                else if (i + 1 < P.size() && P[i + 1] == 2 && i + 4 < P.size()) { col = rgb(P[i + 2], P[i + 3], P[i + 4]); i += 4; }
                else break;
                if (v == 38) curFg = col; else curBg = col;
            }
        }
    }

    // ---------- selecao / copiar / colar ----------
    std::pair<int, int> cellPos(const QPointF &pt) const {
        const int base = useAlt ? 0 : int(hist.size()) - scrollOffset;
        const int r = std::max(0, std::min(rows - 1, int(pt.y() / chh))), c = std::max(0, std::min(cols - 1, int(pt.x() / cw)));
        return {base + r, c};
    }
    bool hasSel() const { return selActive && selA != selB; }
    void clearSel() { selActive = false; selecting = false; }
    bool isSelected(int absLine, int c) const {
        if (!hasSel()) return false;
        const auto a = std::min(selA, selB), b = std::max(selA, selB);
        const std::pair<int, int> p{absLine, c};
        return p >= a && p < b;
    }
    QString selText() const {
        if (!hasSel()) return QString();
        const auto a = std::min(selA, selB), b = std::max(selA, selB);
        QStringList lines;
        for (int l = a.first; l <= b.first; ++l) {
            const std::vector<TCell> *row = lineAbs(l);
            if (!row) continue;
            QString s;
            const int from = l == a.first ? a.second : 0, to = l == b.first ? b.second : int(row->size());
            for (int c = from; c < to && size_t(c) < row->size(); ++c) s += QString::fromUcs4(&(*row)[size_t(c)].ch, 1);
            if (l != b.first || to >= cols) while (s.endsWith(QLatin1Char(' '))) s.chop(1);
            lines << s;
        }
        return lines.join(QLatin1Char('\n'));
    }
    void copySel() { if (hasSel()) QGuiApplication::clipboard()->setText(selText()); }
    void pasteClip(QClipboard::Mode mode) {
        QString t = QGuiApplication::clipboard()->text(mode);
        if (t.isEmpty() || pid <= 0) return;
        t.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        QByteArray b = t.toUtf8();
        if (bracketed) b = "\x1b[200~" + b + "\x1b[201~";
        else b.replace('\n', '\r');
        scrollOffset = 0;
        writePty(b);
    }
    void scrollBy(int d) {   // d > 0: para cima (historico)
        scrollOffset = std::max(0, std::min(int(hist.size()), scrollOffset + d));
        update();
    }

    // ---------- processo ----------
    void launch() {
        const int m = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (m < 0) { feedBytes("[não consegui abrir um pty]\r\n"); update(); return; }
        char slave[128] = {0};
        if (::grantpt(m) != 0 || ::unlockpt(m) != 0 || ::ptsname_r(m, slave, sizeof(slave)) != 0) {
            ::close(m);
            feedBytes("[não consegui preparar o pty]\r\n");
            update();
            return;
        }
        std::vector<std::string> envs;
        QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
        pe.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
        pe.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
        pe.remove(QStringLiteral("LINES"));
        pe.remove(QStringLiteral("COLUMNS"));
        for (const QString &k : pe.keys()) envs.push_back((k + QLatin1Char('=') + pe.value(k)).toLocal8Bit().toStdString());
        std::vector<std::string> av;
        av.push_back(prog.toLocal8Bit().toStdString());
        for (const QString &a : argv) av.push_back(a.toLocal8Bit().toStdString());
        std::vector<char *> avp, evp;
        for (auto &s : av) avp.push_back(&s[0]);
        avp.push_back(nullptr);
        for (auto &s : envs) evp.push_back(&s[0]);
        evp.push_back(nullptr);
        const std::string wd = workDir.toLocal8Bit().toStdString();
        struct winsize ws {};
        ws.ws_col = ushort(cols); ws.ws_row = ushort(rows);
        ws.ws_xpixel = ushort(width()); ws.ws_ypixel = ushort(height());
        const pid_t child = ::fork();
        if (child < 0) { ::close(m); feedBytes("[fork falhou]\r\n"); update(); return; }
        if (child == 0) {
            ::setsid();
            const int sfd = ::open(slave, O_RDWR);
            if (sfd < 0) ::_exit(126);
            ::ioctl(sfd, TIOCSCTTY, 0);
            ::ioctl(sfd, TIOCSWINSZ, &ws);
            ::dup2(sfd, 0); ::dup2(sfd, 1); ::dup2(sfd, 2);
            if (sfd > 2) ::close(sfd);
            ::close(m);
            if (!wd.empty() && ::chdir(wd.c_str()) != 0) { /* fica na pasta atual */ }
            for (int sgn : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGHUP, SIGTERM}) ::signal(sgn, SIG_DFL);
            ::execvpe(avp[0], avp.data(), evp.data());
            ::_exit(127);
        }
        master = m;
        pid = child;
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
        ::fcntl(master, F_SETFD, FD_CLOEXEC);
        sn = new QSocketNotifier(master, QSocketNotifier::Read, this);
        connect(sn, &QSocketNotifier::activated, this, [this] { readable(); });
    }
    void readable() {
        char buf[65536];
        for (int rounds = 0; rounds < 16; ++rounds) {
            const ssize_t n = ::read(master, buf, sizeof(buf));
            if (n > 0) { feedBytes(QByteArray(buf, int(n))); if (n < ssize_t(sizeof(buf))) break; }
            else if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            else { childGone(); return; }
        }
        update();
    }
    void childGone() {
        const pid_t p = pid;
        int st = 0;
        int code = -1;
        if (p > 0) {
            pid_t r = ::waitpid(p, &st, WNOHANG);
            for (int i = 0; r == 0 && i < 20; ++i) { QThread::msleep(10); r = ::waitpid(p, &st, WNOHANG); }
            if (r == p) code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
            else ::kill(p, SIGKILL), ::waitpid(p, &st, 0);
        }
        closePty();
        pid = -1;
        feedBytes(QStringLiteral("\r\n\x1b[90m[processo encerrado, código %1; aperte Enter para reiniciar]\x1b[0m\r\n").arg(code).toUtf8());
        update();
    }
    void closePty() {
        if (sn) { sn->setEnabled(false); sn->deleteLater(); sn = nullptr; }
        if (master >= 0) { ::close(master); master = -1; }
    }
    void killChild(bool hard) {
        closePty();
        if (pid > 0) {
            ::kill(-pid, hard ? SIGKILL : SIGHUP);
            ::kill(pid, hard ? SIGKILL : SIGHUP);
            int st = 0;
            for (int i = 0; i < 30; ++i) { if (::waitpid(pid, &st, WNOHANG) != 0) break; QThread::msleep(5); if (i == 20) { ::kill(-pid, SIGKILL); ::kill(pid, SIGKILL); } }
            pid = -1;
        }
    }
};

// ============================================================================
//  Adaptador de depuracao (Python): gravado em ~/.cache e executado junto do programa depurado
// ============================================================================
static const char *kDbgAdapter = R"PYDBG("""Adaptador de depuracao do Cisma Code (Python). Fala com o editor por um socket local (uma linha de JSON por mensagem)
e usa o bdb, o mesmo nucleo do pdb: pontos de parada, passo sobre/dentro/fora, pilha de chamadas, variaveis e avaliar."""
import bdb
import builtins
import contextlib
import io
import json
import os
import queue
import reprlib
import socket
import sys
import threading
import traceback
import types

_sock = None
_rf = None
_lock = threading.Lock()
_cmds = queue.Queue()
_dbg = None


def _send(obj):
    data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
    with _lock:
        try:
            _sock.sendall(data)
        except OSError:
            os._exit(1)


def _reader():
    try:
        for line in _rf:
            try:
                m = json.loads(line)
            except ValueError:
                continue
            if m.get("cmd") == "setbp":          # ponto de parada posto/tirado com o programa rodando
                if _dbg is not None:
                    _dbg.apply_bps(m.get("bps", {}))
            else:
                _cmds.put(m)
    except Exception:
        pass
    _cmds.put({"cmd": "stop"})


_R = reprlib.Repr()
_R.maxstring = _R.maxother = 160
_R.maxlist = _R.maxtuple = _R.maxdict = _R.maxset = _R.maxarray = _R.maxdeque = _R.maxfrozenset = 12
_R.maxlevel = 3


def _rep(v):
    try:
        return _R.repr(v)
    except Exception as e:
        return "<repr falhou: %s>" % e


def _vars(d, skip_callables):
    out = []
    try:
        items = list(d.items())
    except Exception:
        return out
    for k, v in items[:400]:
        if isinstance(k, str) and k.startswith("__") and k.endswith("__"):
            continue
        if isinstance(v, types.ModuleType):
            continue
        if skip_callables and isinstance(v, (types.FunctionType, types.BuiltinFunctionType, type)):
            continue
        out.append([str(k), type(v).__name__, _rep(v)])
    return out


class _Dbg(bdb.Bdb):
    def __init__(self, entry):
        bdb.Bdb.__init__(self)
        self.entry = entry
        self.first = True
        self.stack = []
        self._canon = {}

    def canonic(self, filename):
        if filename.startswith("<") and filename.endswith(">"):
            return filename
        c = self._canon.get(filename)
        if c is None:
            c = os.path.realpath(filename)
            self._canon[filename] = c
        return c

    def apply_bps(self, bps):
        try:
            self.clear_all_breaks()
            for f, lines in bps.items():
                for ln in lines:
                    self.set_break(self.canonic(f), int(ln))
        except Exception:
            pass

    def _is_bp(self, frame):
        try:
            return frame.f_lineno in self.get_file_breaks(self.canonic(frame.f_code.co_filename))
        except Exception:
            return False

    def user_line(self, frame):
        bp = self._is_bp(frame)
        if self.first:
            self.first = False
            if not bp and not self.entry:
                self.set_continue()
                return
            self.pause(frame, "breakpoint" if bp else "entry")
            return
        self.pause(frame, "breakpoint" if bp else "step")

    def _frames(self, frame):
        st = []
        f = frame
        while f is not None and f is not self.botframe:
            st.append(f)
            f = f.f_back
        return st

    def _vars_of(self, i):
        if not (0 <= i < len(self.stack)):
            return {"locals": [], "globals": []}
        f = self.stack[i]
        glob = [] if f.f_locals is f.f_globals else _vars(f.f_globals, True)
        return {"locals": _vars(f.f_locals, False), "globals": glob}

    def _eval(self, expr, i):
        f = self.stack[max(0, min(i, len(self.stack) - 1))] if self.stack else None
        g = f.f_globals if f else {}
        l = f.f_locals if f else {}
        try:
            try:
                code = compile(expr, "<avaliar>", "eval")
            except SyntaxError:
                code = None
            if code is not None:
                r = repr(eval(code, g, l))
                return r if len(r) <= 4000 else r[:4000] + "…"
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                exec(compile(expr, "<avaliar>", "exec"), g, l)
            return buf.getvalue().rstrip("\n") or "(ok)"
        except BaseException as e:
            return "".join(traceback.format_exception_only(type(e), e)).rstrip()

    def pause(self, frame, reason, post=False):
        self.stack = self._frames(frame)
        msg = {"ev": "stopped", "reason": reason,
               "frames": [[self.canonic(f.f_code.co_filename), f.f_lineno, f.f_code.co_name] for f in self.stack]}
        msg.update(self._vars_of(0))
        _send(msg)
        while True:
            m = _cmds.get()
            c = m.get("cmd")
            i = int(m.get("frame", 0))
            if c == "vars":
                v = {"ev": "vars", "frame": i}
                v.update(self._vars_of(i))
                _send(v)
            elif c == "eval":
                _send({"ev": "evalres", "expr": m.get("expr", ""), "out": self._eval(m.get("expr", ""), i)})
            elif c == "stop":
                sys.stdout.flush()
                os._exit(1)
            elif post:
                return
            elif c == "continue":
                self.set_continue()
                return
            elif c == "next":
                self.set_next(self.stack[0]) if self.stack else self.set_step()
                return
            elif c == "step":
                self.set_step()
                return
            elif c == "out":
                self.set_return(self.stack[0]) if self.stack else self.set_step()
                return


def main():
    global _sock, _rf, _dbg
    sock_path, script = sys.argv[1], os.path.realpath(sys.argv[2])
    _sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    _sock.connect(sock_path)
    _rf = _sock.makefile("r", encoding="utf-8")
    threading.Thread(target=_reader, daemon=True).start()
    _send({"ev": "hello"})
    while True:
        m = _cmds.get()
        if m.get("cmd") == "go":
            break
        if m.get("cmd") == "stop":
            os._exit(0)
    import __main__
    ns = __main__.__dict__
    ns.clear()
    ns.update({"__name__": "__main__", "__file__": script, "__builtins__": builtins, "__doc__": None,
               "__package__": None, "__spec__": None})
    sys.argv = [script]
    sys.path[0] = os.path.dirname(script)
    with open(script, "rb") as fh:
        code = compile(fh.read(), script, "exec")
    d = _Dbg(bool(m.get("entry")))
    _dbg = d
    d.apply_bps(m.get("bps", {}))
    rc = 0
    try:
        d.run(code, ns)
    except SystemExit as e:
        c = e.code
        rc = c if isinstance(c, int) else (0 if c is None else 1)
        if not isinstance(c, (int, type(None))):
            print(c, file=sys.stderr)
    except bdb.BdbQuit:
        pass
    except BaseException as e:
        rc = 1
        tb = e.__traceback__
        t = tb
        while t is not None and t.tb_frame.f_code.co_filename != script:   # tira do traceback o miolo do depurador
            t = t.tb_next
        traceback.print_exception(type(e), e, t or tb)
        sys.stderr.flush()
        last = tb
        while last.tb_next is not None:
            last = last.tb_next
        d.pause(last.tb_frame, "exceção: %s: %s" % (type(e).__name__, str(e)[:200]), post=True)
    _send({"ev": "exit", "code": rc})
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(rc)
)PYDBG";

// ============================================================================
//  Barra de localizar / substituir
// ============================================================================
class FindBar : public QWidget {
public:
    std::function<CodeEditor *()> editor;
    QLineEdit *find, *repl;

    explicit FindBar(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("findBar");
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(10, 6, 10, 6);
        v->setSpacing(4);
        auto *r1 = new QHBoxLayout;
        r1->setSpacing(4);
        find = new QLineEdit; find->setPlaceholderText(QStringLiteral("Localizar"));
        auto tb = [this](const QString &t, const QString &tip, bool checkable) {
            auto *b = new QToolButton(this);
            b->setText(t); b->setToolTip(tip); b->setCheckable(checkable); b->setAutoRaise(true); b->setFocusPolicy(Qt::NoFocus);
            return b;
        };
        cs = tb("Aa", "Diferenciar maiúsculas de minúsculas", true);
        ww = tb("ab", "Palavra inteira", true);
        rx = tb(".*", "Expressão regular", true);
        count = new QLabel; count->setObjectName("findCount"); count->setMinimumWidth(110);
        auto *up = tb("▲", "Anterior (Shift+Enter)", false), *dn = tb("▼", "Próximo (Enter)", false), *cl = tb("✕", "Fechar (Esc)", false);
        r1->addWidget(find, 1);
        for (QWidget *w : {static_cast<QWidget *>(cs), static_cast<QWidget *>(ww), static_cast<QWidget *>(rx), static_cast<QWidget *>(count),
                           static_cast<QWidget *>(up), static_cast<QWidget *>(dn), static_cast<QWidget *>(cl)}) r1->addWidget(w);
        v->addLayout(r1);
        replRow = new QWidget;
        auto *r2 = new QHBoxLayout(replRow);
        r2->setContentsMargins(0, 0, 0, 0);
        r2->setSpacing(4);
        repl = new QLineEdit; repl->setPlaceholderText(QStringLiteral("Substituir por"));
        auto *one = new QPushButton(QStringLiteral("Substituir")), *all = new QPushButton(QStringLiteral("Substituir tudo"));
        r2->addWidget(repl, 1); r2->addWidget(one); r2->addWidget(all);
        v->addWidget(replRow);
        hide();

        refreshTimer.setSingleShot(true);
        refreshTimer.setInterval(120);
        connect(&refreshTimer, &QTimer::timeout, this, [this] { refresh(); });
        connect(find, &QLineEdit::textChanged, this, [this] { if (jumpAsType()) refreshTimer.start(); });
        connect(find, &QLineEdit::returnPressed, this, [this] { findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier); });
        connect(repl, &QLineEdit::returnPressed, this, [this] { replaceOne(); });
        for (auto *b : {cs, ww, rx}) connect(b, &QToolButton::toggled, this, [this] { refresh(); });
        connect(up, &QToolButton::clicked, this, [this] { findNext(true); });
        connect(dn, &QToolButton::clicked, this, [this] { findNext(false); });
        connect(cl, &QToolButton::clicked, this, [this] { closeBar(); });
        connect(one, &QPushButton::clicked, this, [this] { replaceOne(); });
        connect(all, &QPushButton::clicked, this, [this] { replaceAll(); });
        find->installEventFilter(this);
        repl->installEventFilter(this);
    }

    void openBar(bool withReplace) {
        CodeEditor *ed = editor ? editor() : nullptr;
        if (ed) {
            const QString sel = ed->textCursor().selectedText();
            if (!sel.isEmpty() && !sel.contains(QChar::ParagraphSeparator)) find->setText(sel);
        }
        replRow->setVisible(withReplace);
        show();
        find->setFocus();
        find->selectAll();
        refresh();
    }
    void closeBar() {
        hide();
        if (CodeEditor *ed = editor ? editor() : nullptr) { ed->setSearchSelections({}); ed->setFocus(); }
    }
    void refresh() {
        CodeEditor *ed = editor ? editor() : nullptr;
        if (!ed || !isVisible()) return;
        QRegularExpression re;
        if (find->text().isEmpty()) { ed->setSearchSelections({}); count->setText(QString()); setBad(false); return; }
        if (!makeRegex(re)) { ed->setSearchSelections({}); count->setText(QStringLiteral("regex inválida")); setBad(true); return; }
        setBad(false);
        QList<QTextEdit::ExtraSelection> sels;
        int total = 0, cur = 0;
        const int selStart = ed->textCursor().selectionStart();
        QTextCursor c(ed->document());
        while (total < 5000) {
            c = ed->document()->find(re, c);
            if (c.isNull()) break;
            if (!c.hasSelection()) { if (c.atEnd()) break; c.movePosition(QTextCursor::NextCharacter); continue; }
            ++total;
            if (c.selectionStart() == selStart && ed->textCursor().hasSelection()) cur = total;
            QTextEdit::ExtraSelection s;
            s.format.setBackground(QColor(196, 18, 47, 85));
            s.cursor = c;
            sels.append(s);
        }
        ed->setSearchSelections(sels);
        if (total == 0) count->setText(QStringLiteral("Nenhum resultado"));
        else count->setText(cur ? QStringLiteral("%1 de %2").arg(cur).arg(total) : QStringLiteral("%1 resultado(s)").arg(total));
    }
    bool findNext(bool back) {
        CodeEditor *ed = editor ? editor() : nullptr;
        QRegularExpression re;
        if (!ed || find->text().isEmpty() || !makeRegex(re)) return false;
        QTextDocument::FindFlags fl = back ? QTextDocument::FindBackward : QTextDocument::FindFlags();
        QTextCursor c = ed->document()->find(re, ed->textCursor(), fl);
        if (c.isNull()) {  // volta ao comeco (ou ao fim)
            QTextCursor start(ed->document());
            if (back) start.movePosition(QTextCursor::End);
            c = ed->document()->find(re, start, fl);
        }
        if (c.isNull()) { refresh(); return false; }
        ed->setTextCursor(c);
        ed->centerCursor();
        refresh();
        return true;
    }
    void replaceOne() {
        CodeEditor *ed = editor ? editor() : nullptr;
        QRegularExpression re;
        if (!ed || find->text().isEmpty() || !makeRegex(re)) return;
        QTextCursor c = ed->textCursor();
        if (c.hasSelection()) {
            const QRegularExpressionMatch m = re.match(c.selectedText());
            if (m.hasMatch() && m.capturedLength() == c.selectedText().size()) {
                QString s = c.selectedText();
                s.replace(re, repl->text());
                c.insertText(s);
                ed->setTextCursor(c);
            }
        }
        findNext(false);
    }
    void replaceAll() {
        CodeEditor *ed = editor ? editor() : nullptr;
        QRegularExpression re;
        if (!ed || find->text().isEmpty() || !makeRegex(re)) return;
        QTextCursor c(ed->document());
        c.beginEditBlock();
        int n = 0;
        QTextCursor pos(ed->document());
        while (n < 100000) {
            pos = ed->document()->find(re, pos);
            if (pos.isNull()) break;
            if (!pos.hasSelection()) { if (pos.atEnd()) break; pos.movePosition(QTextCursor::NextCharacter); continue; }
            QString s = pos.selectedText();
            s.replace(re, repl->text());
            pos.insertText(s);
            ++n;
        }
        c.endEditBlock();
        refresh();
        count->setText(QStringLiteral("%1 substituído(s)").arg(n));
    }
    bool eventFilter(QObject *o, QEvent *e) override {
        if ((o == find || o == repl) && e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) { closeBar(); return true; }
        return QWidget::eventFilter(o, e);
    }

private:
    QToolButton *cs, *ww, *rx;
    QLabel *count;
    QWidget *replRow;
    QTimer refreshTimer;

    bool jumpAsType() const { return true; }
    void setBad(bool b) { find->setStyleSheet(b ? QStringLiteral("QLineEdit{border:1px solid #ff3b3b;}") : QString()); }
    bool makeRegex(QRegularExpression &re) const {
        QString pat = rx->isChecked() ? find->text() : QRegularExpression::escape(find->text());
        if (ww->isChecked()) pat = QStringLiteral("\\b(?:") + pat + QStringLiteral(")\\b");
        re = QRegularExpression(pat, cs->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        return re.isValid();
    }
};

// ============================================================================
//  Ir para arquivo (Ctrl+P)
// ============================================================================
class QuickOpen : public QDialog {
public:
    QString chosen;
    QuickOpen(QWidget *parent, const QString &root, const QStringList &extra) : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint), root(root) {
        setObjectName("quickOpen");
        resize(620, 380);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(8, 8, 8, 8);
        edit = new QLineEdit;
        edit->setPlaceholderText(QStringLiteral("Digite o nome do arquivo (Ctrl+P)"));
        list = new QListWidget;
        list->setUniformItemSizes(true);
        v->addWidget(edit);
        v->addWidget(list, 1);
        if (!root.isEmpty()) {
            QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
            while (it.hasNext() && files.size() < 30000) {
                const QString p = it.next();
                const QString rel = QDir(root).relativeFilePath(p);
                bool skip = false;
                for (const QString &part : rel.split(QLatin1Char('/'))) if (ignoredDirName(part)) { skip = true; break; }
                if (!skip) files << rel;
            }
        }
        for (const QString &x : extra) if (!files.contains(x)) files << x;
        refill();
        connect(edit, &QLineEdit::textChanged, this, [this] { refill(); });
        connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) { choose(it); });
        connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { choose(it); });
        edit->installEventFilter(this);
        edit->setFocus();
    }
    bool eventFilter(QObject *o, QEvent *e) override {
        if (o == edit && e->type() == QEvent::KeyPress) {
            auto *k = static_cast<QKeyEvent *>(e);
            if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) {
                const int r = list->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1);
                list->setCurrentRow(std::max(0, std::min(r, list->count() - 1)));
                return true;
            }
            if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) { if (list->currentItem()) choose(list->currentItem()); return true; }
        }
        return QDialog::eventFilter(o, e);
    }

private:
    QString root;
    QLineEdit *edit;
    QListWidget *list;
    QStringList files;
    void choose(QListWidgetItem *it) {
        const QString rel = it->data(Qt::UserRole).toString();
        chosen = QFileInfo(rel).isAbsolute() ? rel : QDir(root).absoluteFilePath(rel);
        QDialog::accept();
    }
    void refill() {
        list->clear();
        const QString q = edit->text().trimmed();
        QList<QPair<int, QString>> scored;
        for (const QString &f : files) {
            const int s = q.isEmpty() ? 0 : fuzzyScore(f, q);
            if (s >= 0) scored.append({s, f});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const QPair<int, QString> &a, const QPair<int, QString> &b) { return a.first > b.first; });
        for (int i = 0; i < scored.size() && i < 60; ++i) {
            auto *it = new QListWidgetItem(fileTypeIcon(scored[i].second), scored[i].second);
            it->setData(Qt::UserRole, scored[i].second);
            list->addItem(it);
        }
        if (list->count()) list->setCurrentRow(0);
    }
};

// ============================================================================
//  Tela de boas-vindas (o logo e o slogan do Display UI)
// ============================================================================
class WelcomeWidget : public QWidget {
public:
    std::function<void()> onNew, onOpenFile, onOpenFolder;
    std::function<void(const QString &)> onRecent;

    WelcomeWidget() {
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(56, 48, 56, 32);
        col = new QWidget;
        col->setObjectName("welcomeCol");
        col->setFixedWidth(430);
        col->setAttribute(Qt::WA_TranslucentBackground);
        lay = new QVBoxLayout(col);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(2);
        outer->addWidget(col, 0, Qt::AlignLeft | Qt::AlignTop);
        outer->addStretch(1);
        auto head = [&](const QString &t) { auto *l = new QLabel(t); l->setObjectName("welcomeHead"); lay->addSpacing(14); lay->addWidget(l); };
        head(QStringLiteral("Iniciar"));
        link(QStringLiteral("Novo arquivo\tCtrl+N"), [this] { if (onNew) onNew(); });
        link(QStringLiteral("Abrir arquivo…\tCtrl+O"), [this] { if (onOpenFile) onOpenFile(); });
        link(QStringLiteral("Abrir pasta…\tCtrl+Shift+O"), [this] { if (onOpenFolder) onOpenFolder(); });
        recentHead = new QLabel(QStringLiteral("Recentes"));
        recentHead->setObjectName("welcomeHead");
        lay->addSpacing(14);
        lay->addWidget(recentHead);
        recentBox = new QVBoxLayout;
        recentBox->setSpacing(0);
        lay->addLayout(recentBox);
        setRecent({});
    }
    void setRecent(const QStringList &paths) {
        while (QLayoutItem *it = recentBox->takeAt(0)) { delete it->widget(); delete it; }
        recentHead->setVisible(!paths.isEmpty());
        for (int i = 0; i < paths.size() && i < 7; ++i) {
            const QString p = paths[i];
            auto *b = new QPushButton(QFileInfo(p).fileName() + QStringLiteral("   ") + QDir::toNativeSeparators(QFileInfo(p).absolutePath()).replace(QDir::homePath(), QStringLiteral("~")));
            b->setObjectName("welcomeLink");
            b->setFlat(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral("text-align:left;"));
            connect(b, &QPushButton::clicked, this, [this, p] { if (onRecent) onRecent(p); });
            recentBox->addWidget(b);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient g(0, 0, width(), height());
        g.setColorAt(0, QColor::fromRgbF(0.11, 0.015, 0.03));
        g.setColorAt(0.55, QColor::fromRgbF(0.15, 0.02, 0.05));
        g.setColorAt(1, QColor::fromRgbF(0.25, 0.03, 0.07));
        p.fillRect(rect(), g);
        const qreal W = width(), H = height();
        for (int i = 0; i < 4; ++i) {  // ondas decorativas
            QPainterPath w;
            w.moveTo(W * (0.60 + i * 0.024), H);
            w.cubicTo(W * 0.75, H * (0.84 - i * 0.05), W * 0.87, H * (0.53 + i * 0.03), W, H * (0.29 + i * 0.056));
            w.lineTo(W, H);
            w.closeSubpath();
            p.fillPath(w, hx(0xc4122f, 20 + i * 12));
        }
        if (W < 760) return;
        const qreal cx = W * 0.78, s = std::min(1.0, std::min(W / 1100.0, H / 560.0));
        const qreal top = H * 0.5 - 150 * s;
        p.setPen(Qt::NoPen);
        p.fillPath(logoPath(cx, top, 190 * s, 155 * s), hx(0xb0142e, 242));
        QFont f = font(); f.setBold(true); f.setPixelSize(int(46 * s));
        p.setFont(f);
        const QFontMetrics fm(f);
        const QString a = QStringLiteral("Cisma "), b = QStringLiteral("Code");
        const qreal tw = fm.horizontalAdvance(a) + fm.horizontalAdvance(b);
        const qreal ty = top + 155 * s + 62 * s;
        p.setPen(hx(0xeadada)); p.drawText(QPointF(cx - tw / 2, ty), a);
        p.setPen(hx(0xd01632)); p.drawText(QPointF(cx - tw / 2 + fm.horizontalAdvance(a), ty), b);
        QFont f2 = font(); f2.setPixelSize(int(17 * s));
        p.setFont(f2); p.setPen(hx(0xe8dcdc));
        const QString slogan = QStringLiteral("Leve. Poderoso. Completo.");
        p.drawText(QPointF(cx - QFontMetrics(f2).horizontalAdvance(slogan) / 2.0, ty + 36 * s), slogan);
    }

private:
    QWidget *col;
    QVBoxLayout *lay, *recentBox;
    QLabel *recentHead;
    void link(const QString &text, std::function<void()> fn) {
        const QStringList parts = text.split(QLatin1Char('\t'));
        auto *b = new QPushButton(parts[0] + (parts.size() > 1 ? QStringLiteral("      ") + parts[1] : QString()));
        b->setObjectName("welcomeLink");
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("text-align:left;"));
        connect(b, &QPushButton::clicked, this, [fn] { fn(); });
        lay->addWidget(b);
    }
};

// ============================================================================
//  Estilo (QSS) e paleta escura
// ============================================================================
static QString appStyleSheet() {
    QString q = QStringLiteral(R"(
QWidget { font-family: "Inter", "Noto Sans", "DejaVu Sans", sans-serif; font-size: 13px; color: @txt; }
QMainWindow, QDialog, QMessageBox { background: @editor; }
QToolTip { background: @side; color: @txt; border: 1px solid #55101f; padding: 3px 6px; }
QMenuBar { background: @title; padding: 3px 6px; spacing: 2px; }
QMenuBar::item { padding: 7px 11px; background: transparent; border-radius: 4px; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: @sel; }
QMenu { background: @side; border: 1px solid #55101f; padding: 5px; }
QMenu::item { padding: 6px 30px 6px 22px; border-radius: 4px; }
QMenu::item:selected { background: @sel; }
QMenu::item:disabled { color: #7a6065; }
QMenu::separator { height: 1px; background: #4a0a18; margin: 5px 8px; }
#titleCorner { background: @title; }
#titleCorner QLabel#titleText { font-size: 15px; font-weight: bold; color: #e8d4d6; }
#activityBar { background: @act; }
#activityBar QToolButton { border: none; border-left: 3px solid transparent; border-radius: 0; padding: 0; background: transparent; }
#activityBar QToolButton:hover { background: rgba(255,255,255,0.06); }
#activityBar QToolButton:checked { border-left: 3px solid @accent; background: rgba(255,255,255,0.04); }
#sidebar, #sidePage { background: @side; }
#sideHeader { color: @dim; font-size: 11px; font-weight: bold; letter-spacing: 1px; padding: 4px 0; }
#rootLabel { font-weight: bold; font-size: 12px; padding: 4px 6px; }
#sideHint { color: @dim; }
QTreeView, QListWidget, QTreeWidget, QTableWidget, QAbstractItemView { background: @side; border: none; outline: 0; selection-background-color: @sel; selection-color: white; }
QTreeView::item, QListWidget::item, QTreeWidget::item { padding: 3px 2px; }
QTreeView::item:hover, QListWidget::item:hover, QTreeWidget::item:hover { background: rgba(255,255,255,0.05); }
QTreeView::item:selected, QListWidget::item:selected, QTreeWidget::item:selected { background: @sel; color: white; }
QHeaderView::section { background: @panel; color: @dim; border: none; border-bottom: 1px solid #4a0a18; padding: 4px 8px; }
QTableWidget { gridline-color: transparent; alternate-background-color: rgba(255,255,255,0.02); }
QLineEdit { background: #3a0a16; border: 1px solid #55101f; border-radius: 4px; padding: 4px 7px; selection-background-color: @accent; }
QLineEdit:focus { border: 1px solid @accent; }
QPushButton { background: @accent; color: white; border: none; border-radius: 4px; padding: 5px 13px; }
QPushButton:hover { background: #d8162f; }
QPushButton:disabled { background: #4a2a30; color: #8a7075; }
QPushButton:flat { background: transparent; }
#welcomeLink { background: transparent; color: #f4a3ad; padding: 5px 0; text-align: left; border: none; }
#welcomeLink:hover { color: white; }
#welcomeHead { color: @dim; font-size: 12px; font-weight: bold; letter-spacing: 1px; }
QToolButton { background: transparent; border: none; border-radius: 4px; padding: 3px 7px; color: @dim; }
QToolButton:hover { background: rgba(255,255,255,0.09); color: @txt; }
QToolButton:checked { background: @sel; color: white; }
QCheckBox { spacing: 6px; }
QTabBar#editorTabs { background: @tabbar; }
QTabBar#editorTabs::tab { background: @tabbar; color: @dim; padding: 8px 6px 8px 14px; border: none; border-bottom: 3px solid transparent; min-width: 70px; }
QTabBar#editorTabs::tab:selected { background: #2a0510; color: @txt; border-bottom: 3px solid @accent; }
QTabBar#editorTabs::tab:hover:!selected { background: #240509; color: @txt; }
QTabBar#editorTabs QToolButton { padding: 0; }
#breadcrumb { background: #1f0408; color: @dim; font-size: 12px; padding: 5px 14px; }
QTabWidget#panelTabs::pane { border: none; background: @panel; }
QTabWidget#panelTabs > QTabBar { background: @panel; }
QTabWidget#panelTabs > QTabBar::tab { background: transparent; color: @dim; padding: 8px 14px; font-size: 11px; border-bottom: 3px solid transparent; margin-right: 4px; }
QTabWidget#panelTabs > QTabBar::tab:selected { color: @txt; border-bottom: 3px solid @accent; }
QTabWidget#panelTabs > QTabBar::tab:hover:!selected { color: @txt; }
QPlainTextEdit { background: @editor; color: @txt; selection-background-color: #7a1428; border: none; }
QPlainTextEdit#outText { background: @panel; }
#termRow { background: @panel; border-top: 1px solid #3a0a16; }
#termPrompt { color: @green; font-weight: bold; }
#termRow QLineEdit { background: transparent; border: none; }
#findBar { background: @side; border-top: 1px solid #4a0a18; }
#findCount { color: @dim; }
#quickOpen { background: @side; border: 1px solid @accent; }
QStatusBar { background: @status; }
QStatusBar::item { border: none; }
QStatusBar QToolButton, QStatusBar QLabel { color: @txt; padding: 0 9px; font-size: 12px; border-radius: 0; }
QStatusBar QToolButton:hover { background: rgba(255,255,255,0.12); }
QSplitter::handle { background: #3a0612; }
QSplitter::handle:horizontal { width: 2px; }
QSplitter::handle:vertical { height: 2px; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: #5a1a28; border-radius: 5px; min-height: 32px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: #7a2236; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:horizontal { background: #5a1a28; border-radius: 5px; min-width: 32px; margin: 2px; }
QScrollBar::handle:horizontal:hover { background: #7a2236; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)");
    const QList<QPair<QString, QColor>> m = {
        {"@title", Pal::BG_TITLE}, {"@act", Pal::BG_ACT}, {"@side", Pal::BG_SIDE}, {"@editor", Pal::BG_EDITOR}, {"@panel", Pal::BG_PANEL},
        {"@status", Pal::BG_STATUS}, {"@tabbar", Pal::BG_TABBAR}, {"@sel", Pal::BG_SEL}, {"@accent", Pal::ACCENT}, {"@txt", Pal::TXT},
        {"@dim", Pal::TXT_DIM}, {"@green", Pal::GREEN}};
    for (const auto &p : m) q.replace(p.first, cssColor(p.second));
    return q;
}

static void applyDarkPalette(QApplication &app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, Pal::BG_EDITOR);
    p.setColor(QPalette::WindowText, Pal::TXT);
    p.setColor(QPalette::Base, Pal::BG_SIDE);
    p.setColor(QPalette::AlternateBase, Pal::BG_PANEL);
    p.setColor(QPalette::Text, Pal::TXT);
    p.setColor(QPalette::Button, Pal::BG_SEL);
    p.setColor(QPalette::ButtonText, Pal::TXT);
    p.setColor(QPalette::Highlight, Pal::ACCENT);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, Pal::BG_SIDE);
    p.setColor(QPalette::ToolTipText, Pal::TXT);
    p.setColor(QPalette::PlaceholderText, hx(0x8a7378));
    p.setColor(QPalette::Link, hx(0xf4a3ad));
    p.setColor(QPalette::Disabled, QPalette::Text, hx(0x7a6065));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, hx(0x7a6065));
    app.setPalette(p);
    app.setStyleSheet(appStyleSheet());
}

// ============================================================================
//  Arvore do explorador: pastas primeiro, sem .git / __pycache__
// ============================================================================
class TreeProxy : public QSortFilterProxyModel {
public:
    explicit TreeProxy(QObject *parent = nullptr) : QSortFilterProxyModel(parent) {}
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
        auto *fs = static_cast<QFileSystemModel *>(sourceModel());
        const QModelIndex i = fs->index(row, 0, parent);
        const QString n = fs->fileName(i);
        return !(fs->isDir(i) && (n == QLatin1String(".git") || n == QLatin1String("__pycache__")));
    }
    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override {
        auto *fs = static_cast<QFileSystemModel *>(sourceModel());
        const bool ld = fs->isDir(l), rd = fs->isDir(r);
        if (ld != rd) return ld;
        return QString::compare(fs->fileName(l), fs->fileName(r), Qt::CaseInsensitive) < 0;
    }
};

struct Problem { int line = 1, col = 1; QString msg; bool error = true; };

// ============================================================================
//  Janela principal
// ============================================================================
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(bool singleInstance = true) : cfg(QStringLiteral("cisma-os"), QStringLiteral("cisma-code")) {
        setWindowTitle(QStringLiteral("Cisma Code"));
        setWindowIcon(logoIcon());
        setAcceptDrops(true);
        resize(1280, 800);
        fontPx = std::max(8, std::min(40, cfg.value("font/px", 14).toInt()));
        wrap = cfg.value("wrap", false).toBool();
        minimapOn = cfg.value("minimap", true).toBool();
        loadBps();
        buildUi();
        buildMenus();
        buildStatusBar();
        if (singleInstance) startServer();  // (--grab nao mexe no socket de uma janela que ja esteja aberta)

        checkTimer.setSingleShot(true);
        checkTimer.setInterval(700);
        connect(&checkTimer, &QTimer::timeout, this, [this] { if (CodeEditor *e = current()) runChecks(e); });
        findTimer.setSingleShot(true);
        findTimer.setInterval(150);
        connect(&findTimer, &QTimer::timeout, this, [this] { findBar->refresh(); });
        portsTimer.setInterval(3000);
        connect(&portsTimer, &QTimer::timeout, this, [this] { if (panelTabs->currentWidget() == portsPage && panelTabs->isVisible()) refreshPorts(); });
        portsTimer.start();
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
            if (s == Qt::ApplicationActive) { checkExternalChanges(); refreshGit(); }
        });
        runProcess(QStringLiteral("python3"), {QStringLiteral("--version")}, QString(), [this](int code, const QString &o) {
            if (code == 0) { pyVersion = o.trimmed().section(QLatin1Char(' '), 1, 1); updateStatus(); }
        });
        updateWelcome();
        updateStatus();
    }

    ~MainWindow() override {
        // nenhum processo pode emitir sinais para uma janela que ja esta sendo destruida
        stopRun(true);
        dbgOn = false;
        if (dbgSock) dbgSock->disconnect(this);
        if (dbgServer) dbgServer->disconnect(this);
        for (QProcess *p : findChildren<QProcess *>()) {
            p->disconnect();
            if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(300); }
        }
    }

    // ---------- API usada pelo main() ----------
    CodeEditor *current() const {
        return edStack->count() ? static_cast<CodeEditor *>(edStack->currentWidget()) : nullptr;
    }
    QList<CodeEditor *> editors() const {
        QList<CodeEditor *> l;
        for (int i = 0; i < edStack->count(); ++i) l << static_cast<CodeEditor *>(edStack->widget(i));
        return l;
    }
    int editorCount() const { return edStack->count(); }

    bool openFile(const QString &pathIn, int line = -1, int col = 1) {
        QFileInfo fi(pathIn);
        const QString path = fi.exists() ? fi.canonicalFilePath() : fi.absoluteFilePath();
        for (int i = 0; i < edStack->count(); ++i) {
            auto *e = static_cast<CodeEditor *>(edStack->widget(i));
            if (!e->filePath.isEmpty() && QFileInfo(e->filePath).canonicalFilePath() == path) {
                tabBar->setCurrentIndex(i);
                if (line > 0) e->gotoLine(line, col);
                e->setFocus();
                return true;
            }
        }
        if (!fi.exists()) {
            QMessageBox::warning(this, QStringLiteral("Abrir arquivo"), QStringLiteral("O arquivo não existe:\n%1").arg(path));
            return false;
        }
        if (fi.isDir()) { openFolder(path); return true; }
        if (fi.size() > 20 * 1024 * 1024 &&
            QMessageBox::question(this, QStringLiteral("Arquivo grande"), QStringLiteral("%1 tem %2 MB. Abrir mesmo assim?").arg(fi.fileName()).arg(fi.size() / (1024 * 1024))) != QMessageBox::Yes)
            return false;
        const LoadResult r = loadTextFile(path);
        if (!r.ok) {
            QMessageBox::warning(this, QStringLiteral("Abrir arquivo"), QStringLiteral("Não consegui abrir %1:\n%2").arg(fi.fileName(), r.err));
            return false;
        }
        CodeEditor *e = newEditor();
        e->filePath = path;
        e->crlf = r.crlf;
        e->enc = r.enc;
        e->diskTime = fi.lastModified();
        e->setPlainText(r.text);
        e->document()->setModified(false);
        e->setLanguage(fi.size() > 3 * 1024 * 1024 ? plainLang() : langForFile(path));
        if (e->lang->tabs) e->useSpaces = false;
        else detectIndent(e, r.text);
        e->setBreakpoints(bpStore.value(path));
        addEditorTab(e);
        rememberRecent(QStringLiteral("recent/files"), path);
        runChecks(e);
        if (line > 0) e->gotoLine(line, col);
        e->setFocus();
        return true;
    }

    void newFile() {
        CodeEditor *e = newEditor();
        e->setProperty("untitled", ++untitledCount);
        e->setLanguage(plainLang());
        addEditorTab(e);
        e->setFocus();
    }

    void openFolder(const QString &dir) {
        const QFileInfo fi(dir);
        if (!fi.isDir()) return;
        root = fi.canonicalFilePath();
        fsModel->setRootPath(root);
        tree->setRootIndex(proxy->mapFromSource(fsModel->index(root)));
        rootLabel->setText(QFileInfo(root).fileName().toUpper());
        rootLabel->setToolTip(root);
        explorerStack->setCurrentIndex(1);
        term->setWorkDir(root);
        rememberRecent(QStringLiteral("recent/folders"), root);
        searchResults->clear();
        searchStatus->clear();
        showSidebar(0);
        refreshGit();
        updateTitle();
        updateWelcome();
    }

    void restoreSession() {
        const QString folder = cfg.value("session/folder").toString();
        if (!folder.isEmpty() && QFileInfo(folder).isDir()) openFolder(folder);
        const QStringList files = cfg.value("session/files").toStringList();
        for (const QString &f : files) if (QFileInfo::exists(f)) openFile(f);
        const int idx = cfg.value("session/current", 0).toInt();
        if (idx >= 0 && idx < edStack->count()) tabBar->setCurrentIndex(idx);
    }
    void restoreWindow() {
        restoreGeometry(cfg.value("window/geometry").toByteArray());
        restoreState(cfg.value("window/state").toByteArray());
        const bool pv = cfg.value("panel/visible", true).toBool();
        panelTabs->setVisible(pv);
        const QList<int> hs = {height() - 240, 240};
        vsplit->setSizes(hs);
        const int sv = cfg.value("side/index", 0).toInt();
        if (cfg.value("side/visible", true).toBool()) showSidebar(sv); else { sideStack->setCurrentIndex(sv); sidebar->hide(); }
    }
    // so para testes (--grab ... --do ACAO): faz algo na janela antes de gravar o PNG
    void demo(const QString &a) {
        if (a == QLatin1String("run")) runCurrent();
        else if (a == QLatin1String("term")) { showPanel(term); term->sendLine(QStringLiteral("echo oi do terminal && ls")); }
        else if (a == QLatin1String("git")) showSidebar(2);
        else if (a == QLatin1String("problems")) showPanel(problemsPage);
        else if (a == QLatin1String("ports")) showPanel(portsPage);
        else if (a == QLatin1String("find")) { findBar->find->setText(QStringLiteral("print")); findBar->openBar(true); }
        else if (a == QLatin1String("welcome")) { for (int k = edStack->count() - 1; k >= 0; --k) removeEditor(static_cast<CodeEditor *>(edStack->widget(k))); }
        else if (a.startsWith(QLatin1String("termcmd:"))) { showPanel(term); vsplit->setSizes({160, 640}); term->sendLine(a.mid(8)); }
        else if (a.startsWith(QLatin1String("debug:")) || a.startsWith(QLatin1String("debugnext:"))) {   // ponto de parada na linha N, depura e (opcional) da um passo
            const bool next = a.startsWith(QLatin1String("debugnext:"));
            if (CodeEditor *e = current()) e->toggleBreakpoint(a.section(QLatin1Char(':'), 1).toInt());
            runCurrent(true);
            if (next) QTimer::singleShot(1500, this, [this] { dbgResume(QStringLiteral("next")); });
        }
        else if (a.startsWith(QLatin1String("search:"))) { showSidebar(1); searchEdit->setText(a.mid(7)); runSearch(); }
    }
    void grabAndSave(const QString &out, const QString &action = QString()) {
        resize(1672, 941);
        show();
        QApplication::processEvents();
        if (!action.isEmpty()) demo(action);
        QTimer::singleShot(action.isEmpty() ? 300 : 2500, this, [this, out] { grab().save(out); QApplication::exit(0); });
    }
    void handleRemote(const QStringList &args) {
        for (const QString &a : args) {
            QString p = a; int line = -1;
            static const QRegularExpression re(QStringLiteral("^(.*):(\\d+)$"));
            const auto m = re.match(a);
            if (m.hasMatch() && !QFileInfo::exists(a)) { p = m.captured(1); line = m.captured(2).toInt(); }
            if (QFileInfo(p).isDir()) openFolder(p); else openFile(p, line);
        }
        if (isMinimized()) showNormal();
        raise();
        activateWindow();
    }

protected:
    void closeEvent(QCloseEvent *ev) override {
        searchCancel = true;
        for (CodeEditor *e : editors()) {
            if (!maybeSave(e)) { ev->ignore(); return; }
        }
        QStringList files;
        for (CodeEditor *e : editors()) if (!e->filePath.isEmpty()) files << e->filePath;
        saveBps();
        cfg.setValue("session/files", files);
        cfg.setValue("session/current", tabBar->currentIndex());
        cfg.setValue("session/folder", root);
        cfg.setValue("window/geometry", saveGeometry());
        cfg.setValue("window/state", saveState());
        cfg.setValue("panel/visible", panelTabs->isVisible());
        cfg.setValue("side/visible", sidebar->isVisible());
        cfg.setValue("side/index", sideStack->currentIndex());
        cfg.sync();
        stopRun(true);
        QMainWindow::closeEvent(ev);
    }
    void dragEnterEvent(QDragEnterEvent *e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
    void dropEvent(QDropEvent *e) override {
        for (const QUrl &u : e->mimeData()->urls()) {
            if (!u.isLocalFile()) continue;
            if (QFileInfo(u.toLocalFile()).isDir()) openFolder(u.toLocalFile()); else openFile(u.toLocalFile());
        }
    }

private:
    QSettings cfg;
    int fontPx = 14;
    bool wrap = false;
    int untitledCount = 0;
    QString root, pyVersion;
    QStringList notifications;

    // widgets
    QWidget *actBar = nullptr;
    QList<QToolButton *> actButtons;
    QStackedWidget *sideStack = nullptr, *explorerStack = nullptr, *paneStack = nullptr, *edStack = nullptr;
    QWidget *sidebar = nullptr;
    QSplitter *hsplit = nullptr, *vsplit = nullptr;
    QTabBar *tabBar = nullptr;
    QLabel *breadcrumb = nullptr, *rootLabel = nullptr, *msgLabel = nullptr;
    WelcomeWidget *welcome = nullptr;
    FindBar *findBar = nullptr;
    QFileSystemModel *fsModel = nullptr;
    TreeProxy *proxy = nullptr;
    CismaIcons iconProvider;
    QTreeView *tree = nullptr;
    // pesquisa
    QLineEdit *searchEdit = nullptr, *includeEdit = nullptr;
    QToolButton *searchCase = nullptr, *searchRegex = nullptr;
    QTreeWidget *searchResults = nullptr;
    QLabel *searchStatus = nullptr;
    bool searchRunning = false, searchCancel = false;
    // git
    QLabel *gitBranchLbl = nullptr, *gitHint = nullptr;
    QListWidget *gitList = nullptr;
    QLineEdit *gitMsg = nullptr;
    QPushButton *gitCommitBtn = nullptr, *gitInitBtn = nullptr;
    QString gitBranch;
    bool gitDirty = false, gitRepo = false;
    // executar
    QLabel *runFileLbl = nullptr;
    QLineEdit *runCmd = nullptr;
    QCheckBox *runSave = nullptr;
    QProcess *runProc = nullptr;
    // depurador
    QLocalServer *dbgServer = nullptr;
    QLocalSocket *dbgSock = nullptr;
    QByteArray dbgBuf;
    bool dbgOn = false, dbgPaused = false;
    QWidget *dpPage = nullptr;
    QListWidget *dpStack = nullptr;
    QTreeWidget *dpVars = nullptr;
    QPlainTextEdit *dpLog = nullptr;
    QLineEdit *dpEval = nullptr;
    QLabel *dpState = nullptr;
    QList<QToolButton *> dpBtns;
    QStringList dpFrameFiles;
    QList<int> dpFrameLines;
    QMap<QString, QList<int>> bpStore;
    QAction *actMinimap = nullptr;
    bool minimapOn = true;
    QElapsedTimer runClock;
    // painel inferior
    QTabWidget *panelTabs = nullptr;
    QTreeWidget *problemsView = nullptr;
    QPlainTextEdit *runOut = nullptr;
    QLineEdit *runIn = nullptr;
    PtyTerm *term = nullptr, *dbg = nullptr;
    QTableWidget *portsTable = nullptr;
    QWidget *problemsPage = nullptr, *outPage = nullptr, *portsPage = nullptr;
    QTimer checkTimer, portsTimer, findTimer;
    QStringDecoder runDec{QStringConverter::Utf8};
    bool inExtCheck = false;
    QMap<CodeEditor *, QList<Problem>> problems;
    int checkGen = 0;
    // barra de status
    QToolButton *stBranch = nullptr, *stErrors = nullptr, *stWarns = nullptr, *stPos = nullptr, *stIndent = nullptr, *stEol = nullptr,
                *stLang = nullptr, *stBell = nullptr;
    QLabel *stEnc = nullptr, *stPy = nullptr;
    QTimer msgTimer;
    QLocalServer *server = nullptr;
    // acoes com estado
    QAction *actWrap = nullptr;

    // ======================================================================
    //  Construcao da interface
    // ======================================================================
    static QToolButton *flatBtn(const QString &text, const QString &tip) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    }

    void buildUi() {
        auto *central = new QWidget;
        auto *h = new QHBoxLayout(central);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        // ----- barra de atividades -----
        actBar = new QWidget;
        actBar->setObjectName("activityBar");
        actBar->setFixedWidth(56);
        auto *av = new QVBoxLayout(actBar);
        av->setContentsMargins(0, 8, 0, 8);
        av->setSpacing(2);
        struct A { ActKind k; const char *tip; };
        const A as[] = {{ActFiles, "Explorador (Ctrl+Shift+E)"}, {ActSearch, "Buscar (Ctrl+Shift+F)"}, {ActGit, "Git (Ctrl+Shift+G)"}, {ActRun, "Executar (Ctrl+Shift+D)"}};
        for (int i = 0; i < 4; ++i) {
            auto *b = new QToolButton;
            b->setIcon(actIcon(as[i].k));
            b->setIconSize(QSize(28, 28));
            b->setCheckable(true);
            b->setToolTip(QString::fromUtf8(as[i].tip));
            b->setFixedHeight(54);
            b->setFocusPolicy(Qt::NoFocus);
            connect(b, &QToolButton::clicked, this, [this, i] { toggleSidebar(i); });
            av->addWidget(b);
            actButtons << b;
        }
        av->addStretch(1);
        auto *gear = new QToolButton;
        gear->setIcon(actIcon(ActGear));
        gear->setIconSize(QSize(28, 28));
        gear->setFixedHeight(54);
        gear->setToolTip(QStringLiteral("Configurações"));
        gear->setFocusPolicy(Qt::NoFocus);
        gear->setPopupMode(QToolButton::InstantPopup);
        gearMenu = new QMenu(gear);
        gear->setMenu(gearMenu);
        gear->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;}"));
        av->addWidget(gear);
        h->addWidget(actBar);

        // ----- lateral + editor + painel -----
        hsplit = new QSplitter(Qt::Horizontal);
        hsplit->setHandleWidth(2);
        hsplit->setChildrenCollapsible(false);
        sidebar = new QWidget;
        sidebar->setObjectName("sidebar");
        auto *sv = new QVBoxLayout(sidebar);
        sv->setContentsMargins(0, 0, 0, 0);
        sideStack = new QStackedWidget;
        sv->addWidget(sideStack);
        sideStack->addWidget(buildExplorer());
        sideStack->addWidget(buildSearch());
        sideStack->addWidget(buildGit());
        sideStack->addWidget(buildRunPage());
        sidebar->setMinimumWidth(190);
        hsplit->addWidget(sidebar);

        vsplit = new QSplitter(Qt::Vertical);
        vsplit->setHandleWidth(2);
        vsplit->setChildrenCollapsible(false);
        vsplit->addWidget(buildEditorArea());
        vsplit->addWidget(buildPanel());
        vsplit->setStretchFactor(0, 1);
        vsplit->setStretchFactor(1, 0);
        hsplit->addWidget(vsplit);
        hsplit->setStretchFactor(0, 0);
        hsplit->setStretchFactor(1, 1);
        hsplit->setSizes({270, 1010});
        h->addWidget(hsplit, 1);
        setCentralWidget(central);
        showSidebar(0);
    }

    QWidget *sidePage() {
        auto *w = new QWidget;
        w->setObjectName("sidePage");
        return w;
    }
    QLabel *sideHeader(const QString &t) {
        auto *l = new QLabel(t);
        l->setObjectName("sideHeader");
        return l;
    }

    QWidget *buildExplorer() {
        QWidget *pg = sidePage();
        auto *v = new QVBoxLayout(pg);
        v->setContentsMargins(14, 10, 8, 8);
        v->setSpacing(4);
        v->addWidget(sideHeader(QStringLiteral("EXPLORADOR")));
        explorerStack = new QStackedWidget;
        // vazio
        auto *empty = new QWidget;
        auto *ev = new QVBoxLayout(empty);
        ev->setContentsMargins(0, 8, 6, 0);
        auto *hint = new QLabel(QStringLiteral("Nenhuma pasta aberta.\nAbra uma pasta para ver os arquivos do projeto."));
        hint->setObjectName("sideHint");
        hint->setWordWrap(true);
        ev->addWidget(hint);
        auto *b1 = new QPushButton(QStringLiteral("Abrir pasta"));
        connect(b1, &QPushButton::clicked, this, [this] { chooseFolder(); });
        auto *b2 = new QPushButton(QStringLiteral("Abrir arquivo"));
        connect(b2, &QPushButton::clicked, this, [this] { chooseFile(); });
        ev->addSpacing(6); ev->addWidget(b1); ev->addWidget(b2); ev->addStretch(1);
        explorerStack->addWidget(empty);
        // arvore
        auto *tw = new QWidget;
        auto *tv = new QVBoxLayout(tw);
        tv->setContentsMargins(0, 0, 0, 0);
        tv->setSpacing(2);
        auto *bar = new QHBoxLayout;
        bar->setSpacing(0);
        rootLabel = new QLabel;
        rootLabel->setObjectName("rootLabel");
        bar->addWidget(rootLabel, 1);
        auto *nf = flatBtn(QStringLiteral("＋ Arquivo"), QStringLiteral("Novo arquivo na pasta do projeto"));
        auto *nd = flatBtn(QStringLiteral("＋ Pasta"), QStringLiteral("Nova pasta na pasta do projeto"));
        auto *rf = flatBtn(QStringLiteral("↻"), QStringLiteral("Atualizar"));
        connect(nf, &QToolButton::clicked, this, [this] { createInDir(root, false); });
        connect(nd, &QToolButton::clicked, this, [this] { createInDir(root, true); });
        connect(rf, &QToolButton::clicked, this, [this] { if (!root.isEmpty()) { fsModel->setRootPath(QString()); fsModel->setRootPath(root); tree->setRootIndex(proxy->mapFromSource(fsModel->index(root))); } refreshGit(); });
        bar->addWidget(nf); bar->addWidget(nd); bar->addWidget(rf);
        tv->addLayout(bar);
        fsModel = new QFileSystemModel(this);
        fsModel->setIconProvider(&iconProvider);
        fsModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        proxy = new TreeProxy(this);
        proxy->setSourceModel(fsModel);
        proxy->setDynamicSortFilter(true);
        proxy->sort(0, Qt::AscendingOrder);
        tree = new QTreeView;
        tree->setModel(proxy);
        tree->setHeaderHidden(true);
        for (int c = 1; c < 4; ++c) tree->setColumnHidden(c, true);
        tree->setIndentation(14);
        tree->setIconSize(QSize(18, 18));
        tree->setExpandsOnDoubleClick(false);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        tree->setAnimated(false);
        connect(tree, &QTreeView::clicked, this, [this](const QModelIndex &i) {
            if (fsModel->isDir(proxy->mapToSource(i))) tree->setExpanded(i, !tree->isExpanded(i));
        });
        connect(tree, &QTreeView::activated, this, [this](const QModelIndex &i) {
            const QModelIndex s = proxy->mapToSource(i);
            if (fsModel->isDir(s)) tree->setExpanded(i, !tree->isExpanded(i)); else openFile(fsModel->filePath(s));
        });
        connect(tree, &QTreeView::customContextMenuRequested, this, [this](const QPoint &pt) { treeMenu(pt); });
        tv->addWidget(tree, 1);
        explorerStack->addWidget(tw);
        v->addWidget(explorerStack, 1);
        return pg;
    }

    QWidget *buildSearch() {
        QWidget *pg = sidePage();
        auto *v = new QVBoxLayout(pg);
        v->setContentsMargins(14, 10, 8, 8);
        v->setSpacing(6);
        v->addWidget(sideHeader(QStringLiteral("BUSCAR")));
        auto *row = new QHBoxLayout;
        row->setSpacing(2);
        searchEdit = new QLineEdit;
        searchEdit->setPlaceholderText(QStringLiteral("Buscar nos arquivos da pasta"));
        searchCase = flatBtn(QStringLiteral("Aa"), QStringLiteral("Diferenciar maiúsculas de minúsculas"));
        searchCase->setCheckable(true);
        searchRegex = flatBtn(QStringLiteral(".*"), QStringLiteral("Expressão regular"));
        searchRegex->setCheckable(true);
        row->addWidget(searchEdit, 1); row->addWidget(searchCase); row->addWidget(searchRegex);
        v->addLayout(row);
        includeEdit = new QLineEdit;
        includeEdit->setPlaceholderText(QStringLiteral("Arquivos (ex.: *.py, *.js)"));
        v->addWidget(includeEdit);
        searchStatus = new QLabel;
        searchStatus->setObjectName("sideHint");
        searchStatus->setWordWrap(true);
        v->addWidget(searchStatus);
        searchResults = new QTreeWidget;
        searchResults->setHeaderHidden(true);
        searchResults->setIndentation(12);
        searchResults->setUniformRowHeights(true);
        v->addWidget(searchResults, 1);
        connect(searchEdit, &QLineEdit::returnPressed, this, [this] { runSearch(); });
        connect(includeEdit, &QLineEdit::returnPressed, this, [this] { runSearch(); });
        connect(searchResults, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it, int) {
            const QString p = it->data(0, Qt::UserRole).toString();
            if (p.isEmpty()) return;
            if (it->childCount() > 0) { it->setExpanded(!it->isExpanded()); return; }
            openFile(p, it->data(0, Qt::UserRole + 1).toInt(), it->data(0, Qt::UserRole + 2).toInt());
        });
        return pg;
    }

    QWidget *buildGit() {
        QWidget *pg = sidePage();
        auto *v = new QVBoxLayout(pg);
        v->setContentsMargins(14, 10, 8, 8);
        v->setSpacing(6);
        v->addWidget(sideHeader(QStringLiteral("CONTROLE DE CÓDIGO (GIT)")));
        auto *row = new QHBoxLayout;
        gitBranchLbl = new QLabel;
        gitBranchLbl->setObjectName("rootLabel");
        auto *rf = flatBtn(QStringLiteral("↻"), QStringLiteral("Atualizar"));
        connect(rf, &QToolButton::clicked, this, [this] { refreshGit(); });
        row->addWidget(gitBranchLbl, 1); row->addWidget(rf);
        v->addLayout(row);
        gitHint = new QLabel;
        gitHint->setObjectName("sideHint");
        gitHint->setWordWrap(true);
        v->addWidget(gitHint);
        gitInitBtn = new QPushButton(QStringLiteral("Iniciar repositório"));
        connect(gitInitBtn, &QPushButton::clicked, this, [this] {
            if (root.isEmpty()) return;
            runProcess(QStringLiteral("git"), {QStringLiteral("init")}, root, [this](int, const QString &o) { printOut(o); refreshGit(); });
        });
        v->addWidget(gitInitBtn);
        gitList = new QListWidget;
        v->addWidget(gitList, 1);
        gitMsg = new QLineEdit;
        gitMsg->setPlaceholderText(QStringLiteral("Mensagem do commit"));
        v->addWidget(gitMsg);
        gitCommitBtn = new QPushButton(QStringLiteral("Commit (todas as alterações)"));
        v->addWidget(gitCommitBtn);
        connect(gitList, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
            const QString p = it->data(Qt::UserRole).toString();
            if (!p.isEmpty() && QFileInfo::exists(p)) openFile(p);
        });
        connect(gitMsg, &QLineEdit::returnPressed, this, [this] { doCommit(); });
        connect(gitCommitBtn, &QPushButton::clicked, this, [this] { doCommit(); });
        return pg;
    }

    QWidget *buildRunPage() {
        QWidget *pg = sidePage();
        auto *v = new QVBoxLayout(pg);
        v->setContentsMargins(14, 10, 8, 8);
        v->setSpacing(6);
        v->addWidget(sideHeader(QStringLiteral("EXECUTAR")));
        runFileLbl = new QLabel;
        runFileLbl->setObjectName("rootLabel");
        runFileLbl->setWordWrap(true);
        v->addWidget(runFileLbl);
        auto *l = new QLabel(QStringLiteral("Comando (por linguagem):"));
        l->setObjectName("sideHint");
        v->addWidget(l);
        runCmd = new QLineEdit;
        v->addWidget(runCmd);
        auto *hint = new QLabel(QStringLiteral("%f = arquivo   %d = pasta do arquivo   %b = nome sem extensão"));
        hint->setObjectName("sideHint");
        hint->setWordWrap(true);
        v->addWidget(hint);
        runSave = new QCheckBox(QStringLiteral("Salvar antes de executar"));
        runSave->setChecked(cfg.value("run/saveFirst", true).toBool());
        connect(runSave, &QCheckBox::toggled, this, [this](bool on) { cfg.setValue("run/saveFirst", on); });
        v->addWidget(runSave);
        auto *go = new QPushButton(QStringLiteral("▶  Executar (F5)"));
        auto *stop = new QPushButton(QStringLiteral("■  Parar (Shift+F5)"));
        connect(go, &QPushButton::clicked, this, [this] { runCurrent(); });
        connect(stop, &QPushButton::clicked, this, [this] { stopRun(false); });
        v->addWidget(go);
        v->addWidget(stop);
        auto *note = new QLabel(QStringLiteral("A saída aparece na aba SAÍDA. Se o programa pedir dados (input), digite na caixa abaixo da saída."));
        note->setObjectName("sideHint");
        note->setWordWrap(true);
        v->addSpacing(8);
        v->addWidget(note);
        v->addStretch(1);
        connect(runCmd, &QLineEdit::editingFinished, this, [this] {
            if (CodeEditor *e = current()) cfg.setValue(QStringLiteral("run/") + e->lang->id, runCmd->text());
        });
        return pg;
    }

    QWidget *buildEditorArea() {
        paneStack = new QStackedWidget;
        welcome = new WelcomeWidget;
        welcome->onNew = [this] { newFile(); };
        welcome->onOpenFile = [this] { chooseFile(); };
        welcome->onOpenFolder = [this] { chooseFolder(); };
        welcome->onRecent = [this](const QString &p) { openFile(p); };
        paneStack->addWidget(welcome);

        auto *pane = new QWidget;
        auto *v = new QVBoxLayout(pane);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        tabBar = new QTabBar;
        tabBar->setObjectName("editorTabs");
        tabBar->setExpanding(false);
        tabBar->setMovable(true);
        tabBar->setTabsClosable(false);
        tabBar->setElideMode(Qt::ElideRight);
        tabBar->setUsesScrollButtons(true);
        tabBar->setDrawBase(false);
        tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
        v->addWidget(tabBar);
        breadcrumb = new QLabel;
        breadcrumb->setObjectName("breadcrumb");
        v->addWidget(breadcrumb);
        edStack = new QStackedWidget;
        v->addWidget(edStack, 1);
        findBar = new FindBar;
        findBar->editor = [this] { return current(); };
        v->addWidget(findBar);
        paneStack->addWidget(pane);

        connect(tabBar, &QTabBar::currentChanged, this, [this](int i) {
            if (i >= 0 && i < edStack->count()) edStack->setCurrentIndex(i);
            onCurrentChanged();
        });
        connect(tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
            QWidget *w = edStack->widget(from);
            const bool wasCur = edStack->currentWidget() == w;
            edStack->removeWidget(w);
            edStack->insertWidget(to, w);
            if (wasCur) edStack->setCurrentWidget(w);
        });
        connect(tabBar, &QTabBar::tabBarDoubleClicked, this, [this](int i) { if (i < 0) newFile(); });
        connect(tabBar, &QTabBar::customContextMenuRequested, this, [this](const QPoint &pt) { tabMenu(pt); });
        return paneStack;
    }

    QWidget *buildPanel() {
        panelTabs = new QTabWidget;
        panelTabs->setObjectName("panelTabs");
        panelTabs->setDocumentMode(true);
        panelTabs->tabBar()->setDrawBase(false);
        panelTabs->setMinimumHeight(90);
        const QFont mono = monoFont(std::max(11, fontPx - 1));

        // PROBLEMAS
        problemsView = new QTreeWidget;
        problemsView->setColumnCount(4);
        problemsView->setHeaderLabels({QStringLiteral("Gravidade"), QStringLiteral("Mensagem"), QStringLiteral("Arquivo"), QStringLiteral("Linha")});
        problemsView->setRootIsDecorated(false);
        problemsView->setUniformRowHeights(true);
        problemsView->setStyleSheet(QStringLiteral("QTreeWidget{background:%1;}").arg(cssColor(Pal::BG_PANEL)));
        problemsView->header()->setStretchLastSection(false);
        problemsView->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        connect(problemsView, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it, int) {
            const quintptr ptr = it->data(0, Qt::UserRole + 3).value<quintptr>();
            for (CodeEditor *e : editors()) {
                if (quintptr(e) != ptr) continue;
                tabBar->setCurrentIndex(edStack->indexOf(e));
                e->gotoLine(it->data(0, Qt::UserRole + 1).toInt(), it->data(0, Qt::UserRole + 2).toInt());
                e->setFocus();
                break;
            }
        });
        problemsPage = problemsView;
        panelTabs->addTab(problemsView, QStringLiteral("PROBLEMAS"));

        // SAIDA
        outPage = new QWidget;
        auto *ov = new QVBoxLayout(outPage);
        ov->setContentsMargins(0, 0, 0, 0);
        ov->setSpacing(0);
        runOut = new QPlainTextEdit;
        runOut->setObjectName("outText");
        runOut->setReadOnly(true);
        runOut->setMaximumBlockCount(30000);
        runOut->setFont(mono);
        runOut->viewport()->installEventFilter(this);
        ov->addWidget(runOut, 1);
        runIn = new QLineEdit;
        runIn->setPlaceholderText(QStringLiteral("Entrada do programa (Enter envia)"));
        runIn->setFont(mono);
        runIn->hide();
        ov->addWidget(runIn);
        connect(runIn, &QLineEdit::returnPressed, this, [this] {
            if (!runProc || runProc->state() != QProcess::Running) return;
            const QString s = runIn->text();
            runIn->clear();
            printOut(s + QStringLiteral("\n"), Pal::FN);
            runProc->write(s.toUtf8() + "\n");
        });
        panelTabs->addTab(outPage, QStringLiteral("SAÍDA"));

        // DEPURAR (depurador de verdade: pontos de parada, passos, pilha, variaveis)
        dpPage = new QWidget;
        {
            auto *lay = new QVBoxLayout(dpPage);
            lay->setContentsMargins(8, 4, 8, 4);
            lay->setSpacing(4);
            auto *bar = new QHBoxLayout;
            auto mk = [&](const QString &t, const QString &tip, std::function<void()> fn) {
                auto *b = new QToolButton;
                b->setText(t);
                b->setToolTip(tip);
                b->setAutoRaise(true);
                b->setFocusPolicy(Qt::NoFocus);
                connect(b, &QToolButton::clicked, this, [fn] { fn(); });
                bar->addWidget(b);
                dpBtns << b;
            };
            mk(QStringLiteral("▶ Continuar"), QStringLiteral("Continuar (F8)"), [this] { dbgResume(QStringLiteral("continue")); });
            mk(QStringLiteral("⤼ Passo sobre"), QStringLiteral("Passo sobre (F10)"), [this] { dbgResume(QStringLiteral("next")); });
            mk(QStringLiteral("↴ Entrar"), QStringLiteral("Passo para dentro (F7)"), [this] { dbgResume(QStringLiteral("step")); });
            mk(QStringLiteral("↱ Sair"), QStringLiteral("Passo para fora (Shift+F10)"), [this] { dbgResume(QStringLiteral("out")); });
            mk(QStringLiteral("■ Parar"), QStringLiteral("Parar a depuração (Shift+F6)"), [this] { stopDebug(); });
            dpState = new QLabel(QStringLiteral("Sem depuração. F6 inicia (Python); F9 põe um ponto de parada."));
            dpState->setStyleSheet(QStringLiteral("color: %1;").arg(cssColor(Pal::TXT_DIM)));
            bar->addSpacing(10);
            bar->addWidget(dpState, 1);
            lay->addLayout(bar);
            auto *sp = new QSplitter(Qt::Horizontal);
            dpStack = new QListWidget;
            dpStack->setToolTip(QStringLiteral("Pilha de chamadas"));
            connect(dpStack, &QListWidget::currentRowChanged, this, [this](int r) { dbgSelectFrame(r); });
            dpVars = new QTreeWidget;
            dpVars->setColumnCount(3);
            dpVars->setHeaderLabels({QStringLiteral("Variável"), QStringLiteral("Tipo"), QStringLiteral("Valor")});
            dpVars->setRootIsDecorated(true);
            dpVars->setUniformRowHeights(true);
            dpVars->setColumnWidth(0, 160);
            dpVars->setColumnWidth(1, 90);
            auto *right = new QWidget;
            auto *rl = new QVBoxLayout(right);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->setSpacing(3);
            dpLog = new QPlainTextEdit;
            dpLog->setReadOnly(true);
            dpEval = new QLineEdit;
            dpEval->setPlaceholderText(QStringLiteral("Avaliar expressão no quadro selecionado…"));
            connect(dpEval, &QLineEdit::returnPressed, this, [this] { dbgEval(); });
            rl->addWidget(dpLog, 1);
            rl->addWidget(dpEval);
            sp->addWidget(dpStack);
            sp->addWidget(dpVars);
            sp->addWidget(right);
            sp->setSizes({220, 420, 320});
            lay->addWidget(sp, 1);
        }
        panelTabs->addTab(dpPage, QStringLiteral("DEPURAR"));

        // CONSOLE (Python)
        dbg = new PtyTerm(QStringLiteral("python3"), {QStringLiteral("-q")});
        dbg->setMono(mono);
        dbg->onOpenLine = [this](const QString &l) { openFromLine(l); };
        panelTabs->addTab(dbg, QStringLiteral("CONSOLE PYTHON"));

        // TERMINAL
        term = new PtyTerm(qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash")).isEmpty() ? QStringLiteral("/bin/bash") : qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash")), {});
        term->setMono(mono);
        term->onOpenLine = [this](const QString &l) { openFromLine(l); };
        panelTabs->addTab(term, QStringLiteral("TERMINAL"));

        // PORTAS
        portsPage = new QWidget;
        auto *pv = new QVBoxLayout(portsPage);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->setSpacing(0);
        portsTable = new QTableWidget(0, 4);
        portsTable->setHorizontalHeaderLabels({QStringLiteral("Porta"), QStringLiteral("Endereço"), QStringLiteral("Processo"), QStringLiteral("PID")});
        portsTable->verticalHeader()->hide();
        portsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        portsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        portsTable->setShowGrid(false);
        portsTable->setAlternatingRowColors(true);
        portsTable->horizontalHeader()->setStretchLastSection(true);
        portsTable->setStyleSheet(QStringLiteral("QTableWidget{background:%1;}").arg(cssColor(Pal::BG_PANEL)));
        connect(portsTable, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
            if (auto *it = portsTable->item(r, 0)) QDesktopServices::openUrl(QUrl(QStringLiteral("http://localhost:%1").arg(it->text())));
        });
        pv->addWidget(portsTable, 1);
        auto *pn = new QLabel(QStringLiteral("  Portas TCP em escuta nesta máquina. Duplo clique abre http://localhost:PORTA no navegador."));
        pn->setObjectName("sideHint");
        pv->addWidget(pn);
        panelTabs->addTab(portsPage, QStringLiteral("PORTAS"));

        // cantos: limpar / fechar
        auto *corner = new QWidget;
        auto *cl = new QHBoxLayout(corner);
        cl->setContentsMargins(0, 0, 6, 0);
        cl->setSpacing(2);
        auto *clr = flatBtn(QStringLiteral("Limpar"), QStringLiteral("Limpar a aba atual"));
        auto *cls = flatBtn(QStringLiteral("✕"), QStringLiteral("Fechar o painel (Ctrl+J)"));
        connect(clr, &QToolButton::clicked, this, [this] { clearPanel(); });
        connect(cls, &QToolButton::clicked, this, [this] { panelTabs->hide(); });
        cl->addWidget(clr); cl->addWidget(cls);
        panelTabs->setCornerWidget(corner, Qt::TopRightCorner);
        connect(panelTabs, &QTabWidget::currentChanged, this, [this](int) { onPanelTab(); });
        return panelTabs;
    }

    QMenu *gearMenu = nullptr;

    void buildStatusBar() {
        QStatusBar *sb = statusBar();
        sb->setSizeGripEnabled(false);
        auto mk = [this](const QString &text) {
            auto *b = new QToolButton;
            b->setText(text);
            b->setAutoRaise(true);
            b->setFocusPolicy(Qt::NoFocus);
            return b;
        };
        auto *logo = new QLabel;
        logo->setPixmap(logoIcon().pixmap(18, 18));
        logo->setContentsMargins(10, 0, 2, 0);
        sb->addWidget(logo);
        stBranch = mk(QStringLiteral("sem Git"));
        stBranch->setToolTip(QStringLiteral("Branch do Git (clique para abrir o painel Git)"));
        connect(stBranch, &QToolButton::clicked, this, [this] { showSidebar(2); });
        stErrors = mk(QStringLiteral("✕ 0"));
        stErrors->setToolTip(QStringLiteral("Erros (clique para abrir PROBLEMAS)"));
        stWarns = mk(QStringLiteral("⚠ 0"));
        stWarns->setToolTip(QStringLiteral("Avisos (clique para abrir PROBLEMAS)"));
        connect(stErrors, &QToolButton::clicked, this, [this] { showPanel(problemsPage); });
        connect(stWarns, &QToolButton::clicked, this, [this] { showPanel(problemsPage); });
        sb->addWidget(stBranch);
        sb->addWidget(stErrors);
        sb->addWidget(stWarns);
        msgLabel = new QLabel;
        msgLabel->setContentsMargins(12, 0, 0, 0);
        sb->addWidget(msgLabel, 1);
        msgTimer.setSingleShot(true);
        connect(&msgTimer, &QTimer::timeout, this, [this] { msgLabel->clear(); });

        stPos = mk(QStringLiteral("Ln 1, Col 1"));
        stPos->setToolTip(QStringLiteral("Ir para linha (Ctrl+G)"));
        connect(stPos, &QToolButton::clicked, this, [this] { gotoLineDialog(); });
        stIndent = mk(QStringLiteral("Espaços: 4"));
        stIndent->setToolTip(QStringLiteral("Indentação"));
        connect(stIndent, &QToolButton::clicked, this, [this] { indentMenu(); });
        stEnc = new QLabel(QStringLiteral("UTF-8"));
        stEol = mk(QStringLiteral("LF"));
        stEol->setToolTip(QStringLiteral("Fim de linha (clique para trocar)"));
        connect(stEol, &QToolButton::clicked, this, [this] { eolMenu(); });
        stLang = mk(QStringLiteral("Texto"));
        stLang->setToolTip(QStringLiteral("Linguagem (clique para trocar)"));
        connect(stLang, &QToolButton::clicked, this, [this] { langMenu(); });
        stPy = new QLabel;
        stBell = mk(QStringLiteral("🔔"));
        stBell->setText(QStringLiteral("Avisos"));
        stBell->setToolTip(QStringLiteral("Notificações recentes"));
        connect(stBell, &QToolButton::clicked, this, [this] { bellMenu(); });
        for (QWidget *w : {static_cast<QWidget *>(stPos), static_cast<QWidget *>(stIndent), static_cast<QWidget *>(stEnc), static_cast<QWidget *>(stEol),
                           static_cast<QWidget *>(stLang), static_cast<QWidget *>(stPy), static_cast<QWidget *>(stBell)})
            sb->addPermanentWidget(w);
    }

    QAction *act(QMenu *m, const QString &text, const QString &sc, std::function<void()> fn) {
        QAction *a = m->addAction(text);
        if (!sc.isEmpty()) a->setShortcut(QKeySequence(sc));
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        return a;
    }
    // acao sem atalho "de verdade" (o editor ja trata essas teclas); so mostra a dica
    QAction *hint(QMenu *m, const QString &text, const QString &keys, std::function<void()> fn) {
        QAction *a = m->addAction(text + QStringLiteral("\t") + keys);
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        return a;
    }

    void buildMenus() {
        // canto esquerdo da barra de menus: logo + nome
        auto *corner = new QWidget;
        corner->setObjectName("titleCorner");
        auto *cl = new QHBoxLayout(corner);
        cl->setContentsMargins(14, 0, 14, 0);
        cl->setSpacing(10);
        auto *lg = new QLabel;
        lg->setPixmap(logoIcon().pixmap(26, 26));
        auto *tt = new QLabel(QStringLiteral("Cisma Code"));
        tt->setObjectName("titleText");
        cl->addWidget(lg); cl->addWidget(tt);
        menuBar()->setCornerWidget(corner, Qt::TopLeftCorner);

        QMenu *f = menuBar()->addMenu(QStringLiteral("Arquivo"));
        act(f, QStringLiteral("Novo arquivo"), "Ctrl+N", [this] { newFile(); });
        act(f, QStringLiteral("Abrir arquivo…"), "Ctrl+O", [this] { chooseFile(); });
        act(f, QStringLiteral("Abrir pasta…"), "Ctrl+Shift+O", [this] { chooseFolder(); });
        recentMenu = f->addMenu(QStringLiteral("Abrir recente"));
        connect(recentMenu, &QMenu::aboutToShow, this, [this] { fillRecentMenu(); });
        f->addSeparator();
        act(f, QStringLiteral("Salvar"), "Ctrl+S", [this] { if (CodeEditor *e = current()) saveEditor(e, false); });
        act(f, QStringLiteral("Salvar como…"), "Ctrl+Shift+S", [this] { if (CodeEditor *e = current()) saveEditor(e, true); });
        act(f, QStringLiteral("Salvar tudo"), "Ctrl+Alt+S", [this] { for (CodeEditor *e : editors()) if (e->document()->isModified()) saveEditor(e, false); });
        f->addSeparator();
        act(f, QStringLiteral("Fechar aba"), "Ctrl+W", [this] { if (tabBar->currentIndex() >= 0) closeTab(tabBar->currentIndex()); });
        act(f, QStringLiteral("Fechar pasta"), "", [this] { closeFolder(); });
        f->addSeparator();
        act(f, QStringLiteral("Sair"), "Ctrl+Q", [this] { close(); });

        QMenu *ed = menuBar()->addMenu(QStringLiteral("Editar"));
        hint(ed, QStringLiteral("Desfazer"), QStringLiteral("Ctrl+Z"), [this] { if (auto *e = current()) e->undo(); });
        hint(ed, QStringLiteral("Refazer"), QStringLiteral("Ctrl+Shift+Z"), [this] { if (auto *e = current()) e->redo(); });
        ed->addSeparator();
        hint(ed, QStringLiteral("Recortar"), QStringLiteral("Ctrl+X"), [this] { if (auto *e = current()) e->cut(); });
        hint(ed, QStringLiteral("Copiar"), QStringLiteral("Ctrl+C"), [this] { if (auto *e = current()) e->copy(); });
        hint(ed, QStringLiteral("Colar"), QStringLiteral("Ctrl+V"), [this] { if (auto *e = current()) e->paste(); });
        ed->addSeparator();
        act(ed, QStringLiteral("Localizar"), "Ctrl+F", [this] { if (current()) findBar->openBar(false); });
        act(ed, QStringLiteral("Substituir"), "Ctrl+H", [this] { if (current()) findBar->openBar(true); });
        act(ed, QStringLiteral("Buscar nos arquivos"), "Ctrl+Shift+F", [this] { showSidebar(1); searchEdit->setFocus(); searchEdit->selectAll(); });
        ed->addSeparator();
        act(ed, QStringLiteral("Alternar comentário"), "Ctrl+/", [this] { if (auto *e = current()) e->toggleComment(); });
        act(ed, QStringLiteral("Aumentar indentação"), "Ctrl+]", [this] { if (auto *e = current()) e->indentSelection(true); });
        act(ed, QStringLiteral("Diminuir indentação"), "Ctrl+[", [this] { if (auto *e = current()) e->indentSelection(false); });

        QMenu *sel = menuBar()->addMenu(QStringLiteral("Seleção"));
        hint(sel, QStringLiteral("Selecionar tudo"), QStringLiteral("Ctrl+A"), [this] { if (auto *e = current()) e->selectAll(); });
        act(sel, QStringLiteral("Selecionar linha"), "Ctrl+L", [this] { if (auto *e = current()) e->numbersClick(e->cursorRect().center().y()); });
        sel->addSeparator();
        act(sel, QStringLiteral("Mover linha para cima"), "Alt+Up", [this] { if (auto *e = current()) e->moveLines(-1); });
        act(sel, QStringLiteral("Mover linha para baixo"), "Alt+Down", [this] { if (auto *e = current()) e->moveLines(1); });
        act(sel, QStringLiteral("Duplicar linha para baixo"), "Shift+Alt+Down", [this] { if (auto *e = current()) e->duplicateLines(true); });
        act(sel, QStringLiteral("Duplicar linha para cima"), "Shift+Alt+Up", [this] { if (auto *e = current()) e->duplicateLines(false); });
        act(sel, QStringLiteral("Excluir linha"), "Ctrl+Shift+K", [this] { if (auto *e = current()) e->deleteLines(); });
        sel->addSeparator();
        act(sel, QStringLiteral("Adicionar cursor acima"), "Ctrl+Alt+Up", [this] { if (auto *e = current()) e->addCursorVertical(-1); });
        act(sel, QStringLiteral("Adicionar cursor abaixo"), "Ctrl+Alt+Down", [this] { if (auto *e = current()) e->addCursorVertical(1); });
        act(sel, QStringLiteral("Adicionar próxima ocorrência"), "Ctrl+D", [this] { if (auto *e = current()) e->addNextOccurrence(); });
        act(sel, QStringLiteral("Selecionar todas as ocorrências"), "Ctrl+Shift+L", [this] { if (auto *e = current()) e->selectAllOccurrences(); });

        QMenu *v = menuBar()->addMenu(QStringLiteral("Ver"));
        act(v, QStringLiteral("Explorador"), "Ctrl+Shift+E", [this] { showSidebar(0); if (tree && explorerStack->currentIndex() == 1) tree->setFocus(); });
        act(v, QStringLiteral("Buscar"), "", [this] { showSidebar(1); });
        act(v, QStringLiteral("Git"), "Ctrl+Shift+G", [this] { showSidebar(2); });
        act(v, QStringLiteral("Executar"), "Ctrl+Shift+D", [this] { showSidebar(3); });
        act(v, QStringLiteral("Alternar barra lateral"), "Ctrl+B", [this] { toggleSidebar(sideStack->currentIndex()); });
        act(v, QStringLiteral("Alternar painel inferior"), "Ctrl+J", [this] { panelTabs->setVisible(!panelTabs->isVisible()); });
        act(v, QStringLiteral("Problemas"), "Ctrl+Shift+M", [this] { showPanel(problemsPage); });
        v->addSeparator();
        actWrap = act(v, QStringLiteral("Quebra de linha"), "Alt+Z", [this] { setWrap(!wrap); });
        actWrap->setCheckable(true);
        actWrap->setChecked(wrap);
        actMinimap = act(v, QStringLiteral("Minimapa"), "", [this] {
            minimapOn = !minimapOn;
            cfg.setValue("minimap", minimapOn);
            for (CodeEditor *e : editors()) e->setMinimapVisible(minimapOn);
            actMinimap->setChecked(minimapOn);
        });
        actMinimap->setCheckable(true);
        actMinimap->setChecked(minimapOn);
        act(v, QStringLiteral("Aumentar fonte"), "Ctrl+=", [this] { zoom(1); });
        act(v, QStringLiteral("Aumentar fonte "), "Ctrl++", [this] { zoom(1); })->setVisible(false);
        act(v, QStringLiteral("Diminuir fonte"), "Ctrl+-", [this] { zoom(-1); });
        act(v, QStringLiteral("Fonte padrão"), "Ctrl+0", [this] { setFontPx(14); });
        v->addSeparator();
        act(v, QStringLiteral("Tela cheia"), "F11", [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });

        QMenu *g = menuBar()->addMenu(QStringLiteral("Acessar"));
        act(g, QStringLiteral("Ir para arquivo…"), "Ctrl+P", [this] { quickOpen(); });
        act(g, QStringLiteral("Ir para linha…"), "Ctrl+G", [this] { gotoLineDialog(); });
        g->addSeparator();
        act(g, QStringLiteral("Próxima aba"), "Ctrl+PgDown", [this] { cycleTab(1); });
        act(g, QStringLiteral("Aba anterior"), "Ctrl+PgUp", [this] { cycleTab(-1); });

        QMenu *r = menuBar()->addMenu(QStringLiteral("Executar"));
        act(r, QStringLiteral("Executar arquivo"), "F5", [this] { runCurrent(); });
        act(r, QStringLiteral("Parar"), "Shift+F5", [this] { stopRun(false); });
        r->addSeparator();
        act(r, QStringLiteral("Depurar arquivo (Python)"), "F6", [this] { debugStartOrContinue(); });
        act(r, QStringLiteral("Continuar"), "F8", [this] { dbgResume(QStringLiteral("continue")); });
        act(r, QStringLiteral("Passo sobre"), "F10", [this] { dbgResume(QStringLiteral("next")); });
        act(r, QStringLiteral("Passo para dentro"), "F7", [this] { dbgResume(QStringLiteral("step")); });
        act(r, QStringLiteral("Passo para fora"), "Shift+F10", [this] { dbgResume(QStringLiteral("out")); });
        act(r, QStringLiteral("Parar depuração"), "Shift+F6", [this] { stopDebug(); });
        r->addSeparator();
        act(r, QStringLiteral("Alternar ponto de parada"), "F9", [this] { if (auto *e = current()) e->toggleBreakpoint(); });

        QMenu *t = menuBar()->addMenu(QStringLiteral("Terminal"));
        act(t, QStringLiteral("Abrir terminal"), "Ctrl+`", [this] { showPanel(term); term->focusTerminal(); });
        act(t, QStringLiteral("Limpar terminal"), "", [this] { term->clear(); });
        act(t, QStringLiteral("Reiniciar terminal"), "", [this] { showPanel(term); term->setWorkDir(root); term->restart(); });
        act(t, QStringLiteral("Console Python"), "", [this] { showPanel(dbg); dbg->focusTerminal(); });

        QMenu *hm = menuBar()->addMenu(QStringLiteral("Ajuda"));
        act(hm, QStringLiteral("Atalhos de teclado"), "", [this] { showShortcuts(); });
        act(hm, QStringLiteral("Sobre o Cisma Code"), "", [this] { showAbout(); });

        // menu da engrenagem
        act(gearMenu, QStringLiteral("Quebra de linha"), "", [this] { setWrap(!wrap); });
        act(gearMenu, QStringLiteral("Aumentar fonte"), "", [this] { zoom(1); });
        act(gearMenu, QStringLiteral("Diminuir fonte"), "", [this] { zoom(-1); });
        act(gearMenu, QStringLiteral("Fonte padrão"), "", [this] { setFontPx(14); });
        gearMenu->addSeparator();
        act(gearMenu, QStringLiteral("Atalhos de teclado"), "", [this] { showShortcuts(); });
        act(gearMenu, QStringLiteral("Sobre o Cisma Code"), "", [this] { showAbout(); });
    }
    QMenu *recentMenu = nullptr;

    // ======================================================================
    //  Barra lateral / painel
    // ======================================================================
    void showSidebar(int i) {
        sideStack->setCurrentIndex(i);
        sidebar->show();
        for (int k = 0; k < actButtons.size(); ++k) actButtons[k]->setChecked(k == i);
        if (i == 2) refreshGit();
        if (i == 3) updateRunPage();
    }
    void toggleSidebar(int i) {
        if (sidebar->isVisible() && sideStack->currentIndex() == i) {
            sidebar->hide();
            for (auto *b : actButtons) b->setChecked(false);
        } else showSidebar(i);
    }
    void showPanel(QWidget *w) {
        panelTabs->show();
        panelTabs->setCurrentWidget(w);
        const QList<int> sz = vsplit->sizes();
        if (sz.size() == 2 && sz[1] < 140) vsplit->setSizes({std::max(100, height() - 260), 220});
        onPanelTab();
    }
    void onPanelTab() {
        QWidget *w = panelTabs->currentWidget();
        const QString wd = root.isEmpty() ? QDir::homePath() : root;
        if (w == term) { term->setWorkDir(wd); term->start(); }
        else if (w == dbg) { dbg->setWorkDir(wd); dbg->start(); }
        else if (w == portsPage) refreshPorts();
    }
    void clearPanel() {
        QWidget *w = panelTabs->currentWidget();
        if (w == outPage) runOut->clear();
        else if (w == term) term->clear();
        else if (w == dbg) dbg->clear();
        else if (w == portsPage) refreshPorts();
    }
    void printOut(const QString &raw, const QColor &col = QColor()) {
        static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
        QString s = raw;
        s.remove(ansi);
        s.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        s.remove(QLatin1Char('\r'));
        if (s.isEmpty()) return;
        QScrollBar *sb = runOut->verticalScrollBar();
        const bool atEnd = sb->value() >= sb->maximum() - 4;
        QTextCursor c(runOut->document());
        c.movePosition(QTextCursor::End);
        QTextCharFormat f;
        f.setForeground(col.isValid() ? col : Pal::TXT);
        c.insertText(s, f);
        if (atEnd) sb->setValue(sb->maximum());
    }

    // processo curto assincrono (git, ss, python3 --version, verificacoes)
    void runProcess(const QString &prog, const QStringList &args, const QString &cwd, std::function<void(int, const QString &)> done,
                    const QByteArray &stdinData = QByteArray(), int timeoutMs = 20000) {
        auto *p = new QProcess(this);
        p->setProcessChannelMode(QProcess::MergedChannels);
        if (!cwd.isEmpty()) p->setWorkingDirectory(cwd);
        connect(p, &QProcess::started, this, [p, stdinData] {
            if (!stdinData.isEmpty()) p->write(stdinData);
            p->closeWriteChannel();
        });
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [p, done](int code, QProcess::ExitStatus) {
            done(code, QString::fromUtf8(p->readAll()));
            p->deleteLater();
        });
        connect(p, &QProcess::errorOccurred, this, [p, done](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) { done(-1, QString()); p->deleteLater(); }
        });
        QTimer::singleShot(timeoutMs, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
        p->start(prog, args);
    }

    // ======================================================================
    //  Arquivos, abas e editores
    // ======================================================================
    void chooseFolder() {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Abrir pasta"), root.isEmpty() ? QDir::homePath() : root);
        if (!d.isEmpty()) openFolder(d);
    }
    void chooseFile() {
        CodeEditor *c = current();
        const QString dir = c && !c->filePath.isEmpty() ? QFileInfo(c->filePath).absolutePath() : (root.isEmpty() ? QDir::homePath() : root);
        const QStringList fs = QFileDialog::getOpenFileNames(this, QStringLiteral("Abrir arquivo"), dir);
        for (const QString &f : fs) openFile(f);
    }
    void closeFolder() {
        root.clear();
        fsModel->setRootPath(QString());
        explorerStack->setCurrentIndex(0);
        searchResults->clear();
        refreshGit();
        updateTitle();
    }

    CodeEditor *newEditor() {
        auto *e = new CodeEditor;
        e->setObjectName("code");
        e->applyFont(monoFont(fontPx));
        e->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        e->onZoom = [this](int d) { zoom(d); };
        e->setMinimapVisible(minimapOn);
        e->onBreakpointsChanged = [this] { saveBps(); syncBreakpoints(); };
        e->onCursorsChanged = [this, e] { if (e == current()) updateStatus(); };
        connect(e->document(), &QTextDocument::modificationChanged, this, [this, e](bool) { updateTabLabel(e); updateTitle(); });
        connect(e, &QPlainTextEdit::cursorPositionChanged, this, [this, e] { if (e == current()) updateStatus(); });
        connect(e, &QPlainTextEdit::selectionChanged, this, [this, e] { if (e == current()) updateStatus(); });
        connect(e->document(), &QTextDocument::contentsChanged, this, [this, e] {
            if (e != current()) return;
            checkTimer.start();
            if (findBar->isVisible()) findTimer.start();
        });
        return e;
    }
    static QString displayName(CodeEditor *e) {
        if (e->filePath.isEmpty()) return QStringLiteral("Sem título-%1").arg(e->property("untitled").toInt());
        return QFileInfo(e->filePath).fileName();
    }
    void addEditorTab(CodeEditor *e) {
        const int idx = tabBar->count();
        edStack->addWidget(e);
        tabBar->insertTab(idx, fileTypeIcon(displayName(e)), displayName(e));
        auto *btn = new QToolButton;
        btn->setText(QStringLiteral("✕"));
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setToolTip(QStringLiteral("Fechar (Ctrl+W)"));
        connect(btn, &QToolButton::clicked, this, [this, e] { const int i = edStack->indexOf(e); if (i >= 0) closeTab(i); });
        tabBar->setTabButton(idx, QTabBar::RightSide, btn);
        tabBar->setTabToolTip(idx, e->filePath);
        tabBar->setCurrentIndex(idx);
        edStack->setCurrentWidget(e);
        updateWelcome();
        onCurrentChanged();
    }
    void updateTabLabel(CodeEditor *e) {
        const int i = edStack->indexOf(e);
        if (i < 0) return;
        tabBar->setTabText(i, displayName(e));
        tabBar->setTabIcon(i, fileTypeIcon(displayName(e)));
        tabBar->setTabToolTip(i, e->filePath);
        if (auto *b = qobject_cast<QToolButton *>(tabBar->tabButton(i, QTabBar::RightSide)))
            b->setText(e->document()->isModified() ? QStringLiteral("●") : QStringLiteral("✕"));
    }
    void detectIndent(CodeEditor *e, const QString &text) {
        int tabs = 0, spaces = 0, prev = 0, seen = 0;
        QHash<int, int> delta;
        const QStringList lines = text.left(200000).split(QLatin1Char('\n'));
        for (const QString &ln : lines) {
            if (++seen > 1500) break;
            if (ln.trimmed().isEmpty()) continue;
            if (ln.startsWith(QLatin1Char('\t'))) { ++tabs; continue; }
            int ind = 0;
            while (ind < ln.size() && ln[ind] == QLatin1Char(' ')) ++ind;
            if (ind > 0) ++spaces;
            if (ind > prev) { const int d = ind - prev; if (d == 2 || d == 3 || d == 4 || d == 8) delta[d]++; }
            prev = ind;
        }
        if (tabs > spaces) { e->useSpaces = false; return; }
        int best = 4, cnt = 0;
        for (auto it = delta.constBegin(); it != delta.constEnd(); ++it) if (it.value() > cnt) { cnt = it.value(); best = it.key(); }
        e->useSpaces = true;
        e->setTabSize(best);
    }
    void rememberRecent(const QString &key, const QString &path) {
        QStringList l = cfg.value(key).toStringList();
        l.removeAll(path);
        l.prepend(path);
        while (l.size() > 10) l.removeLast();
        cfg.setValue(key, l);
        if (welcome) welcome->setRecent(cfg.value("recent/files").toStringList());
    }
    void fillRecentMenu() {
        recentMenu->clear();
        const QStringList files = cfg.value("recent/files").toStringList(), dirs = cfg.value("recent/folders").toStringList();
        for (const QString &f : files) {
            QAction *a = recentMenu->addAction(fileTypeIcon(f), QStringLiteral("%1   —   %2").arg(QFileInfo(f).fileName(), QDir::toNativeSeparators(QFileInfo(f).absolutePath())));
            connect(a, &QAction::triggered, this, [this, f] { openFile(f); });
        }
        if (!files.isEmpty() && !dirs.isEmpty()) recentMenu->addSeparator();
        for (const QString &d : dirs) {
            QAction *a = recentMenu->addAction(folderIcon(), QStringLiteral("%1   —   %2").arg(QFileInfo(d).fileName(), QDir::toNativeSeparators(d)));
            connect(a, &QAction::triggered, this, [this, d] { openFolder(d); });
        }
        if (files.isEmpty() && dirs.isEmpty()) recentMenu->addAction(QStringLiteral("Nada recente"))->setEnabled(false);
        else {
            recentMenu->addSeparator();
            QAction *c = recentMenu->addAction(QStringLiteral("Limpar lista"));
            connect(c, &QAction::triggered, this, [this] { cfg.remove("recent/files"); cfg.remove("recent/folders"); welcome->setRecent({}); });
        }
    }

    bool maybeSave(CodeEditor *e) {
        if (!e->document()->isModified()) return true;
        tabBar->setCurrentIndex(edStack->indexOf(e));
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("Cisma Code"));
        box.setIcon(QMessageBox::Warning);
        box.setText(QStringLiteral("Salvar as alterações em “%1”?").arg(displayName(e)));
        box.setInformativeText(QStringLiteral("Se você não salvar, as alterações serão perdidas."));
        QPushButton *save = box.addButton(QStringLiteral("Salvar"), QMessageBox::AcceptRole);
        QPushButton *discard = box.addButton(QStringLiteral("Não salvar"), QMessageBox::DestructiveRole);
        box.addButton(QStringLiteral("Cancelar"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == save) return saveEditor(e, false);
        return box.clickedButton() == discard;
    }
    bool saveEditor(CodeEditor *e, bool saveAs) {
        QString path = e->filePath;
        if (path.isEmpty() || saveAs) {
            const QString base = !path.isEmpty() ? path : QDir(root.isEmpty() ? QDir::homePath() : root).filePath(displayName(e) == QLatin1String("Sem título-1") ? QStringLiteral("sem-titulo") : displayName(e));
            path = QFileDialog::getSaveFileName(this, QStringLiteral("Salvar como"), base);
            if (path.isEmpty()) return false;
        }
        QString err;
        if (!saveTextFile(path, e->toPlainText(), e->crlf, e->enc, err)) {
            QMessageBox::warning(this, QStringLiteral("Salvar"), QStringLiteral("Não consegui salvar %1:\n%2").arg(QFileInfo(path).fileName(), err));
            return false;
        }
        const bool changedPath = path != e->filePath;
        e->filePath = path;
        e->diskTime = QFileInfo(path).lastModified();
        e->document()->setModified(false);
        if (changedPath) { e->setLanguage(langForFile(path)); rememberRecent(QStringLiteral("recent/files"), path); }
        updateTabLabel(e);
        updateTitle();
        updateStatus();
        updateBreadcrumb();
        updateRunPage();
        runChecks(e);
        refreshGit();
        notify(QStringLiteral("Salvo: %1").arg(QFileInfo(path).fileName()));
        return true;
    }
    bool closeTab(int i) {
        if (i < 0 || i >= edStack->count()) return false;
        auto *e = static_cast<CodeEditor *>(edStack->widget(i));
        if (!maybeSave(e)) return false;
        removeEditor(e);
        return true;
    }
    void removeEditor(CodeEditor *e) {
        const int i = edStack->indexOf(e);
        if (i < 0) return;
        saveBps();
        problems.remove(e);
        edStack->removeWidget(e);
        tabBar->removeTab(i);
        e->deleteLater();
        updateProblemsView();
        updateWelcome();
        onCurrentChanged();
    }
    void tabMenu(const QPoint &pt) {
        const int i = tabBar->tabAt(pt);
        if (i < 0) return;
        auto *e = static_cast<CodeEditor *>(edStack->widget(i));
        QMenu m(this);
        QAction *close = m.addAction(QStringLiteral("Fechar"));
        QAction *others = m.addAction(QStringLiteral("Fechar as outras"));
        QAction *all = m.addAction(QStringLiteral("Fechar todas"));
        m.addSeparator();
        QAction *save = m.addAction(QStringLiteral("Salvar"));
        QAction *copy = m.addAction(QStringLiteral("Copiar caminho"));
        copy->setEnabled(!e->filePath.isEmpty());
        QAction *r = m.exec(tabBar->mapToGlobal(pt));
        if (!r) return;
        if (r == close) closeTab(edStack->indexOf(e));
        else if (r == others) { for (CodeEditor *o : editors()) if (o != e && !closeTab(edStack->indexOf(o))) break; }
        else if (r == all) { for (int k = edStack->count() - 1; k >= 0; --k) if (!closeTab(k)) break; }
        else if (r == save) saveEditor(e, false);
        else if (r == copy) QGuiApplication::clipboard()->setText(e->filePath);
    }

    void onCurrentChanged() {
        updateTitle();
        updateStatus();
        updateBreadcrumb();
        updateRunPage();
        if (findBar->isVisible()) findBar->refresh();
        if (current()) checkTimer.start();
    }
    void updateWelcome() {
        paneStack->setCurrentIndex(edStack->count() ? 1 : 0);
        welcome->setRecent(cfg.value("recent/files").toStringList());
    }
    void updateTitle() {
        CodeEditor *e = current();
        QString t;
        if (e) t = displayName(e) + (e->document()->isModified() ? QStringLiteral(" ●") : QString()) + QStringLiteral(" — ");
        if (!root.isEmpty()) t += QFileInfo(root).fileName() + QStringLiteral(" — ");
        setWindowTitle(t + QStringLiteral("Cisma Code"));
    }
    void updateBreadcrumb() {
        CodeEditor *e = current();
        breadcrumb->setVisible(e != nullptr);
        if (!e) return;
        if (e->filePath.isEmpty()) { breadcrumb->setText(displayName(e)); return; }
        QString p = e->filePath;
        if (!root.isEmpty() && p.startsWith(root + QLatin1Char('/'))) p = QFileInfo(root).fileName() + QLatin1Char('/') + QDir(root).relativeFilePath(p);
        else p = QDir::toNativeSeparators(p).replace(QDir::homePath(), QStringLiteral("~"));
        breadcrumb->setText(p.split(QLatin1Char('/'), Qt::SkipEmptyParts).join(QStringLiteral("  ›  ")));
    }
    void updateStatus() {
        CodeEditor *e = current();
        const bool has = e != nullptr;
        for (QWidget *w : {static_cast<QWidget *>(stPos), static_cast<QWidget *>(stIndent), static_cast<QWidget *>(stEnc), static_cast<QWidget *>(stEol),
                           static_cast<QWidget *>(stLang), static_cast<QWidget *>(stPy)})
            w->setVisible(has);
        if (!has) return;
        const QTextCursor c = e->textCursor();
        QString pos = QStringLiteral("Ln %1, Col %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1);
        if (c.hasSelection()) pos += QStringLiteral(" (%1 selecionados)").arg(c.selectionEnd() - c.selectionStart());
        if (e->cursorCount() > 1) pos = QStringLiteral("%1 cursores").arg(e->cursorCount());
        stPos->setText(pos);
        stIndent->setText(e->useSpaces ? QStringLiteral("Espaços: %1").arg(e->tabSize) : QStringLiteral("Tab: %1").arg(e->tabSize));
        stEnc->setText(e->enc);
        stEol->setText(e->crlf ? QStringLiteral("CRLF") : QStringLiteral("LF"));
        stLang->setText(e->lang->name);
        stPy->setText(e->lang->id == QLatin1String("python") ? pyVersion : QString());
        stPy->setVisible(!stPy->text().isEmpty());
    }
    void updateRunPage() {
        CodeEditor *e = current();
        runCmd->setEnabled(e != nullptr);
        if (!e) { runFileLbl->setText(QStringLiteral("Nenhum arquivo aberto")); runCmd->clear(); return; }
        runFileLbl->setText(QStringLiteral("%1  (%2)").arg(displayName(e), e->lang->name));
        runCmd->setText(cfg.value(QStringLiteral("run/") + e->lang->id, e->lang->run).toString());
    }
    void notify(const QString &t) {
        notifications.prepend(QTime::currentTime().toString(QStringLiteral("HH:mm")) + QStringLiteral("  ") + t);
        while (notifications.size() > 20) notifications.removeLast();
        msgLabel->setText(t);
        msgTimer.start(4000);
    }

    // ======================================================================
    //  Menus de contexto e dialogos
    // ======================================================================
    static QString shellQuote(const QString &s) {
        QString r = s;
        r.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QStringLiteral("'") + r + QStringLiteral("'");
    }
    void popupAbove(QMenu &m, QWidget *w) { m.exec(w->mapToGlobal(QPoint(0, -m.sizeHint().height()))); }
    void indentMenu() {
        CodeEditor *e = current();
        if (!e) return;
        QMenu m(this);
        for (int n : {2, 4, 8}) {
            QAction *a = m.addAction(QStringLiteral("Indentar com espaços: %1").arg(n));
            a->setCheckable(true); a->setChecked(e->useSpaces && e->tabSize == n);
            connect(a, &QAction::triggered, this, [this, e, n] { e->useSpaces = true; e->setTabSize(n); updateStatus(); });
        }
        m.addSeparator();
        for (int n : {2, 4, 8}) {
            QAction *a = m.addAction(QStringLiteral("Indentar com tabulação (largura %1)").arg(n));
            a->setCheckable(true); a->setChecked(!e->useSpaces && e->tabSize == n);
            connect(a, &QAction::triggered, this, [this, e, n] { e->useSpaces = false; e->setTabSize(n); updateStatus(); });
        }
        popupAbove(m, stIndent);
    }
    void eolMenu() {
        CodeEditor *e = current();
        if (!e) return;
        QMenu m(this);
        QAction *lf = m.addAction(QStringLiteral("LF (Linux, macOS)")), *crlf = m.addAction(QStringLiteral("CRLF (Windows)"));
        lf->setCheckable(true); crlf->setCheckable(true);
        lf->setChecked(!e->crlf); crlf->setChecked(e->crlf);
        connect(lf, &QAction::triggered, this, [this, e] { e->crlf = false; e->document()->setModified(true); updateStatus(); });
        connect(crlf, &QAction::triggered, this, [this, e] { e->crlf = true; e->document()->setModified(true); updateStatus(); });
        popupAbove(m, stEol);
    }
    void langMenu() {
        CodeEditor *e = current();
        if (!e) return;
        QMenu m(this);
        for (const Lang &l : allLangs()) {
            QAction *a = m.addAction(l.name);
            a->setCheckable(true); a->setChecked(e->lang == &l);
            connect(a, &QAction::triggered, this, [this, e, &l] { e->setLanguage(&l); updateStatus(); updateRunPage(); runChecks(e); });
        }
        popupAbove(m, stLang);
    }
    void bellMenu() {
        QMenu m(this);
        if (notifications.isEmpty()) m.addAction(QStringLiteral("Sem notificações"))->setEnabled(false);
        for (const QString &n : notifications) m.addAction(n)->setEnabled(false);
        if (!notifications.isEmpty()) {
            m.addSeparator();
            connect(m.addAction(QStringLiteral("Limpar notificações")), &QAction::triggered, this, [this] { notifications.clear(); });
        }
        popupAbove(m, stBell);
    }
    void gotoLineDialog() {
        CodeEditor *e = current();
        if (!e) return;
        bool ok = false;
        const int n = QInputDialog::getInt(this, QStringLiteral("Ir para linha"), QStringLiteral("Linha (1 a %1):").arg(e->blockCount()),
                                           e->textCursor().blockNumber() + 1, 1, e->blockCount(), 1, &ok);
        if (ok) { e->gotoLine(n); e->setFocus(); }
    }
    void quickOpen() {
        QStringList extra;
        for (CodeEditor *e : editors()) if (!e->filePath.isEmpty() && (root.isEmpty() || !e->filePath.startsWith(root + QLatin1Char('/')))) extra << e->filePath;
        QuickOpen dlg(this, root, extra);
        dlg.move(mapToGlobal(QPoint(std::max(0, width() / 2 - 310), 70)));
        if (dlg.exec() == QDialog::Accepted && !dlg.chosen.isEmpty()) openFile(dlg.chosen);
    }
    void cycleTab(int d) {
        const int n = tabBar->count();
        if (n < 2) return;
        tabBar->setCurrentIndex((tabBar->currentIndex() + d + n) % n);
    }
    void setWrap(bool on) {
        wrap = on;
        cfg.setValue("wrap", on);
        for (CodeEditor *e : editors()) e->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        if (actWrap) actWrap->setChecked(on);
    }
    void zoom(int d) { setFontPx(fontPx + d); }
    void setFontPx(int px) {
        px = std::max(8, std::min(40, px));
        fontPx = px;
        cfg.setValue("font/px", px);
        for (CodeEditor *e : editors()) e->applyFont(monoFont(px));
        const QFont m = monoFont(std::max(11, px - 1));
        term->setMono(m); dbg->setMono(m); runOut->setFont(m); runIn->setFont(m);
        notify(QStringLiteral("Fonte: %1 px").arg(px));
    }
    void showShortcuts() {
        const QString html = QStringLiteral(
            "<table cellpadding='3'>"
            "<tr><td><b>Ctrl+N / O / S</b></td><td>novo / abrir / salvar</td></tr>"
            "<tr><td><b>Ctrl+Shift+O</b></td><td>abrir pasta</td></tr>"
            "<tr><td><b>Ctrl+P</b></td><td>ir para arquivo</td></tr>"
            "<tr><td><b>Ctrl+G</b></td><td>ir para linha</td></tr>"
            "<tr><td><b>Ctrl+F / Ctrl+H</b></td><td>localizar / substituir</td></tr>"
            "<tr><td><b>Ctrl+Shift+F</b></td><td>buscar nos arquivos</td></tr>"
            "<tr><td><b>Ctrl+/</b></td><td>comentar linha</td></tr>"
            "<tr><td><b>Alt+Cima / Baixo</b></td><td>mover linha</td></tr>"
            "<tr><td><b>Shift+Alt+Cima / Baixo</b></td><td>duplicar linha</td></tr>"
            "<tr><td><b>Ctrl+Shift+K</b></td><td>excluir linha</td></tr>"
            "<tr><td><b>Ctrl+Espaço</b></td><td>autocompletar</td></tr>"
            "<tr><td><b>Tab / Shift+Tab</b></td><td>indentar / desindentar</td></tr>"
            "<tr><td><b>Ctrl+roda, Ctrl+= / -</b></td><td>zoom da fonte</td></tr>"
            "<tr><td><b>Ctrl+Alt+Cima / Baixo</b></td><td>adicionar cursor acima / abaixo</td></tr>"
            "<tr><td><b>Alt+clique</b></td><td>adicionar cursor</td></tr>"
            "<tr><td><b>Ctrl+D</b></td><td>próxima ocorrência (mais um cursor)</td></tr>"
            "<tr><td><b>Ctrl+Shift+L</b></td><td>selecionar todas as ocorrências</td></tr>"
            "<tr><td><b>Esc</b></td><td>voltar a um cursor só</td></tr>"
            "<tr><td><b>F5 / Shift+F5</b></td><td>executar / parar</td></tr>"
            "<tr><td><b>F6 / Shift+F6</b></td><td>depurar (Python) / parar a depuração</td></tr>"
            "<tr><td><b>F9</b></td><td>ponto de parada (ou clique ao lado do número da linha)</td></tr>"
            "<tr><td><b>F8 / F10 / F7 / Shift+F10</b></td><td>continuar / passo sobre / dentro / fora</td></tr>"
            "<tr><td><b>Ctrl+`</b></td><td>terminal</td></tr>"
            "<tr><td><b>Ctrl+J / Ctrl+B</b></td><td>painel inferior / barra lateral</td></tr>"
            "<tr><td><b>Ctrl+PgDown / PgUp</b></td><td>próxima / anterior aba</td></tr>"
            "<tr><td><b>Alt+Z</b></td><td>quebra de linha</td></tr></table>");
        QMessageBox::information(this, QStringLiteral("Atalhos de teclado"), html);
    }
    void showAbout() {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("Sobre o Cisma Code"));
        box.setIconPixmap(logoIcon().pixmap(72, 72));
        box.setText(QStringLiteral("<b>Cisma Code %1</b><br>Leve. Poderoso. Completo.").arg(QString::fromLatin1(APP_VERSION)));
        box.setInformativeText(QStringLiteral("Editor de código do Cisma OS.\nFeito com Qt %1.").arg(QString::fromLatin1(qVersion())));
        box.exec();
    }

    // ======================================================================
    //  Explorador: criar / renomear / lixeira
    // ======================================================================
    void createInDir(const QString &dir, bool isDir) {
        if (dir.isEmpty()) { notify(QStringLiteral("Abra uma pasta primeiro")); return; }
        bool ok = false;
        const QString name = QInputDialog::getText(this, isDir ? QStringLiteral("Nova pasta") : QStringLiteral("Novo arquivo"),
                                                   isDir ? QStringLiteral("Nome da pasta:") : QStringLiteral("Nome do arquivo (pode incluir subpastas):"),
                                                   QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        const QString path = QDir(dir).filePath(name);
        if (QFileInfo::exists(path)) { QMessageBox::warning(this, QStringLiteral("Criar"), QStringLiteral("Já existe: %1").arg(path)); return; }
        if (isDir) {
            if (!QDir().mkpath(path)) QMessageBox::warning(this, QStringLiteral("Criar"), QStringLiteral("Não consegui criar a pasta."));
            return;
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, QStringLiteral("Criar"), f.errorString()); return; }
        f.close();
        openFile(path);
    }
    void renamePath(const QString &path) {
        const QFileInfo fi(path);
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Renomear"), QStringLiteral("Novo nome:"), QLineEdit::Normal, fi.fileName(), &ok).trimmed();
        if (!ok || name.isEmpty() || name == fi.fileName()) return;
        const QString np = fi.dir().filePath(name);
        if (QFileInfo::exists(np)) { QMessageBox::warning(this, QStringLiteral("Renomear"), QStringLiteral("Já existe: %1").arg(name)); return; }
        if (!QDir().rename(path, np)) { QMessageBox::warning(this, QStringLiteral("Renomear"), QStringLiteral("Não consegui renomear.")); return; }
        for (CodeEditor *e : editors()) {
            if (e->filePath == path) e->filePath = np;
            else if (e->filePath.startsWith(path + QLatin1Char('/'))) e->filePath = np + e->filePath.mid(path.size());
            else continue;
            e->setLanguage(langForFile(e->filePath));
            updateTabLabel(e);
        }
        updateTitle(); updateBreadcrumb(); updateRunPage(); updateStatus();
    }
    void trashPath(const QString &path) {
        QList<CodeEditor *> affected;
        for (CodeEditor *e : editors())
            if (e->filePath == path || e->filePath.startsWith(path + QLatin1Char('/'))) affected << e;
        for (CodeEditor *e : affected)
            if (e->document()->isModified()) {
                QMessageBox::warning(this, QStringLiteral("Lixeira"), QStringLiteral("“%1” tem alterações não salvas. Salve ou feche a aba antes.").arg(displayName(e)));
                return;
            }
        if (QMessageBox::question(this, QStringLiteral("Mover para a lixeira"), QStringLiteral("Mover “%1” para a lixeira?").arg(QFileInfo(path).fileName())) != QMessageBox::Yes) return;
        if (!QFile::moveToTrash(path)) { QMessageBox::warning(this, QStringLiteral("Lixeira"), QStringLiteral("Não consegui mover para a lixeira.")); return; }
        for (CodeEditor *e : affected) removeEditor(e);
        refreshGit();
    }
    void treeMenu(const QPoint &pt) {
        const QModelIndex idx = tree->indexAt(pt);
        const QModelIndex s = idx.isValid() ? proxy->mapToSource(idx) : QModelIndex();
        const QString path = idx.isValid() ? fsModel->filePath(s) : root;
        const bool isDir = !idx.isValid() || fsModel->isDir(s);
        const QString dirForNew = isDir ? path : QFileInfo(path).absolutePath();
        QMenu m(this);
        QAction *open = idx.isValid() && !isDir ? m.addAction(QStringLiteral("Abrir")) : nullptr;
        QAction *nf = m.addAction(QStringLiteral("Novo arquivo…")), *nd = m.addAction(QStringLiteral("Nova pasta…"));
        QAction *ren = nullptr, *del = nullptr;
        if (idx.isValid()) { m.addSeparator(); ren = m.addAction(QStringLiteral("Renomear…")); del = m.addAction(QStringLiteral("Mover para a lixeira")); }
        m.addSeparator();
        QAction *cp = m.addAction(QStringLiteral("Copiar caminho")), *cr = m.addAction(QStringLiteral("Copiar caminho relativo"));
        QAction *ter = m.addAction(QStringLiteral("Abrir no terminal")), *fm = m.addAction(QStringLiteral("Mostrar no gerenciador de arquivos"));
        QAction *r = m.exec(tree->viewport()->mapToGlobal(pt));
        if (!r) return;
        if (r == open) openFile(path);
        else if (r == nf) createInDir(dirForNew, false);
        else if (r == nd) createInDir(dirForNew, true);
        else if (r == ren) renamePath(path);
        else if (r == del) trashPath(path);
        else if (r == cp) QGuiApplication::clipboard()->setText(path);
        else if (r == cr) QGuiApplication::clipboard()->setText(QDir(root).relativeFilePath(path));
        else if (r == ter) { showPanel(term); term->sendLine(QStringLiteral("cd ") + shellQuote(dirForNew)); term->focusTerminal(); }
        else if (r == fm) QDesktopServices::openUrl(QUrl::fromLocalFile(dirForNew));
    }

    // ======================================================================
    //  Buscar nos arquivos
    // ======================================================================
    void runSearch() {
        if (searchRunning) { searchCancel = true; return; }
        const QString q = searchEdit->text();
        if (q.isEmpty()) return;
        if (root.isEmpty()) { searchStatus->setText(QStringLiteral("Abra uma pasta para buscar.")); return; }
        QRegularExpression re(searchRegex->isChecked() ? q : QRegularExpression::escape(q),
                              searchCase->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) { searchStatus->setText(QStringLiteral("Expressão regular inválida: %1").arg(re.errorString())); return; }
        QList<QRegularExpression> inc;
        for (QString g : includeEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            g = g.trimmed();
            if (!g.isEmpty()) inc << QRegularExpression(QRegularExpression::wildcardToRegularExpression(g), QRegularExpression::CaseInsensitiveOption);
        }
        searchRunning = true;
        searchCancel = false;
        searchResults->clear();
        searchStatus->setText(QStringLiteral("Buscando… (Enter de novo para parar)"));
        int files = 0, hits = 0, scanned = 0;
        std::function<void(const QString &)> walk = [&](const QString &dir) {
            const QFileInfoList list = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
            for (const QFileInfo &fi : list) {
                if (searchCancel || hits >= 3000) return;
                if (fi.isSymLink()) continue;
                if (fi.isDir()) { if (!ignoredDirName(fi.fileName())) walk(fi.absoluteFilePath()); continue; }
                if (fi.size() > 2 * 1024 * 1024) continue;
                if (!inc.isEmpty()) {
                    bool ok = false;
                    for (const auto &r : inc) if (r.match(fi.fileName()).hasMatch()) { ok = true; break; }
                    if (!ok) continue;
                }
                if (++scanned % 40 == 0) QApplication::processEvents();
                if (searchCancel) return;
                QFile f(fi.absoluteFilePath());
                if (!f.open(QIODevice::ReadOnly)) continue;
                const QByteArray data = f.readAll();
                if (data.left(4096).contains('\0')) continue;
                const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
                QTreeWidgetItem *fileItem = nullptr;
                int fileHits = 0;
                for (int i = 0; i < lines.size() && fileHits < 200; ++i) {
                    QString ln = lines[i];
                    if (ln.endsWith(QLatin1Char('\r'))) ln.chop(1);
                    const auto m = re.match(ln);
                    if (!m.hasMatch()) continue;
                    if (!fileItem) {
                        fileItem = new QTreeWidgetItem(searchResults);
                        fileItem->setIcon(0, fileTypeIcon(fi.fileName()));
                        fileItem->setData(0, Qt::UserRole, fi.absoluteFilePath());
                        ++files;
                    }
                    auto *c = new QTreeWidgetItem(fileItem);
                    c->setText(0, QStringLiteral("%1:  %2").arg(i + 1).arg(ln.trimmed().left(160)));
                    c->setData(0, Qt::UserRole, fi.absoluteFilePath());
                    c->setData(0, Qt::UserRole + 1, i + 1);
                    c->setData(0, Qt::UserRole + 2, m.capturedStart() + 1);
                    ++fileHits; ++hits;
                }
                if (fileItem) {
                    fileItem->setText(0, QStringLiteral("%1   (%2)").arg(QDir(root).relativeFilePath(fi.absoluteFilePath())).arg(fileHits));
                    fileItem->setExpanded(true);
                }
            }
        };
        walk(root);
        searchRunning = false;
        QString st = hits ? QStringLiteral("%1 resultado(s) em %2 arquivo(s)").arg(hits).arg(files) : QStringLiteral("Nenhum resultado");
        if (hits >= 3000) st += QStringLiteral(" (limite de 3000 atingido)");
        if (searchCancel) st += QStringLiteral(" — busca interrompida");
        searchStatus->setText(st);
    }

    // ======================================================================
    //  Git
    // ======================================================================
    void setBranchStatus() {
        if (!stBranch) return;
        stBranch->setText(gitRepo ? QStringLiteral("⎇ %1%2").arg(gitBranch, gitDirty ? QStringLiteral("*") : QString()) : QStringLiteral("sem Git"));
    }
    void refreshGit() {
        if (!gitList) return;
        if (root.isEmpty() || !QStandardPaths::findExecutable(QStringLiteral("git")).size()) {
            gitRepo = false; gitDirty = false;
            gitList->clear();
            gitBranchLbl->clear();
            gitHint->setText(root.isEmpty() ? QStringLiteral("Abra uma pasta para usar o Git.") : QStringLiteral("O programa git não está instalado."));
            gitInitBtn->hide(); gitMsg->setEnabled(false); gitCommitBtn->setEnabled(false);
            setBranchStatus();
            return;
        }
        const QString r = root;
        runProcess(QStringLiteral("git"), {QStringLiteral("-C"), r, QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-b"), QStringLiteral("-uall")},
                   QString(), [this, r](int code, const QString &out) {
            if (r != root) return;
            gitList->clear();
            if (code != 0) {
                gitRepo = false; gitDirty = false;
                gitBranchLbl->clear();
                gitHint->setText(QStringLiteral("Esta pasta não é um repositório Git."));
                gitInitBtn->show(); gitMsg->setEnabled(false); gitCommitBtn->setEnabled(false);
                setBranchStatus();
                return;
            }
            gitRepo = true;
            gitInitBtn->hide(); gitMsg->setEnabled(true); gitCommitBtn->setEnabled(true);
            int changes = 0;
            for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (line.startsWith(QLatin1String("## "))) {
                    QString b = line.mid(3);
                    if (b.startsWith(QLatin1String("No commits yet on "))) b = b.mid(18);
                    else if (b.startsWith(QLatin1String("HEAD (no branch)"))) b = QStringLiteral("HEAD (destacado)");
                    const int d = b.indexOf(QLatin1String("..."));
                    if (d > 0) b = b.left(d);
                    const int sp = b.indexOf(QLatin1Char(' '));
                    if (sp > 0 && !b.startsWith(QLatin1String("HEAD"))) b = b.left(sp);
                    gitBranch = b;
                    continue;
                }
                if (line.size() < 4) continue;
                ++changes;
                if (gitList->count() >= 500) continue;
                const QString xy = line.left(2);
                QString p = line.mid(3);
                const int arrow = p.indexOf(QLatin1String(" -> "));
                if (arrow >= 0) p = p.mid(arrow + 4);
                if (p.startsWith(QLatin1Char('"')) && p.endsWith(QLatin1Char('"'))) p = p.mid(1, p.size() - 2);
                QString letter = xy.contains(QLatin1Char('?')) ? QStringLiteral("U") : xy.contains(QLatin1Char('D')) ? QStringLiteral("D")
                               : xy.contains(QLatin1Char('A')) ? QStringLiteral("A") : xy.contains(QLatin1Char('R')) ? QStringLiteral("R") : QStringLiteral("M");
                auto *it = new QListWidgetItem(fileTypeIcon(p), QStringLiteral("%1   %2").arg(letter, p));
                it->setForeground(letter == QLatin1String("D") ? Pal::KW : (letter == QLatin1String("U") || letter == QLatin1String("A")) ? Pal::GREEN
                                  : letter == QLatin1String("R") ? Pal::TYPE : Pal::FN);
                it->setData(Qt::UserRole, QDir(root).filePath(p));
                gitList->addItem(it);
            }
            gitDirty = changes > 0;
            gitBranchLbl->setText(QStringLiteral("⎇ %1").arg(gitBranch));
            gitHint->setText(changes ? QStringLiteral("%1 alteração(ões)").arg(changes) : QStringLiteral("Sem alterações."));
            setBranchStatus();
        });
    }
    void doCommit() {
        const QString msg = gitMsg->text().trimmed();
        if (root.isEmpty() || !gitRepo) return;
        if (msg.isEmpty()) { notify(QStringLiteral("Escreva a mensagem do commit")); gitMsg->setFocus(); return; }
        const QString r = root;
        runProcess(QStringLiteral("git"), {QStringLiteral("-C"), r, QStringLiteral("add"), QStringLiteral("-A")}, QString(), [this, r, msg](int c, const QString &o) {
            if (c != 0) { showPanel(outPage); printOut(QStringLiteral("$ git add -A\n") + o, Pal::KW); notify(QStringLiteral("git add falhou")); return; }
            runProcess(QStringLiteral("git"), {QStringLiteral("-C"), r, QStringLiteral("commit"), QStringLiteral("-m"), msg}, QString(), [this, msg](int c2, const QString &o2) {
                showPanel(outPage);
                printOut(QStringLiteral("$ git commit -m \"%1\"\n%2\n").arg(msg, o2), c2 == 0 ? Pal::GREEN : Pal::KW);
                if (c2 == 0) { gitMsg->clear(); notify(QStringLiteral("Commit criado")); } else notify(QStringLiteral("Commit falhou (veja a aba SAÍDA)"));
                refreshGit();
            });
        });
    }

    // ======================================================================
    //  Executar (F5)
    // ======================================================================
    void runCurrent(bool debug = false) {
        CodeEditor *e = current();
        if (!e) { notify(QStringLiteral("Abra um arquivo para executar")); return; }
        if (runProc && runProc->state() != QProcess::NotRunning) { notify(QStringLiteral("Já tem um programa rodando (Shift+F5 para parar)")); return; }
        if (e->filePath.isEmpty() || (runSave->isChecked() && e->document()->isModified())) { if (!saveEditor(e, false)) return; }
        if (e->filePath.isEmpty()) return;
        QString cmd = runCmd->text().trimmed();
        if (cmd.isEmpty()) cmd = e->lang->run;
        if (cmd.isEmpty()) {
            notify(QStringLiteral("Sem comando para %1: preencha na aba Executar").arg(e->lang->name));
            showSidebar(3); runCmd->setFocus();
            return;
        }
        cfg.setValue(QStringLiteral("run/") + e->lang->id, cmd);
        auto esc = [](QString s) {
            s.replace(QLatin1Char('\\'), QStringLiteral("\\\\")); s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
            s.replace(QLatin1Char('$'), QStringLiteral("\\$")); s.replace(QLatin1Char('`'), QStringLiteral("\\`"));
            return s;
        };
        const QFileInfo fi(e->filePath);
        cmd.replace(QLatin1String("%f"), esc(fi.absoluteFilePath()));
        cmd.replace(QLatin1String("%d"), esc(fi.absolutePath()));
        cmd.replace(QLatin1String("%b"), esc(fi.completeBaseName()));
        if (debug && !setupDebug(e, fi, cmd)) return;
        showPanel(outPage);
        runOut->clear();
        printOut(QStringLiteral("$ ") + cmd + QStringLiteral("\n"), Pal::COMMENT);
        runDec = QStringDecoder(QStringConverter::Utf8);
        runProc = new QProcess(this);
        runProc->setProcessChannelMode(QProcess::MergedChannels);
        runProc->setWorkingDirectory(fi.absolutePath());
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
        runProc->setProcessEnvironment(env);
        runProc->setChildProcessModifier([] { ::setpgid(0, 0); });
        connect(runProc, &QProcess::readyRead, this, [this] { if (runProc) printOut(runDec.decode(runProc->readAll())); });
        connect(runProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus st) {
            const double secs = runClock.elapsed() / 1000.0;
            const bool ok = code == 0 && st == QProcess::NormalExit;
            printOut(QStringLiteral("\n[%1 em %2 s, código %3]\n").arg(st == QProcess::CrashExit ? QStringLiteral("interrompido") : QStringLiteral("concluído")).arg(secs, 0, 'f', 1).arg(code),
                     ok ? Pal::GREEN : Pal::KW);
            runIn->hide();
            endDebug();
            if (runProc) { runProc->deleteLater(); runProc = nullptr; }
        });
        connect(runProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError er) {
            if (er == QProcess::FailedToStart) { printOut(QStringLiteral("[não consegui iniciar /bin/sh]\n"), Pal::KW); runIn->hide(); endDebug(); }
        });
        runClock.start();
        runProc->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
        runIn->show();
    }
    // ======================================================================
    //  Depurador (Python): bdb no programa, JSON por um socket local
    // ======================================================================
    static QString dbgReason(const QString &r) {
        if (r == QLatin1String("breakpoint")) return QStringLiteral("Parou em um ponto de parada");
        if (r == QLatin1String("entry")) return QStringLiteral("Parou na primeira linha");
        if (r == QLatin1String("step")) return QStringLiteral("Passo");
        return r;
    }
    bool setupDebug(CodeEditor *e, const QFileInfo &fi, QString &cmd) {
        if (e->lang->id != QLatin1String("python")) {
            notify(QStringLiteral("O depurador funciona com Python (este arquivo é %1)").arg(e->lang->name));
            return false;
        }
        endDebug();
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/debugger");
        QDir().mkpath(dir);
        QFile f(dir + QStringLiteral("/cisma_dbg.py"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(kDbgAdapter) < 0) { notify(QStringLiteral("Não consegui gravar o adaptador do depurador")); return false; }
        f.close();
        dbgServer = new QLocalServer(this);
        const QString sock = QDir::temp().filePath(QStringLiteral("cisma-code-dbg-%1.sock").arg(::getpid()));
        QLocalServer::removeServer(sock);
        if (!dbgServer->listen(sock)) { notify(QStringLiteral("Não consegui abrir o canal do depurador")); endDebug(); return false; }
        connect(dbgServer, &QLocalServer::newConnection, this, [this] { onDbgConnection(); });
        QString py = cmd.section(QLatin1Char(' '), 0, 0);
        if (!py.contains(QLatin1String("python"))) py = QStringLiteral("python3");
        QString d = dir;
        d.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        const QString boot = QStringLiteral("import sys;sys.path.insert(0,\"%1\");import cisma_dbg;sys.path.pop(0);cisma_dbg.main()").arg(d);
        cmd = py + QStringLiteral(" -u -c ") + shellQuote(boot) + QLatin1Char(' ') + shellQuote(sock) + QLatin1Char(' ') + shellQuote(fi.absoluteFilePath());
        dbgOn = true;
        dpLog->clear();
        dpStack->clear();
        dpVars->clear();
        updateDbgUi();
        return true;
    }
    void endDebug() {
        dbgPaused = false;
        if (dbgSock) { dbgSock->disconnect(this); dbgSock->abort(); dbgSock->deleteLater(); dbgSock = nullptr; }
        if (dbgServer) { dbgServer->close(); dbgServer->deleteLater(); dbgServer = nullptr; }
        dbgBuf.clear();
        const bool was = dbgOn;
        dbgOn = false;
        clearDebugMarks();
        updateDbgUi();
        if (was && dpStack) { dpStack->clear(); dpVars->clear(); }
    }
    void stopDebug() {
        if (!dbgOn) { notify(QStringLiteral("Não há depuração em andamento")); return; }
        dbgSend({{QStringLiteral("cmd"), QStringLiteral("stop")}});
        stopRun(true);
        endDebug();
    }
    void debugStartOrContinue() {
        if (dbgOn && dbgPaused) { dbgResume(QStringLiteral("continue")); return; }
        if (dbgOn) { notify(QStringLiteral("O programa está rodando (Shift+F6 para parar)")); return; }
        runCurrent(true);
    }
    void onDbgConnection() {
        if (!dbgServer) return;
        dbgSock = dbgServer->nextPendingConnection();
        if (!dbgSock) return;
        connect(dbgSock, &QLocalSocket::readyRead, this, [this] {
            if (!dbgSock) return;
            dbgBuf += dbgSock->readAll();
            int nl;
            while ((nl = dbgBuf.indexOf('\n')) >= 0) {
                const QByteArray line = dbgBuf.left(nl);
                dbgBuf.remove(0, nl + 1);
                handleDbg(QJsonDocument::fromJson(line).object());
                if (!dbgSock) return;
            }
        });
        connect(dbgSock, &QLocalSocket::disconnected, this, [this] { if (!dbgOn) return; dbgPaused = false; clearDebugMarks(); updateDbgUi(); });
    }
    QJsonObject breakpointsJson() {
        saveBps();
        QJsonObject o;
        for (auto it = bpStore.cbegin(); it != bpStore.cend(); ++it) {
            QJsonArray a;
            for (int l : it.value()) a.append(l);
            o.insert(it.key(), a);
        }
        return o;
    }
    void dbgSend(const QJsonObject &o) {
        if (!dbgSock || dbgSock->state() != QLocalSocket::ConnectedState) return;
        dbgSock->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
        dbgSock->flush();
    }
    void syncBreakpoints() { if (dbgOn && dbgSock) dbgSend({{QStringLiteral("cmd"), QStringLiteral("setbp")}, {QStringLiteral("bps"), breakpointsJson()}}); }
    void dbgResume(const QString &c) {
        if (!dbgOn || !dbgPaused) { if (!dbgOn) notify(QStringLiteral("Sem depuração em andamento (F6 inicia)")); return; }
        dbgPaused = false;
        clearDebugMarks();
        dbgSend({{QStringLiteral("cmd"), c}});
        updateDbgUi();
    }
    void clearDebugMarks() { for (CodeEditor *e : editors()) if (e->debugLine > 0) e->setDebugLine(-1); }
    void updateDbgUi() {
        if (!dpState) return;
        for (int i = 0; i < dpBtns.size(); ++i) dpBtns[i]->setEnabled(i == 4 ? dbgOn : (dbgOn && dbgPaused));
        if (!dbgOn) dpState->setText(QStringLiteral("Sem depuração. F6 inicia (Python); F9 põe um ponto de parada."));
        else if (!dbgPaused) dpState->setText(QStringLiteral("Executando…"));
    }
    void dbgFillVars(const QJsonArray &loc, const QJsonArray &glob) {
        dpVars->clear();
        auto group = [&](const QString &name, const QJsonArray &arr, bool open) {
            if (arr.isEmpty()) return;
            auto *top = new QTreeWidgetItem(dpVars, {QStringLiteral("%1 (%2)").arg(name).arg(arr.size())});
            QFont f = top->font(0);
            f.setBold(true);
            top->setFont(0, f);
            for (const QJsonValue &v : arr) {
                const QJsonArray r = v.toArray();
                auto *it = new QTreeWidgetItem(top, {r.at(0).toString(), r.at(1).toString(), r.at(2).toString()});
                it->setToolTip(2, r.at(2).toString());
            }
            top->setExpanded(open);
        };
        group(QStringLiteral("Locais"), loc, true);
        group(QStringLiteral("Globais"), glob, loc.isEmpty());
    }
    void dbgShowLine(const QString &file, int line, bool top) {
        clearDebugMarks();
        if (!openFile(file, line, 1)) return;
        for (CodeEditor *e : editors())
            if (!e->filePath.isEmpty() && QFileInfo(e->filePath).canonicalFilePath() == QFileInfo(file).canonicalFilePath()) { e->setDebugLine(line); break; }
        Q_UNUSED(top);
    }
    void dbgSelectFrame(int r) {
        if (!dbgPaused || r < 0 || r >= dpFrameFiles.size()) return;
        dbgShowLine(dpFrameFiles[r], dpFrameLines[r], r == 0);
        dbgSend({{QStringLiteral("cmd"), QStringLiteral("vars")}, {QStringLiteral("frame"), r}});
    }
    void dbgEval() {
        const QString ex = dpEval->text().trimmed();
        if (ex.isEmpty()) return;
        dpLog->appendPlainText(QStringLiteral("> ") + ex);
        if (!dbgOn || !dbgPaused) { dpLog->appendPlainText(QStringLiteral("(só dá para avaliar com o programa parado)")); return; }
        dbgSend({{QStringLiteral("cmd"), QStringLiteral("eval")}, {QStringLiteral("expr"), ex}, {QStringLiteral("frame"), std::max(0, dpStack->currentRow())}});
        dpEval->clear();
    }
    void handleDbg(const QJsonObject &m) {
        const QString ev = m.value(QStringLiteral("ev")).toString();
        if (ev == QLatin1String("hello")) {
            const QJsonObject bps = breakpointsJson();
            dbgSend({{QStringLiteral("cmd"), QStringLiteral("go")}, {QStringLiteral("bps"), bps}, {QStringLiteral("entry"), bps.isEmpty()}});
        } else if (ev == QLatin1String("stopped")) {
            dbgPaused = true;
            const QJsonArray frames = m.value(QStringLiteral("frames")).toArray();
            dpFrameFiles.clear();
            dpFrameLines.clear();
            dpStack->blockSignals(true);
            dpStack->clear();
            for (const QJsonValue &fv : frames) {
                const QJsonArray f = fv.toArray();
                dpFrameFiles << f.at(0).toString();
                dpFrameLines << f.at(1).toInt();
                dpStack->addItem(QStringLiteral("%1   %2:%3").arg(f.at(2).toString(), QFileInfo(f.at(0).toString()).fileName()).arg(f.at(1).toInt()));
            }
            if (dpStack->count()) dpStack->setCurrentRow(0);
            dpStack->blockSignals(false);
            dbgFillVars(m.value(QStringLiteral("locals")).toArray(), m.value(QStringLiteral("globals")).toArray());
            dpState->setText(dbgReason(m.value(QStringLiteral("reason")).toString()));
            updateDbgUi();
            showPanel(dpPage);
            if (!dpFrameFiles.isEmpty()) dbgShowLine(dpFrameFiles[0], dpFrameLines[0], true);
            raise();
            activateWindow();
        } else if (ev == QLatin1String("vars")) {
            dbgFillVars(m.value(QStringLiteral("locals")).toArray(), m.value(QStringLiteral("globals")).toArray());
        } else if (ev == QLatin1String("evalres")) {
            dpLog->appendPlainText(m.value(QStringLiteral("out")).toString());
        }
    }
    void saveBps() {
        for (CodeEditor *e : editors()) {
            if (e->filePath.isEmpty()) continue;
            const QList<int> l = e->breakpoints();
            if (l.isEmpty()) bpStore.remove(e->filePath); else bpStore[e->filePath] = l;
        }
        QJsonObject o;
        for (auto it = bpStore.cbegin(); it != bpStore.cend(); ++it) {
            QJsonArray a;
            for (int l : it.value()) a.append(l);
            o.insert(it.key(), a);
        }
        cfg.setValue(QStringLiteral("debug/breakpoints"), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    }
    void loadBps() {
        const QJsonObject o = QJsonDocument::fromJson(cfg.value(QStringLiteral("debug/breakpoints")).toString().toUtf8()).object();
        for (auto it = o.begin(); it != o.end(); ++it) {
            QList<int> l;
            for (const QJsonValue &v : it.value().toArray()) l << v.toInt();
            if (!l.isEmpty()) bpStore[it.key()] = l;
        }
    }

    void stopRun(bool hard) {
        if (!runProc || runProc->state() == QProcess::NotRunning) { if (!hard) notify(QStringLiteral("Nada rodando")); return; }
        const pid_t pid = pid_t(runProc->processId());
        if (pid <= 0) return;
        ::kill(-pid, hard ? SIGKILL : SIGTERM);
        if (!hard) QTimer::singleShot(1500, this, [pid] { ::kill(-pid, SIGKILL); });
    }
    void openFromLine(const QString &line) {
        QString f; int ln = 1, col = 1;
        if (!parseLocation(line, f, ln, col)) return;
        QStringList bases;
        if (CodeEditor *e = current()) if (!e->filePath.isEmpty()) bases << QFileInfo(e->filePath).absolutePath();
        if (!root.isEmpty()) bases << root;
        bases << QDir::currentPath();
        QString found;
        if (QFileInfo(f).isAbsolute()) { if (QFileInfo::exists(f)) found = f; }
        else for (const QString &b : bases) if (QFileInfo::exists(QDir(b).filePath(f))) { found = QDir(b).filePath(f); break; }
        if (found.isEmpty()) { notify(QStringLiteral("Arquivo não encontrado: %1").arg(f)); return; }
        openFile(found, ln, col);
    }

    // ======================================================================
    //  Problemas (Python e JSON) e portas
    // ======================================================================
    void runChecks(CodeEditor *e) {
        if (!e) return;
        const int gen = ++checkGen;
        e->setProperty("chk", gen);
        QPointer<CodeEditor> pe(e);
        const QString text = e->toPlainText();
        auto store = [this, pe, gen](const QList<Problem> &l) {
            if (!pe || pe->property("chk").toInt() != gen) return;
            problems[pe.data()] = l;
            updateProblemsView();
        };
        if (e->lang->id == QLatin1String("python")) {
            static const QByteArray script =
                "import ast,sys\nsrc=sys.stdin.read()\ntry:\n    ast.parse(src)\nexcept SyntaxError as e:\n    print('E|%s|%s|%s' % (e.lineno or 1, e.offset or 1, e.msg))\n";
            runProcess(QStringLiteral("python3"), {QStringLiteral("-I"), QStringLiteral("-c"), QString::fromLatin1(script)}, QString(),
                       [this, store, text, pe, gen](int code, const QString &out) {
                           if (!pe || pe->property("chk").toInt() != gen) return;
                           QList<Problem> l;
                           for (const QString &ln : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                               if (!ln.startsWith(QLatin1String("E|"))) continue;
                               const QStringList p = ln.split(QLatin1Char('|'));
                               if (p.size() >= 4) l.append({p[1].toInt(), p[2].toInt(), p.mid(3).join(QStringLiteral("|")), true});
                           }
                           if (code < 0 || !l.isEmpty()) { store(l); return; }
                           runProcess(QStringLiteral("python3"), {QStringLiteral("-m"), QStringLiteral("pyflakes")}, QString(), [store, pe, gen](int, const QString &o2) {
                               if (!pe || pe->property("chk").toInt() != gen) return;
                               static const QRegularExpression re(QStringLiteral("^<stdin>:(\\d+):(?:(\\d+):?)? (.*)$"));
                               QList<Problem> w;
                               for (const QString &ln : o2.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                                   const auto m = re.match(ln);
                                   if (m.hasMatch()) w.append({m.captured(1).toInt(), m.captured(2).isEmpty() ? 1 : m.captured(2).toInt(), m.captured(3), false});
                               }
                               store(w);
                           }, text.toUtf8());
                       }, text.toUtf8());
        } else if (e->lang->id == QLatin1String("json") && !e->filePath.endsWith(QLatin1String(".jsonc"))) {
            QJsonParseError err;
            const QByteArray bytes = text.toUtf8();
            QList<Problem> l;
            if (!bytes.trimmed().isEmpty()) {
                QJsonDocument::fromJson(bytes, &err);
                if (err.error != QJsonParseError::NoError) {
                    const QByteArray head = bytes.left(std::max(0, int(err.offset)));
                    l.append({int(head.count('\n')) + 1, int(head.size() - head.lastIndexOf('\n')), err.errorString(), true});
                }
            }
            store(l);
        } else store({});
    }
    void updateProblemsView() {
        problemsView->clear();
        int errs = 0, warns = 0;
        for (CodeEditor *e : editors()) {
            const QList<Problem> l = problems.value(e);
            for (const Problem &p : l) {
                (p.error ? errs : warns)++;
                auto *it = new QTreeWidgetItem(problemsView);
                it->setText(0, p.error ? QStringLiteral("Erro") : QStringLiteral("Aviso"));
                it->setForeground(0, p.error ? Pal::KW : Pal::FN);
                it->setText(1, p.msg);
                it->setText(2, displayName(e));
                it->setText(3, QString::number(p.line));
                it->setData(0, Qt::UserRole + 1, p.line);
                it->setData(0, Qt::UserRole + 2, p.col);
                it->setData(0, Qt::UserRole + 3, QVariant::fromValue<quintptr>(quintptr(e)));
            }
        }
        panelTabs->setTabText(panelTabs->indexOf(problemsPage), errs + warns ? QStringLiteral("PROBLEMAS (%1)").arg(errs + warns) : QStringLiteral("PROBLEMAS"));
        stErrors->setText(QStringLiteral("✕ %1").arg(errs));
        stWarns->setText(QStringLiteral("⚠ %1").arg(warns));
    }
    void refreshPorts() {
        runProcess(QStringLiteral("ss"), {QStringLiteral("-ltnpH")}, QString(), [this](int code, const QString &out) {
            struct Row { int port; QString addr, proc, pid; };
            QList<Row> rows;
            QSet<QString> seen;
            static const QRegularExpression pr(QStringLiteral("users:\\(\\(\"([^\"]+)\",pid=(\\d+)"));
            if (code == 0) {
                for (const QString &ln : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                    const QStringList p = ln.simplified().split(QLatin1Char(' '));
                    if (p.size() < 5) continue;
                    const QString local = p[3];
                    const int c = local.lastIndexOf(QLatin1Char(':'));
                    if (c < 0) continue;
                    const auto m = pr.match(ln);
                    Row r{local.mid(c + 1).toInt(), local.left(c), m.hasMatch() ? m.captured(1) : QString(), m.hasMatch() ? m.captured(2) : QString()};
                    const QString key = QString::number(r.port) + r.proc + r.addr;
                    if (seen.contains(key)) continue;
                    seen.insert(key);
                    rows.append(r);
                }
            }
            std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.port < b.port; });
            portsTable->setRowCount(int(rows.size()));
            for (int i = 0; i < rows.size(); ++i) {
                portsTable->setItem(i, 0, new QTableWidgetItem(QString::number(rows[i].port)));
                portsTable->setItem(i, 1, new QTableWidgetItem(rows[i].addr));
                portsTable->setItem(i, 2, new QTableWidgetItem(rows[i].proc.isEmpty() ? QStringLiteral("—") : rows[i].proc));
                portsTable->setItem(i, 3, new QTableWidgetItem(rows[i].pid));
            }
            portsTable->resizeColumnsToContents();
            portsTable->horizontalHeader()->setStretchLastSection(true);
        });
    }

    // ======================================================================
    //  Mudancas feitas fora do editor
    // ======================================================================
    void reloadEditor(CodeEditor *e) {
        const LoadResult r = loadTextFile(e->filePath);
        if (!r.ok) return;
        const int pos = e->textCursor().position(), sb = e->verticalScrollBar()->value();
        e->setPlainText(r.text);
        e->crlf = r.crlf;
        e->enc = r.enc;
        e->diskTime = QFileInfo(e->filePath).lastModified();
        e->document()->setModified(false);
        QTextCursor c(e->document());
        c.setPosition(std::min(pos, std::max(0, e->document()->characterCount() - 1)));
        e->setTextCursor(c);
        e->verticalScrollBar()->setValue(sb);
        updateStatus();
        runChecks(e);
    }
    void checkExternalChanges() {
        if (inExtCheck) return;
        inExtCheck = true;
        for (CodeEditor *e : editors()) {
            if (e->filePath.isEmpty()) continue;
            const QFileInfo fi(e->filePath);
            if (!fi.exists() || fi.lastModified() == e->diskTime) continue;
            if (!e->document()->isModified()) { reloadEditor(e); notify(QStringLiteral("%1 mudou no disco e foi recarregado").arg(fi.fileName())); continue; }
            tabBar->setCurrentIndex(edStack->indexOf(e));
            const auto a = QMessageBox::question(this, QStringLiteral("Arquivo alterado"),
                                                 QStringLiteral("“%1” mudou no disco, mas você tem alterações não salvas aqui.\nRecarregar do disco (perde as suas alterações)?").arg(fi.fileName()),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (a == QMessageBox::Yes) reloadEditor(e); else e->diskTime = fi.lastModified();
        }
        inExtCheck = false;
    }

    // ======================================================================
    //  Instancia unica
    // ======================================================================
    void startServer() {
        const QString name = QStringLiteral("cisma-code-%1").arg(getuid());
        server = new QLocalServer(this);
        server->setSocketOptions(QLocalServer::UserAccessOption);
        QLocalServer::removeServer(name);
        if (!server->listen(name)) return;
        connect(server, &QLocalServer::newConnection, this, [this] {
            QLocalSocket *s = server->nextPendingConnection();
            if (!s) return;
            connect(s, &QLocalSocket::readyRead, this, [this, s] { handleRemote(QString::fromUtf8(s->readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)); });
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        });
    }

    bool eventFilter(QObject *o, QEvent *ev) override {
        if (runOut && o == runOut->viewport() && ev->type() == QEvent::MouseButtonDblClick) {
            auto *m = static_cast<QMouseEvent *>(ev);
            openFromLine(runOut->cursorForPosition(m->position().toPoint()).block().text());
        }
        return QMainWindow::eventFilter(o, ev);
    }
};

// ============================================================================
//  Autoteste (sem tela):  QT_QPA_PLATFORM=offscreen cisma-code --selftest
// ============================================================================
static int selfTest() {
    int ran = 0, bad = 0;
    auto check = [&](bool ok, const char *what) {
        ++ran;
        if (!ok) { ++bad; std::fprintf(stderr, "  FALHOU: %s\n", what); }
    };
    auto type = [](CodeEditor &e, const QString &s) {
        for (QChar ch : s) {
            const bool nl = ch == QLatin1Char('\n');
            QKeyEvent ev(QEvent::KeyPress, nl ? Qt::Key_Return : 0, Qt::NoModifier, nl ? QStringLiteral("\r") : QString(ch));
            QApplication::sendEvent(&e, &ev);
        }
    };
    auto hasFmt = [](CodeEditor &e, int block, int start, int len, const QColor &c) {
        for (const auto &r : e.document()->findBlockByNumber(block).layout()->formats())
            if (r.start == start && r.length == len && r.format.foreground().color() == c) return true;
        return false;
    };

    check(langForFile("a/b/main.py")->id == "python", "linguagem .py");
    check(langForFile("Makefile")->id == "make", "linguagem Makefile");
    check(langForFile("x.TSX")->id == "typescript", "linguagem .tsx");
    check(langForFile("arquivo.xyz")->id == "plain", "linguagem desconhecida = texto");

    {   // destaque de sintaxe
        CodeEditor e;
        e.setLanguage(langById("python"));
        e.setPlainText("def foo(x):\n    return 'a'  # c\n");
        check(hasFmt(e, 0, 0, 3, Pal::KW), "def em vermelho (palavra-chave)");
        check(hasFmt(e, 0, 4, 3, Pal::FN), "nome da funcao depois do def");
        check(hasFmt(e, 1, 11, 3, Pal::STR), "string 'a'");
        check(hasFmt(e, 1, 16, 3, Pal::COMMENT), "comentario");
        e.setLanguage(langById("python"));
        e.setPlainText("x = \"\"\"abc\ndef\"\"\"\ny = 1\n");
        check(hasFmt(e, 1, 0, 6, Pal::STR), "string de 3 aspas em varias linhas");
        check(hasFmt(e, 2, 4, 1, Pal::NUM), "numero depois da string de varias linhas");
    }
    {   // auto-indentar, pares e chaves
        CodeEditor e;
        e.setLanguage(langById("python"));
        type(e, "if x:\n");
        check(e.toPlainText() == "if x:\n    ", "indenta depois de dois-pontos");
        type(e, "y\n");
        check(e.toPlainText() == "if x:\n    y\n    ", "mantem a indentacao");
        e.setPlainText("");
        type(e, "f(");
        check(e.toPlainText() == "f()" && e.textCursor().position() == 2, "( fecha com )");
        type(e, ")");
        check(e.toPlainText() == "f()" && e.textCursor().position() == 3, ") anda por cima do fechamento");
        e.setPlainText("");
        type(e, "s = \"");
        check(e.toPlainText() == "s = \"\"", "aspas fecham sozinhas");
        e.setPlainText("");
        type(e, "don't");
        check(e.toPlainText() == "don't", "apostrofo no meio da palavra nao fecha");
        CodeEditor c;
        c.setLanguage(langById("cpp"));
        type(c, "{\n");
        check(c.toPlainText() == "{\n    \n}" && c.textCursor().blockNumber() == 1, "Enter entre chaves abre o bloco");
        c.setPlainText("");
        c.useSpaces = false;
        type(c, "a\n\tb");
        QKeyEvent bs(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QApplication::sendEvent(&c, &bs);
        check(c.toPlainText() == "a\n\t", "backspace normal");
    }
    {   // linhas
        CodeEditor e;
        e.setLanguage(langById("python"));
        e.setPlainText("a\nb");
        e.selectAll();
        e.toggleComment();
        check(e.toPlainText() == "# a\n# b", "comentar linhas");
        e.selectAll();
        e.toggleComment();
        check(e.toPlainText() == "a\nb", "descomentar linhas");
        e.setPlainText("1\n2\n3");
        e.gotoLine(2);
        e.moveLines(-1);
        check(e.toPlainText() == "2\n1\n3", "mover linha para cima");
        e.moveLines(1);
        check(e.toPlainText() == "1\n2\n3", "mover linha para baixo");
        e.gotoLine(3);
        e.moveLines(1);
        check(e.toPlainText() == "1\n2\n3", "nao move alem da ultima linha");
        e.setPlainText("a\nb");
        e.gotoLine(1);
        e.duplicateLines(true);
        check(e.toPlainText() == "a\na\nb", "duplicar linha");
        e.deleteLines();
        check(e.toPlainText() == "a\nb", "excluir linha");
        e.gotoLine(2);
        e.deleteLines();
        check(e.toPlainText() == "a", "excluir a ultima linha");
        e.setPlainText("x\ny");
        e.selectAll();
        e.indentSelection(true);
        check(e.toPlainText() == "    x\n    y", "indentar selecao");
        e.selectAll();
        e.indentSelection(false);
        check(e.toPlainText() == "x\ny", "desindentar selecao");
        e.setLanguage(langById("html"));
        e.setPlainText("<p>oi</p>");
        e.toggleComment();
        check(e.toPlainText() == "<!-- <p>oi</p> -->", "comentar HTML");
        e.toggleComment();
        check(e.toPlainText() == "<p>oi</p>", "descomentar HTML");
    }
    {   // localizar e substituir
        CodeEditor e;
        e.setPlainText("foo bar Foo");
        FindBar fb;
        fb.editor = [&e] { return &e; };
        fb.find->setText("foo");
        fb.repl->setText("baz");
        fb.replaceAll();
        check(e.toPlainText() == "baz bar baz", "substituir tudo (sem diferenciar maiusculas)");
    }
    {   // arquivos
        QTemporaryDir dir;
        const QString p = dir.filePath("t.txt");
        QString err;
        check(saveTextFile(p, "a\nb\n", true, "UTF-8", err), "salvar CRLF");
        LoadResult r = loadTextFile(p);
        check(r.ok && r.crlf && r.text == "a\nb\n" && r.enc == "UTF-8", "ler CRLF");
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write("caf\xE9");
        f.close();
        r = loadTextFile(p);
        check(r.ok && r.enc == "ISO-8859-1" && r.text == QString::fromUtf8("café"), "ler Latin-1");
        check(saveTextFile(p, r.text, false, r.enc, err) && QFile(p).size() == 4, "salvar Latin-1 do mesmo jeito");
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray("ab\0cd", 5));
        f.close();
        check(!loadTextFile(p).ok, "recusa arquivo binario");
        check(!loadTextFile(dir.filePath("nao-existe")).ok, "arquivo inexistente");
    }
    {   // utilidades
        QString f; int l = 0, c = 0;
        check(parseLocation("  File \"/x/y.py\", line 12, in <module>", f, l, c) && f == "/x/y.py" && l == 12, "local no traceback do Python");
        check(parseLocation("main.cpp:34:5: error: oops", f, l, c) && f == "main.cpp" && l == 34 && c == 5, "local no erro do gcc");
        check(!parseLocation("nada de mais aqui", f, l, c), "linha sem local");
        check(fuzzyScore("src/main.py", "smp") >= 0 && fuzzyScore("src/main.py", "zzz") < 0, "busca aproximada");
        check(fuzzyScore("src/main.py", "main") > fuzzyScore("src/utils.py", "main"), "busca aproximada ordena por nome");
    }
    {   // varios cursores
        CodeEditor e;
        e.setLanguage(langById("python"));
        e.setPlainText("a\nb\nc");
        QTextCursor c = e.textCursor();
        c.setPosition(0);
        e.setTextCursor(c);
        e.addCursorVertical(1);
        e.addCursorVertical(1);
        check(e.cursorCount() == 3, "Ctrl+Alt+Baixo adiciona cursores");
        type(e, "X");
        check(e.toPlainText() == "Xa\nXb\nXc", "digitar em varios cursores");
        QKeyEvent bk(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QApplication::sendEvent(&e, &bk);
        check(e.toPlainText() == "a\nb\nc" && e.cursorCount() == 3, "backspace em varios cursores");
        e.document()->undo();
        e.clearExtraCursors();
        check(e.cursorCount() == 1, "Esc/limpar volta a um cursor");
    }
    {
        CodeEditor e;
        e.setLanguage(langById("python"));
        e.setPlainText("foo bar foo foo");
        QTextCursor c = e.textCursor();
        c.setPosition(1);
        e.setTextCursor(c);
        e.addNextOccurrence();   // seleciona a palavra
        e.addNextOccurrence();
        e.addNextOccurrence();
        check(e.cursorCount() == 3, "Ctrl+D adiciona as proximas ocorrencias");
        type(e, "x");
        check(e.toPlainText() == "x bar x x", "editar todas as ocorrencias de uma vez");
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&e, &esc);
        check(e.cursorCount() == 1, "Esc limpa os cursores extras");
        e.setPlainText("one two one");
        c = e.textCursor(); c.setPosition(1); e.setTextCursor(c);
        e.selectAllOccurrences();
        check(e.cursorCount() == 2, "Ctrl+Shift+L seleciona todas");
    }
    {
        CodeEditor e;
        e.setLanguage(langById("python"));
        e.setPlainText("  a\n  b");
        QTextCursor c = e.textCursor();
        c.setPosition(3);
        e.setTextCursor(c);
        e.addCursorVertical(1);
        QKeyEvent rt(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
        QApplication::sendEvent(&e, &rt);
        check(e.toPlainText() == "  a\n  \n  b\n  ", "Enter com varios cursores mantem a indentacao");
        check(e.cursorCount() == 2, "continua com dois cursores");
    }
    {   // pontos de parada acompanham a linha
        CodeEditor e;
        e.setPlainText("a\nb\nc\nd");
        e.toggleBreakpoint(2);
        e.toggleBreakpoint(4);
        check(e.breakpoints() == QList<int>({2, 4}), "pontos de parada");
        QTextCursor c(e.document());
        c.insertText("novo\n");
        check(e.breakpoints() == QList<int>({3, 5}), "pontos de parada acompanham as linhas");
        e.toggleBreakpoint(3);
        check(e.breakpoints() == QList<int>({5}), "tirar ponto de parada");
        e.setDebugLine(2);
        check(e.debugLine == 2, "linha de depuracao");
    }
    {   // minimapa: nao quebra e ocupa espaco
        CodeEditor e;
        e.resize(600, 300);
        e.setPlainText("def f():\n    return 1\n");
        e.show();
        QApplication::processEvents();
        const QPixmap pm = e.grab();
        check(!pm.isNull() && e.minimapVisible(), "minimapa ligado");
        e.setMinimapVisible(false);
        QApplication::processEvents();
        check(!e.minimapVisible(), "minimapa desligado");
    }
    {   // terminal: sequencias de controle
        PtyTerm t(QStringLiteral("/bin/sh"), {});
        t.feed("hello\r\n\x1b[31mred\x1b[0m!");
        check(t.lineText(0) == "hello" && t.lineText(1) == "red!", "terminal: texto e quebra de linha");
        check(t.cellAt(1, 0).fg == PtyTerm::paletteColor(1).rgba() && t.cellAt(1, 3).fg == 0, "terminal: cor ANSI e reset");
        t.feed("\x1b[3;5H");
        check(t.curY() == 2 && t.curX() == 4, "terminal: posicionar cursor");
        t.feed("\x1b[2J\x1b[H");
        check(t.lineText(0).isEmpty() && t.curX() == 0 && t.curY() == 0, "terminal: limpar tela");
        t.feed("\x1b[38;5;196mA\x1b[38;2;1;2;3mB");
        check(t.cellAt(0, 0).fg == PtyTerm::paletteColor(196).rgba() && t.cellAt(0, 1).fg == 0xFF010203u, "terminal: 256 cores e truecolor");
        t.feed("\r\x1b[2P");
        check(t.lineText(0).isEmpty(), "terminal: apagar caracteres (DCH)");
        t.feed("abc\r\x1b[2@");
        check(t.lineText(0) == "  abc", "terminal: inserir caracteres (ICH)");
        t.feed("\x1b[2J\x1b[H" + QByteArray(85, 'z'));
        check(t.lineText(0).size() == 80 && t.lineText(1) == "zzzzz", "terminal: quebra automatica na ultima coluna");
        t.feed("\x1b[?1049h");
        check(t.altScreen(), "terminal: tela alternativa (vim/less/top)");
        t.feed("tela\x1b[?1049l");
        check(!t.altScreen() && t.lineText(0).size() == 80, "terminal: volta da tela alternativa sem perder o conteudo");
        t.feed("\x1b[2J\x1b[H");
        for (int i = 0; i < 40; ++i) t.feed(QByteArray("linha ") + QByteArray::number(i) + "\r\n");
        check(t.histSize() > 10 && t.lineText(t.curY() - 1).startsWith("linha 39"), "terminal: rolagem e historico");
        t.feed("\x1b[2J\x1b[H\x1b[5;20r\x1b[5;1Hx\r\ny");
        check(t.lineText(4) == "x" && t.lineText(5) == "y", "terminal: regiao de rolagem");
        t.feed("\xc3\xa7\xe2\x94\x80");
        check(t.lineText(5).contains(QString::fromUtf8("ç─")), "terminal: UTF-8");
    }
    {   // terminal: processo de verdade num pty
        auto waitFor = [](const std::function<bool()> &cond, int ms) {
            QElapsedTimer t;
            t.start();
            while (!cond() && t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(5); }
            return cond();
        };
        PtyTerm t(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("if [ -t 0 ] && [ -t 1 ]; then echo ISATTY; fi; stty size; echo FIM")});
        t.start();
        check(waitFor([&] { return t.allText().contains("FIM"); }, 4000), "pty: processo roda e escreve");
        check(t.allText().contains("ISATTY"), "pty: o programa enxerga um terminal de verdade");
        check(t.allText().contains("24 80"), "pty: tamanho do terminal chega ao programa");
        check(waitFor([&] { return t.allText().contains("processo encerrado"); }, 3000) && !t.running(), "pty: detecta o fim do processo");

        PtyTerm sh(QStringLiteral("/bin/sh"), {});
        sh.start();
        sh.sendLine(QStringLiteral("echo $((6*7))"));
        const bool answered = waitFor([&] { for (int r = 0; r < sh.nRows(); ++r) { const QString l = sh.lineText(r); if (l.endsWith("42") && !l.contains("echo")) return true; } return false; }, 4000);
        if (!answered) std::fprintf(stderr, "--- tela do shell ---\n%s\n", qPrintable(sh.allText()));
        check(answered, "pty: shell interativo responde");

        PtyTerm cat(QStringLiteral("/bin/cat"), {QStringLiteral("-v")});
        cat.start();
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(&cat, &up);
        QKeyEvent cc(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
        check(waitFor([&] { return cat.allText().contains("^[[A"); }, 3000), "pty: tecla seta chega como sequencia de escape");
        QApplication::sendEvent(&cat, &cc);
        check(waitFor([&] { return !cat.running(); }, 3000), "pty: Ctrl+C interrompe o programa");
    }
    std::printf("%s: %d verificacoes, %d falha(s)\n", bad ? "FALHOU" : "OK", ran, bad);
    return bad ? 1 : 0;
}

static void usage() {
    std::printf("Cisma Code %s\n\n"
                "uso: cisma-code [ARQUIVO[:LINHA] | PASTA ...]\n"
                "     cisma-code --version | --help\n"
                "     cisma-code --grab ARQ.png [--do ACAO] [ARQUIVO ...]   grava a janela num PNG e sai (testes)\n"
                "     cisma-code --selftest                      autoteste (use QT_QPA_PLATFORM=offscreen)\n",
                APP_VERSION);
}

int main(int argc, char *argv[]) {
    // (o --version/--help nao precisam de tela)
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        if (a == "--version") { std::printf("Cisma Code %s\n", APP_VERSION); return 0; }
        if (a == "--help" || a == "-h") { usage(); return 0; }
    }
    QApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("cisma-code"));
    QGuiApplication::setDesktopFileName(QStringLiteral("cisma-code"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(APP_VERSION));
    applyDarkPalette(app);

    QStringList args = app.arguments().mid(1);
    if (args.contains(QStringLiteral("--selftest"))) return selfTest();

    QString grab;
    const int gi = args.indexOf(QStringLiteral("--grab"));
    if (gi >= 0) {
        if (gi + 1 >= args.size()) { usage(); return 2; }
        grab = args[gi + 1];
        args.removeAt(gi + 1);
        args.removeAt(gi);
    }
    QString doAction;
    const int di = args.indexOf(QStringLiteral("--do"));
    if (di >= 0 && di + 1 < args.size()) { doAction = args[di + 1]; args.removeAt(di + 1); args.removeAt(di); }
    QStringList paths;
    for (const QString &a : args) {
        if (a.startsWith(QStringLiteral("--"))) continue;
        static const QRegularExpression re(QStringLiteral("^(.*):(\\d+)$"));
        const auto m = re.match(a);
        if (!QFileInfo::exists(a) && m.hasMatch() && QFileInfo::exists(m.captured(1)))
            paths << QFileInfo(m.captured(1)).absoluteFilePath() + QLatin1Char(':') + m.captured(2);
        else paths << QFileInfo(a).absoluteFilePath();
    }

    if (grab.isEmpty()) {  // ja tem um Cisma Code aberto? entrega os arquivos a ele e sai
        QLocalSocket sock;
        sock.connectToServer(QStringLiteral("cisma-code-%1").arg(getuid()));
        if (sock.waitForConnected(300)) {
            sock.write((paths.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
            sock.waitForBytesWritten(1500);
            sock.disconnectFromServer();
            return 0;
        }
    }

    MainWindow w(grab.isEmpty());
    if (!grab.isEmpty()) {
        w.handleRemote(paths);
        w.grabAndSave(grab, doAction);
        return app.exec();
    }
    w.restoreWindow();
    if (paths.isEmpty()) w.restoreSession(); else w.handleRemote(paths);
    w.show();
    return app.exec();
}
