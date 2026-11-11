// Andrey OS - réplica da interface do desktop em C++ (Cairo)
//
// Compilar:
//   g++ -std=c++17 -O2 andrey_os.cpp -o andrey_os $(pkg-config --cflags --libs cairo x11)
//
// Uso:
//   ./andrey_os              -> gera andrey_os.png (1672x941)
//   ./andrey_os --window     -> abre uma janela X11 com a interface
//   ./andrey_os --bg a.png   -> so o papel de parede
//   ./andrey_os --icons DIR  -> exporta os icones do desenho em SVG (tema de icones Andrey)

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo-svg.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const int W = 1672;
static const int H = 941;
static const double PI = 3.14159265358979323846;

// ---------------------------------------------------------------- paleta
struct Col { double r, g, b; };
static const Col RED      = {0.93, 0.07, 0.15};
static const Col RED_DARK = {0.55, 0.02, 0.07};
static const Col RED_HI   = {1.00, 0.20, 0.25};
static const Col WHITE    = {0.93, 0.93, 0.94};
static const Col GREY     = {0.62, 0.62, 0.65};

static void setc(cairo_t* cr, Col c, double a = 1.0) { cairo_set_source_rgba(cr, c.r, c.g, c.b, a); }

// ---------------------------------------------------------------- helpers
static void rrect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 1.5 * PI);
    cairo_close_path(cr);
}

static void font(cairo_t* cr, const char* family, double size, bool bold = false, bool italic = false) {
    cairo_select_font_face(cr, family, italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

static double text_width(cairo_t* cr, const std::string& s, double spacing = 0) {
    cairo_text_extents_t e;
    double w = 0;
    if (spacing == 0) { cairo_text_extents(cr, s.c_str(), &e); return e.x_advance; }
    // contagem por caractere UTF-8
    for (size_t i = 0; i < s.size();) {
        size_t n = 1;
        unsigned char c = s[i];
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        std::string ch = s.substr(i, n);
        cairo_text_extents(cr, ch.c_str(), &e);
        w += e.x_advance + spacing;
        i += n;
    }
    return w - spacing;
}

// desenha texto com espaçamento entre letras; align: 0 esquerda, 1 centro, 2 direita
static void text(cairo_t* cr, const std::string& s, double x, double y, double spacing = 0, int align = 0) {
    double w = text_width(cr, s, spacing);
    if (align == 1) x -= w / 2; else if (align == 2) x -= w;
    if (spacing == 0) { cairo_move_to(cr, x, y); cairo_show_text(cr, s.c_str()); return; }
    for (size_t i = 0; i < s.size();) {
        size_t n = 1;
        unsigned char c = s[i];
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        std::string ch = s.substr(i, n);
        cairo_text_extents_t e;
        cairo_text_extents(cr, ch.c_str(), &e);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, ch.c_str());
        x += e.x_advance + spacing;
        i += n;
    }
}

// texto esticado para ocupar exatamente a largura alvo (usado no logotipo)
static void text_fit(cairo_t* cr, const std::string& s, double x, double y, double target_w) {
    cairo_text_extents_t e;
    cairo_text_extents(cr, s.c_str(), &e);
    double sx = target_w / e.width;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, sx, 1.0);
    cairo_translate(cr, -e.x_bearing, 0);
    cairo_move_to(cr, 0, 0);
    cairo_text_path(cr, s.c_str());
    cairo_restore(cr);
}

// luz suave (brilho) usando camadas de traço
static void glow_stroke(cairo_t* cr, Col c, double w, double a) {
    for (int i = 5; i >= 1; --i) {
        setc(cr, c, a * 0.12 / i * 2);
        cairo_set_line_width(cr, w + i * 5);
        cairo_stroke_preserve(cr);
    }
    setc(cr, c, a);
    cairo_set_line_width(cr, w);
    cairo_stroke(cr);
}

// ---------------------------------------------------------------- fundo
static void draw_background(cairo_t* cr) {
    setc(cr, {0.012, 0.004, 0.008});
    cairo_paint(cr);

    // brilho avermelhado difuso
    cairo_pattern_t* g = cairo_pattern_create_radial(W * 0.52, H * 0.40, 10, W * 0.52, H * 0.40, 560);
    cairo_pattern_add_color_stop_rgba(g, 0, 0.30, 0.02, 0.05, 0.38);
    cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0);
    cairo_set_source(cr, g); cairo_paint(cr); cairo_pattern_destroy(g);

    // ondas superiores (esquerda)
    struct Wave { double p[8]; double w; double a; };
    std::vector<Wave> up = {
        {{ 60, 420, 300, 270, 560, 250, 760, 130}, 1.6, 0.55},
        {{ 20, 520, 280, 340, 520, 290, 780, 110}, 1.2, 0.40},
        {{150, 400, 340, 300, 600, 270, 820,  90}, 1.0, 0.30},
        {{ 80, 600, 330, 380, 540, 300, 740, 140}, 2.0, 0.22},
    };
    for (auto& w : up) {
        cairo_move_to(cr, w.p[0], w.p[1]);
        cairo_curve_to(cr, w.p[2], w.p[3], w.p[4], w.p[5], w.p[6], w.p[7]);
        glow_stroke(cr, RED, w.w, w.a);
    }
    // ondas superiores (direita)
    std::vector<Wave> ur = {
        {{1000, 120, 1150, 200, 1300, 260, 1450, 250}, 1.4, 0.45},
        {{ 980, 140, 1180, 260, 1330, 330, 1480, 280}, 1.0, 0.30},
        {{1020, 100, 1200, 160, 1360, 190, 1560, 140}, 1.2, 0.20},
    };
    for (auto& w : ur) {
        cairo_move_to(cr, w.p[0], w.p[1]);
        cairo_curve_to(cr, w.p[2], w.p[3], w.p[4], w.p[5], w.p[6], w.p[7]);
        glow_stroke(cr, RED, w.w, w.a);
    }

    // curvas da base (horizonte)
    std::vector<Wave> lo = {
        {{-40, 820, 400, 690, 1000, 700, 1400, 810}, 2.2, 0.75},
        {{-40, 850, 420, 730, 1020, 740, 1450, 850}, 1.5, 0.45},
        {{-40, 790, 380, 660, 980, 670, 1420, 780}, 1.2, 0.40},
        {{ 400, 760, 900, 690, 1300, 720, 1700, 880}, 1.0, 0.30},
        {{-40, 880, 500, 780, 1100, 790, 1700, 960}, 2.0, 0.35},
    };
    for (auto& w : lo) {
        cairo_move_to(cr, w.p[0], w.p[1]);
        cairo_curve_to(cr, w.p[2], w.p[3], w.p[4], w.p[5], w.p[6], w.p[7]);
        glow_stroke(cr, RED, w.w, w.a);
    }

    // brilho sob o horizonte
    cairo_pattern_t* h = cairo_pattern_create_linear(0, 690, 0, H);
    cairo_pattern_add_color_stop_rgba(h, 0.0, 0.8, 0.05, 0.1, 0.0);
    cairo_pattern_add_color_stop_rgba(h, 0.35, 0.7, 0.04, 0.08, 0.16);
    cairo_pattern_add_color_stop_rgba(h, 1.0, 0.0, 0.0, 0.0, 0.0);
    cairo_set_source(cr, h);
    cairo_rectangle(cr, 0, 690, W, H - 690);
    cairo_fill(cr);
    cairo_pattern_destroy(h);

    // vinheta
    cairo_pattern_t* v = cairo_pattern_create_radial(W / 2.0, H / 2.0, 380, W / 2.0, H / 2.0, 1100);
    cairo_pattern_add_color_stop_rgba(v, 0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(v, 1, 0, 0, 0, 0.55);
    cairo_set_source(cr, v); cairo_paint(cr); cairo_pattern_destroy(v);
}

// ---------------------------------------------------------------- ícones
static void icon_grid(cairo_t* cr, double cx, double cy, double sz = 8.5, double r = 2.4) {
    setc(cr, WHITE);
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j) {
            cairo_arc(cr, cx + i * sz, cy + j * sz, r, 0, 2 * PI);
            cairo_fill(cr);
        }
}

static void icon_folder(cairo_t* cr, double cx, double cy) {
    cairo_pattern_t* p = cairo_pattern_create_linear(0, cy - 17, 0, cy + 17);
    cairo_pattern_add_color_stop_rgb(p, 0, 0.95, 0.12, 0.18);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.62, 0.03, 0.08);
    cairo_set_source(cr, p);
    rrect(cr, cx - 19, cy - 14, 15, 9, 3);
    cairo_fill(cr);
    rrect(cr, cx - 19, cy - 10, 38, 26, 3.5);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

static void icon_firefox(cairo_t* cr, double cx, double cy) {
    // globo azul/roxo
    cairo_pattern_t* g = cairo_pattern_create_radial(cx - 4, cy - 4, 2, cx, cy, 17);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.38, 0.30, 0.85);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.15, 0.08, 0.45);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy + 1, 13, 0, 2 * PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    // chama laranja
    cairo_pattern_t* f = cairo_pattern_create_linear(cx - 18, cy - 18, cx + 18, cy + 18);
    cairo_pattern_add_color_stop_rgb(f, 0, 1.0, 0.85, 0.15);
    cairo_pattern_add_color_stop_rgb(f, 0.5, 1.0, 0.45, 0.10);
    cairo_pattern_add_color_stop_rgb(f, 1, 0.9, 0.10, 0.25);
    cairo_set_source(cr, f);
    cairo_move_to(cr, cx + 15, cy - 14);
    cairo_curve_to(cr, cx + 20, cy - 2, cx + 16, cy + 15, cx, cy + 18);
    cairo_curve_to(cr, cx - 14, cy + 19, cx - 20, cy + 4, cx - 14, cy - 8);
    cairo_curve_to(cr, cx - 12, cy - 4, cx - 9, cy - 2, cx - 6, cy - 4);
    cairo_curve_to(cr, cx - 10, cy - 12, cx - 2, cy - 18, cx + 4, cy - 14);
    cairo_curve_to(cr, cx + 8, cy - 18, cx + 12, cy - 17, cx + 15, cy - 14);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_pattern_destroy(f);
    // miolo roxo
    setc(cr, {0.30, 0.18, 0.65});
    cairo_arc(cr, cx - 1, cy + 3, 8.5, 0, 2 * PI);
    cairo_fill(cr);
}

static void icon_terminal(cairo_t* cr, double cx, double cy) {
    rrect(cr, cx - 18, cy - 15, 36, 30, 4);
    setc(cr, {0.10, 0.10, 0.12});
    cairo_fill_preserve(cr);
    setc(cr, {0.35, 0.35, 0.40});
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    setc(cr, WHITE);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - 12, cy - 7); cairo_line_to(cr, cx - 5, cy - 2); cairo_line_to(cr, cx - 12, cy + 3);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 3, cy + 5); cairo_line_to(cr, cx + 5, cy + 5);
    cairo_stroke(cr);
}

static void icon_monitor(cairo_t* cr, double cx, double cy) {
    rrect(cr, cx - 18, cy - 15, 36, 26, 3);
    setc(cr, {0.12, 0.12, 0.14});
    cairo_fill_preserve(cr);
    setc(cr, GREY);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);
    setc(cr, GREY);
    cairo_move_to(cr, cx - 6, cy + 11); cairo_line_to(cr, cx - 8, cy + 16);
    cairo_line_to(cr, cx + 8, cy + 16); cairo_line_to(cr, cx + 6, cy + 11);
    cairo_fill(cr);
    setc(cr, WHITE);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - 14, cy - 2);
    cairo_line_to(cr, cx - 8, cy - 2); cairo_line_to(cr, cx - 5, cy - 9);
    cairo_line_to(cr, cx, cy + 4); cairo_line_to(cr, cx + 4, cy - 4);
    cairo_line_to(cr, cx + 7, cy - 2); cairo_line_to(cr, cx + 14, cy - 2);
    cairo_stroke(cr);
}

static void icon_gear(cairo_t* cr, double cx, double cy, double R = 18) {
    int teeth = 8;
    cairo_new_path(cr);
    for (int i = 0; i < teeth; ++i) {
        double a0 = 2 * PI * i / teeth;
        double hw = PI / teeth * 0.52;
        double ro = R, ri = R * 0.78;
        cairo_line_to(cr, cx + ri * cos(a0 - PI / teeth), cy + ri * sin(a0 - PI / teeth));
        cairo_line_to(cr, cx + ro * cos(a0 - hw), cy + ro * sin(a0 - hw));
        cairo_line_to(cr, cx + ro * cos(a0 + hw), cy + ro * sin(a0 + hw));
        cairo_line_to(cr, cx + ri * cos(a0 + PI / teeth), cy + ri * sin(a0 + PI / teeth));
    }
    cairo_close_path(cr);
    cairo_pattern_t* p = cairo_pattern_create_linear(cx - R, cy - R, cx + R, cy + R);
    cairo_pattern_add_color_stop_rgb(p, 0, 0.82, 0.82, 0.84);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.42, 0.42, 0.46);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    // furo central
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, R * 0.38, 0, 2 * PI);
    cairo_set_source_rgb(cr, 0.03, 0.01, 0.02);
    cairo_fill(cr);
}

static void icon_power(cairo_t* cr, double cx, double cy) {
    setc(cr, RED);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 4.2);
    cairo_arc(cr, cx, cy + 2, 14.5, -PI / 2 + 0.62, -PI / 2 + 2 * PI - 0.62);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - 18);
    cairo_line_to(cr, cx, cy - 2);
    cairo_stroke(cr);
}

static void icon_trash(cairo_t* cr, double cx, double cy) {
    cairo_pattern_t* p = cairo_pattern_create_linear(0, cy - 16, 0, cy + 16);
    cairo_pattern_add_color_stop_rgb(p, 0, 0.95, 0.15, 0.20);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.65, 0.04, 0.10);
    cairo_set_source(cr, p);
    // tampa
    rrect(cr, cx - 15, cy - 13, 30, 5, 2); cairo_fill(cr);
    rrect(cr, cx - 5, cy - 17, 10, 5, 2); cairo_fill(cr);
    // corpo
    cairo_move_to(cr, cx - 12, cy - 6);
    cairo_line_to(cr, cx + 12, cy - 6);
    cairo_line_to(cr, cx + 10, cy + 16);
    cairo_curve_to(cr, cx + 10, cy + 17, cx + 9, cy + 18, cx + 8, cy + 18);
    cairo_line_to(cr, cx - 8, cy + 18);
    cairo_curve_to(cr, cx - 9, cy + 18, cx - 10, cy + 17, cx - 10, cy + 16);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    // ripas
    cairo_set_source_rgb(cr, 0.03, 0.01, 0.02);
    cairo_set_line_width(cr, 1.8);
    for (int i = -1; i <= 1; ++i) {
        cairo_move_to(cr, cx + i * 5, cy);
        cairo_line_to(cr, cx + i * 4.6, cy + 12);
        cairo_stroke(cr);
    }
}

// ---------------------------------------------------------------- barra superior
static void chevron(cairo_t* cr, double x, double y) {
    setc(cr, WHITE);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, x - 3.5, y - 1.5); cairo_line_to(cr, x, y + 2); cairo_line_to(cr, x + 3.5, y - 1.5);
    cairo_stroke(cr);
}

static void mini_logo(cairo_t* cr, double cx, double cy, double s) {
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s, s);
    setc(cr, RED);
    cairo_move_to(cr, 0, -14); cairo_line_to(cr, -13, 13); cairo_line_to(cr, -6, 13);
    cairo_line_to(cr, 0, 0); cairo_line_to(cr, 4, 8); cairo_line_to(cr, 14, 13);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_topbar(cairo_t* cr) {
    // fundo da barra
    cairo_pattern_t* g = cairo_pattern_create_linear(0, 0, 0, 40);
    cairo_pattern_add_color_stop_rgba(g, 0, 0.02, 0.0, 0.01, 0.95);
    cairo_pattern_add_color_stop_rgba(g, 1, 0.02, 0.0, 0.01, 0.70);
    cairo_set_source(cr, g);
    cairo_rectangle(cr, 0, 0, W, 39);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    mini_logo(cr, 38, 19, 0.95);

    font(cr, "Inter", 15.5);
    setc(cr, WHITE);
    text(cr, "Aplicativos", 82, 25);
    chevron(cr, 174, 19);
    text(cr, "Locais", 214, 25);
    chevron(cr, 272, 19);
    text(cr, "Sistema", 311, 25);
    chevron(cr, 381, 19);

    // data e hora
    text(cr, "Dom, 26 de Out", 740, 25);
    setc(cr, WHITE);
    cairo_arc(cr, 873, 20, 2, 0, 2 * PI); cairo_fill(cr);
    text(cr, "02:48", 890, 25);

    // ícone de tela
    setc(cr, WHITE);
    cairo_set_line_width(cr, 1.8);
    rrect(cr, 1458, 12, 20, 15, 2); cairo_stroke(cr);
    cairo_rectangle(cr, 1458, 19, 9, 8); cairo_fill(cr);

    // volume
    cairo_move_to(cr, 1500, 17); cairo_line_to(cr, 1504, 17); cairo_line_to(cr, 1510, 12);
    cairo_line_to(cr, 1510, 28); cairo_line_to(cr, 1504, 23); cairo_line_to(cr, 1500, 23);
    cairo_close_path(cr); cairo_fill(cr);
    cairo_set_line_width(cr, 1.8);
    cairo_arc(cr, 1511, 20, 5, -0.8, 0.8); cairo_stroke(cr);
    cairo_arc(cr, 1511, 20, 9, -0.8, 0.8); cairo_stroke(cr);

    // wifi
    for (int i = 0; i < 3; ++i) {
        cairo_arc(cr, 1553, 27, 6 + i * 5.5, -PI / 2 - 0.75, -PI / 2 + 0.75);
        cairo_set_line_width(cr, 2.2);
        cairo_stroke(cr);
    }
    cairo_arc(cr, 1553, 25.5, 2, 0, 2 * PI); cairo_fill(cr);

    // bateria
    rrect(cr, 1587, 13, 14, 20, 2.5); cairo_fill(cr);
    cairo_rectangle(cr, 1591, 10.5, 6, 3); cairo_fill(cr);
    text(cr, "100%", 1608, 25);
}

// ---------------------------------------------------------------- dock lateral
static void panel(cairo_t* cr, double x, double y, double w, double h, double r, double a = 0.82) {
    rrect(cr, x, y, w, h, r);
    setc(cr, {0.03, 0.005, 0.01}, a);
    cairo_fill_preserve(cr);
    setc(cr, RED, 0.35);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

static void draw_left_dock(cairo_t* cr) {
    panel(cr, -12, 143, 87, 469, 10);
    icon_grid(cr, 37, 182);
    icon_folder(cr, 37, 246);
    icon_firefox(cr, 37, 304);
    icon_terminal(cr, 37, 363);
    icon_monitor(cr, 37, 424);
    icon_gear(cr, 37, 484);
    icon_power(cr, 37, 544);
    setc(cr, RED);
    cairo_set_line_width(cr, 1.8);
    cairo_move_to(cr, 19, 593); cairo_line_to(cr, 56, 593);
    cairo_stroke(cr);
}

// ---------------------------------------------------------------- dock inferior
static void draw_bottom_dock(cairo_t* cr) {
    panel(cr, 617, 861, 438, 68, 14);
    icon_grid(cr, 657, 895, 8.5, 2.5);
    icon_firefox(cr, 727, 895);
    icon_terminal(cr, 795, 895);
    icon_folder(cr, 863, 896);
    icon_gear(cr, 930, 895, 17);
    setc(cr, RED, 0.7);
    cairo_set_line_width(cr, 1.2);
    cairo_move_to(cr, 974, 876); cairo_line_to(cr, 974, 915);
    cairo_stroke(cr);
    icon_trash(cr, 1016, 895);
}

// ---------------------------------------------------------------- painel do sistema
static void sparkline(cairo_t* cr, double x, double y, double w, double h,
                      const std::vector<double>& v) {
    cairo_new_path(cr);
    for (size_t i = 0; i < v.size(); ++i) {
        double px = x + w * i / (v.size() - 1);
        double py = y + h - v[i] * h;
        if (i == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
    }
    setc(cr, RED);
    cairo_set_line_width(cr, 1.3);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke_preserve(cr);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x, y + h);
    cairo_close_path(cr);
    cairo_pattern_t* p = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(p, 0, 0.93, 0.07, 0.15, 0.28);
    cairo_pattern_add_color_stop_rgba(p, 1, 0.93, 0.07, 0.15, 0.0);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

static std::vector<double> series(unsigned seed, int n, double base, double amp, int peaks) {
    std::vector<double> v(n, base);
    unsigned s = seed;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return ((s >> 8) & 0xFFFF) / 65535.0; };
    for (int i = 0; i < n; ++i) v[i] = base + (rnd() - 0.5) * amp * 0.35;
    for (int k = 0; k < peaks; ++k) {
        int c = 4 + (int)(rnd() * (n - 8));
        double a = amp * (0.5 + rnd() * 0.5);
        for (int d = -3; d <= 3; ++d)
            if (c + d >= 0 && c + d < n) v[c + d] += a * std::exp(-d * d / 2.2);
    }
    for (auto& x : v) x = std::fmax(0.02, std::fmin(0.98, x));
    return v;
}

static void rule(cairo_t* cr, double y) {
    setc(cr, RED, 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 1441, y); cairo_line_to(cr, 1633, y);
    cairo_stroke(cr);
}

static void draw_sysinfo(cairo_t* cr) {
    panel(cr, 1428, 68, 218, 528, 12, 0.78);

    auto title = [&](const char* t, double y) {
        font(cr, "Inter", 14.5, true);
        setc(cr, RED);
        text(cr, t, 1441, y);
    };
    auto row = [&](const char* k, const char* v, double y, double vx = 1525) {
        font(cr, "Inter", 13.5);
        setc(cr, WHITE, 0.92);
        text(cr, k, 1441, y);
        setc(cr, WHITE, 0.82);
        text(cr, v, vx, y);
    };

    title("SISTEMA", 95);
    row("Kernel:",   "6.16.0-andrey", 121);
    row("Uptime:",   "0h 12m 34s",    143);
    row("Packages:", "1250",          165);
    row("Shell:",    "bash 5.2.37",   187);
    rule(cr, 206);

    title("CPU", 233);
    row("Uso:", "3%", 259, 1472);
    sparkline(cr, 1441, 273, 192, 16, series(11, 70, 0.20, 0.35, 5));

    title("MEMÓRIA", 323);
    row("Uso:", "18%", 348, 1479);
    sparkline(cr, 1441, 356, 192, 24, series(23, 70, 0.30, 0.55, 5));

    title("DISCO", 412);
    row("Uso:", "7%", 438, 1472);
    sparkline(cr, 1441, 450, 192, 18, series(37, 70, 0.12, 0.65, 3));
    rule(cr, 483);

    title("REDE", 509);
    row("Download:", "12.4 KiB/s", 535, 1530);
    row("Upload:",   "8.7 KiB/s",  557, 1530);
    sparkline(cr, 1441, 565, 192, 18, series(53, 70, 0.10, 0.60, 4));
}

// ---------------------------------------------------------------- logotipo central
static void draw_logo(cairo_t* cr) {
    // brilho atrás do emblema
    cairo_pattern_t* g = cairo_pattern_create_radial(845, 350, 10, 845, 350, 280);
    cairo_pattern_add_color_stop_rgba(g, 0, 0.8, 0.05, 0.1, 0.22);
    cairo_pattern_add_color_stop_rgba(g, 1, 0.8, 0.05, 0.1, 0.0);
    cairo_set_source(cr, g); cairo_paint(cr); cairo_pattern_destroy(g);

    // ----- letra A (facetas)
    auto poly = [&](std::vector<std::pair<double,double>> pts, Col c0, Col c1) {
        cairo_new_path(cr);
        for (size_t i = 0; i < pts.size(); ++i)
            (i ? cairo_line_to : cairo_move_to)(cr, pts[i].first, pts[i].second);
        cairo_close_path(cr);
        cairo_pattern_t* p = cairo_pattern_create_linear(pts[0].first, pts[0].second,
                                                         pts[pts.size()/2].first, pts[pts.size()/2].second);
        cairo_pattern_add_color_stop_rgb(p, 0, c0.r, c0.g, c0.b);
        cairo_pattern_add_color_stop_rgb(p, 1, c1.r, c1.g, c1.b);
        cairo_set_source(cr, p);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    };
    // perna esquerda
    poly({{845,220},{728,430},{770,430},{848,296}}, {0.95,0.12,0.18}, {0.50,0.02,0.06});
    // faceta central escura
    poly({{845,220},{848,296},{900,340},{917,322}}, {0.75,0.06,0.10}, {0.35,0.01,0.04});
    // perna direita (grande)
    poly({{845,220},{917,322},{992,462},{895,392},{848,328},{818,340}}, {0.98,0.18,0.22}, {0.45,0.02,0.06});
    // barra interna
    poly({{780,380},{850,326},{895,392},{820,400}}, {0.30,0.01,0.04}, {0.15,0.0,0.02});
    // ponta inferior direita
    poly({{895,392},{992,462},{945,424},{930,372}}, {1.0,0.30,0.30}, {0.60,0.04,0.08});

    // ----- anel orbital (crescente afinando nas pontas)
    cairo_save(cr);
    cairo_translate(cr, 846, 348);
    cairo_rotate(cr, -0.34);
    cairo_new_path(cr);
    cairo_save(cr);
    cairo_scale(cr, 1.0, 0.27);
    cairo_arc(cr, 0, 0, 205, 0, 2 * PI);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_translate(cr, 12, 4);
    cairo_scale(cr, 1.0, 0.20);
    cairo_arc(cr, 0, 0, 190, 0, 2 * PI);
    cairo_restore(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_pattern_t* ring = cairo_pattern_create_linear(-205, 0, 205, 0);
    cairo_pattern_add_color_stop_rgb(ring, 0.0, 0.45, 0.02, 0.06);
    cairo_pattern_add_color_stop_rgb(ring, 0.45, 0.95, 0.12, 0.18);
    cairo_pattern_add_color_stop_rgb(ring, 0.85, 1.00, 0.20, 0.25);
    cairo_pattern_add_color_stop_rgb(ring, 1.0, 0.55, 0.03, 0.08);
    cairo_set_source(cr, ring);
    cairo_fill(cr);
    cairo_pattern_destroy(ring);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_restore(cr);

    // ----- "ANDREY" metálico + "OS" vermelho (itálico largo)
    font(cr, "Poppins", 96, true, true);
    text_fit(cr, "ANDREY", 417, 566, 610);
    cairo_pattern_t* m = cairo_pattern_create_linear(0, 503, 0, 566);
    cairo_pattern_add_color_stop_rgb(m, 0.0, 0.96, 0.90, 0.92);
    cairo_pattern_add_color_stop_rgb(m, 0.5, 0.74, 0.66, 0.69);
    cairo_pattern_add_color_stop_rgb(m, 1.0, 0.50, 0.42, 0.45);
    cairo_set_source(cr, m);
    cairo_fill(cr);
    cairo_pattern_destroy(m);

    text_fit(cr, "OS", 1053, 566, 200);
    cairo_pattern_t* r = cairo_pattern_create_linear(0, 503, 0, 566);
    cairo_pattern_add_color_stop_rgb(r, 0.0, 0.95, 0.10, 0.16);
    cairo_pattern_add_color_stop_rgb(r, 1.0, 0.60, 0.02, 0.07);
    cairo_set_source(cr, r);
    cairo_fill(cr);
    cairo_pattern_destroy(r);

    // ----- slogan
    font(cr, "Inter", 19.5, true);
    setc(cr, {0.62, 0.60, 0.63});
    text(cr, "VELOCIDADE", 388, 613, 10.4);
    text(cr, "PERFORMANCE", 702, 613, 10.6);
    text(cr, "DESEMPENHO", 1053, 613, 10.6);
    setc(cr, RED);
    cairo_arc(cr, 655, 605, 3.2, 0, 2 * PI); cairo_fill(cr);
    cairo_arc(cr, 1007, 605, 3.2, 0, 2 * PI); cairo_fill(cr);

    font(cr, "Inter", 18.5);
    setc(cr, {0.55, 0.53, 0.56});
    text(cr, "SUPERE OS LIMITES", 654, 656, 11.2);
    setc(cr, RED, 0.7);
    cairo_set_line_width(cr, 1.2);
    cairo_move_to(cr, 507, 650); cairo_line_to(cr, 628, 650); cairo_stroke(cr);
    cairo_move_to(cr, 1044, 650); cairo_line_to(cr, 1160, 650); cairo_stroke(cr);
}

// ---------------------------------------------------------------- cantos
static void draw_corners(cairo_t* cr) {
    font(cr, "Inter", 12.5, true);
    setc(cr, RED);
    text(cr, "ANDREY OS", 35, 883, 4.9);
    font(cr, "Inter", 12);
    setc(cr, {0.62, 0.60, 0.63});
    text(cr, "POTENCIAL MÁXIMO EM CADA DETALHE", 35, 900, 3.3);

    font(cr, "Inter", 12.5);
    setc(cr, {0.66, 0.64, 0.67});
    text(cr, "MAIS RÁPIDO.", 1637, 870, 4.5, 2);
    text(cr, "MAIS FORTE.",  1637, 888, 4.5, 2);
    // última linha: "MAIS " cinza + "ANDREY OS." vermelho
    double wA = text_width(cr, "ANDREY OS.", 4.5);
    setc(cr, RED);
    font(cr, "Inter", 12.5, true);
    text(cr, "ANDREY OS.", 1637 - wA, 906, 4.5);
    font(cr, "Inter", 12.5);
    setc(cr, {0.66, 0.64, 0.67});
    text(cr, "MAIS", 1637 - wA - 14, 906, 4.5, 2);
}

// ---------------------------------------------------------------- composição
static void render(cairo_t* cr) {
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    draw_background(cr);
    draw_logo(cr);
    draw_topbar(cr);
    draw_left_dock(cr);
    draw_bottom_dock(cr);
    draw_sysinfo(cr);
    draw_corners(cr);
}

// versão só do papel de parede (sem barras/docks/painel: esses são reais no sistema)
static void render_bg(cairo_t* cr) {
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    draw_background(cr);
    draw_logo(cr);
    draw_corners(cr);
}


// ---------------------------------------------------------------- exporta icones SVG
// Os mesmos icones desenhados na UI viram arquivos SVG 48x48 para o tema de icones "Andrey".
typedef void (*IconFn)(cairo_t*);

static void write_icon(const std::string& dir, const char* sub, const char* name, IconFn fn) {
    std::string d = dir + "/" + sub;
    mkdir(dir.c_str(), 0755);
    mkdir(d.c_str(), 0755);
    std::string path = d + "/" + name + ".svg";
    cairo_surface_t* surf = cairo_svg_surface_create(path.c_str(), 48, 48);
    cairo_t* cr = cairo_create(surf);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    fn(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    printf("  %s\n", path.c_str());
}

static void ic_grid(cairo_t* cr)     { icon_grid(cr, 24, 24, 11, 3.2); }
static void ic_folder(cairo_t* cr)   { icon_folder(cr, 24, 24); }
static void ic_terminal(cairo_t* cr) { icon_terminal(cr, 24, 24); }
static void ic_monitor(cairo_t* cr)  { icon_monitor(cr, 24, 24); }
static void ic_gear(cairo_t* cr)     { icon_gear(cr, 24, 24, 20); }
static void ic_power(cairo_t* cr)    { icon_power(cr, 24, 24); }
static void ic_trash(cairo_t* cr)    { icon_trash(cr, 24, 24); }
static void ic_logo(cairo_t* cr)     { mini_logo(cr, 24, 24, 1.55); }

static int export_icons(const std::string& dir) {
    printf("Exportando icones para %s\n", dir.c_str());
    // aplicativos (usados pelos lancadores andrey-*.desktop)
    write_icon(dir, "apps", "andrey-grid",     ic_grid);
    write_icon(dir, "apps", "andrey-files",    ic_folder);
    write_icon(dir, "apps", "andrey-terminal", ic_terminal);
    write_icon(dir, "apps", "andrey-monitor",  ic_monitor);
    write_icon(dir, "apps", "andrey-settings", ic_gear);
    write_icon(dir, "apps", "andrey-power",    ic_power);
    write_icon(dir, "apps", "andrey-trash",    ic_trash);
    write_icon(dir, "apps", "system-file-manager", ic_folder);
    // lugares/status (sobrescrevem o Adwaita onde o sistema usa estes nomes)
    write_icon(dir, "places", "start-here",      ic_logo);
    write_icon(dir, "places", "user-home",       ic_folder);
    write_icon(dir, "places", "folder",          ic_folder);
    write_icon(dir, "places", "user-trash",      ic_trash);
    write_icon(dir, "places", "user-trash-full", ic_trash);
    return 0;
}

// ---------------------------------------------------------------- janela X11
static int run_window() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { fprintf(stderr, "Sem display X11. Rode sem --window para gerar o PNG.\n"); return 1; }
    int scr = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0, 0, BlackPixel(dpy, scr));
    XStoreName(dpy, win, "Andrey OS");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
    Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &del, 1);
    XMapWindow(dpy, win);

    cairo_surface_t* surf = cairo_xlib_surface_create(dpy, win, DefaultVisual(dpy, scr), W, H);
    cairo_t* cr = cairo_create(surf);
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0) {
            cairo_save(cr);
            cairo_scale(cr, 1.0, 1.0);
            render(cr);
            cairo_restore(cr);
            cairo_surface_flush(surf);
        } else if (ev.type == KeyPress) break;
        else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == del) break;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    XCloseDisplay(dpy);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--window")) return run_window();
    if (argc > 1 && !strcmp(argv[1], "--icons"))  return export_icons(argc > 2 ? argv[2] : "icons");

    bool bg = argc > 1 && !strcmp(argv[1], "--bg");   // ./andrey_os --bg saida.png
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t* cr = cairo_create(surf);
    if (bg) render_bg(cr); else render(cr);
    const char* out = bg ? (argc > 2 ? argv[2] : "wallpaper.png")
                         : (argc > 1 ? argv[1] : "andrey_os.png");
    cairo_surface_write_to_png(surf, out);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    printf("Gerado: %s (%dx%d)\n", out, W, H);
    return 0;
}