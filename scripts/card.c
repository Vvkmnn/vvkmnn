/*
 * card.c — renders the Vvkmnn profile card from data.txt (written by fetch.sh).
 *
 * Output (into OUT_DIR):
 *   card-{desktop,tablet,phone}-{light,dark}.svg  three columns: packages, code, ai
 *   cat-{light,dark}.svg                          the ᓚᘏᗢ easter egg (own link in README)
 *
 * Look: GitHub-native system fonts; Kanagawa palette (rebelot/kanagawa.nvim) —
 * "wave" on GitHub dark, "lotus" on light. Monochrome text, each mark tinted:
 *   packages -> one hue per tool, stepping through the spectrum by rank
 *   code     -> one hue per language, matched to its familiar GitHub color
 *   ai       -> one hue family per maker; model families are shades within it
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra -o card scripts/card.c -lm
 * Usage: ./card data/data.txt assets
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cat.h"

enum { MAX_PKGS = 32, MAX_LANGS = 64, MAX_MODELS = 128, MAX_DAYS = 4096, ROWS = 7 };
static const double PITCH = 18, ROW_TOP = 30;
static const double BLOCK_H = 30 + (ROWS - 1) * 18 + 32; /* ROW_TOP + rows + stats line */
static const char *FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI','Noto Sans',Helvetica,Arial,sans-serif";

/* ------------------------------------------------------------------ data */
typedef struct { char name[32]; long total; int ndays; long *days; } Pkg;
typedef struct { char name[32]; long repos; } Lang;
typedef struct { char maker[32]; char family[32]; long long tokens; } Model; /* tokens < 0: unknown */

static struct {
    long installs, daily, stars, forks, commits, lines, langs_total;
    Pkg pkgs[MAX_PKGS]; int npkgs;
    Lang langs[MAX_LANGS]; int nlangs;
    Model models[MAX_MODELS]; int nmodels;
} D;

static void die(const char *msg) { fprintf(stderr, "card: %s\n", msg); exit(1); }

/** Parses data.txt; every record type is documented in fetch.sh. */
static void load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open data file");
    static char line[1 << 16];
    while (fgets(line, sizeof line, f)) {
        char kind[16];
        if (sscanf(line, "%15s", kind) != 1) continue;
        const char *rest = line + strlen(kind);
        if (!strcmp(kind, "installs")) D.installs = strtol(rest, NULL, 10);
        else if (!strcmp(kind, "daily")) D.daily = strtol(rest, NULL, 10);
        else if (!strcmp(kind, "stars")) D.stars = strtol(rest, NULL, 10);
        else if (!strcmp(kind, "forks")) D.forks = strtol(rest, NULL, 10);
        else if (!strcmp(kind, "commits")) D.commits = strtol(rest, NULL, 10);
        else if (!strcmp(kind, "lines")) D.lines = strtol(rest, NULL, 10);
        else if (!strcmp(kind, "langs_total")) D.langs_total = strtol(rest, NULL, 10);
        else if (!strcmp(kind, "lang") && D.nlangs < MAX_LANGS) {
            Lang *l = &D.langs[D.nlangs++];
            if (sscanf(rest, "%31s %ld", l->name, &l->repos) != 2) die("bad lang record");
            for (char *c = l->name; *c; c++) if (*c == '_') *c = ' ';
        } else if (!strcmp(kind, "model") && D.nmodels < MAX_MODELS) {
            Model *m = &D.models[D.nmodels++];
            if (sscanf(rest, "%31s %31s %lld", m->maker, m->family, &m->tokens) != 3) die("bad model record");
        } else if (!strcmp(kind, "pkg") && D.npkgs < MAX_PKGS) {
            Pkg *p = &D.pkgs[D.npkgs++];
            int used = 0;
            if (sscanf(rest, "%31s %ld%n", p->name, &p->total, &used) != 2) die("bad pkg record");
            p->days = calloc(MAX_DAYS, sizeof *p->days);
            if (!p->days) die("out of memory");
            const char *s = rest + used;
            char *end;
            for (long v = strtol(s, &end, 10); end != s && p->ndays < MAX_DAYS; v = strtol(s, &end, 10)) {
                p->days[p->ndays++] = v;
                s = end;
            }
        }
    }
    fclose(f);
    if (!D.npkgs || !D.nlangs || !D.nmodels) die("data file is missing pkg/lang/model records");
}

/* ------------------------------------------------------------------ palette */
typedef struct { const char *key, *dark, *light; } Hue;
typedef struct { const char *text, *muted; double mark; int dark; } Theme;

static const Theme THEMES[2] = {
    { "#545464", "#8a8980", 0.90, 0 }, /* lotus (GitHub light) */
    { "#DCD7BA", "#727169", 0.85, 1 }, /* wave  (GitHub dark)  */
};
static const char *PKG_HUES[2][7] = {
    { "#4d699b", "#4e8ca2", "#597b75", "#6f894e", "#de9800", "#cc6d00", "#b35b79" },
    { "#7E9CD8", "#7FB4CA", "#7AA89F", "#98BB6C", "#E6C384", "#FFA066", "#D27E99" },
};
static const Hue LANG_HUES[] = {
    { "Python", "#7E9CD8", "#4d699b" }, { "JavaScript", "#E6C384", "#de9800" },
    { "TypeScript", "#7FB4CA", "#6693bf" }, { "C", "#938AA9", "#766b90" },
    { "Shell", "#98BB6C", "#6f894e" }, { "Vim Script", "#76946A", "#6e915f" },
    { "Ruby", "#E46876", "#c84053" }, { "Lua", "#957FB8", "#624c83" },
};
static const Hue FAMILY_HUES[] = {
    { "opus", "#FFA066", "#cc6d00" }, { "sonnet", "#DCA561", "#de9800" },
    { "fable", "#D27E99", "#b35b79" }, { "haiku", "#C0A36E", "#836f4a" },
    { "gpt", "#98BB6C", "#6f894e" }, { "whisper", "#7AA89F", "#5e857a" },
    { "qwen", "#957FB8", "#624c83" }, { "soprano", "#938AA9", "#766b90" },
    { "gemini", "#7E9CD8", "#4d699b" }, { "gemma", "#9CABCA", "#6693bf" },
    { "inception", "#7FB4CA", "#4e8ca2" }, { "deepseek", "#A3D4D5", "#5a7785" },
    { "vgg", "#938056", "#77713f" }, { "nemotron", "#76946A", "#6e915f" },
    { "llama", "#658594", "#5d57a3" }, { "resnet", "#717C7C", "#716e61" },
};

static const char *hue(const Hue *table, size_t n, const char *key, const Theme *t) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(table[i].key, key)) return t->dark ? table[i].dark : table[i].light;
    return t->muted;
}
#define HUE(table, key, t) hue(table, sizeof table / sizeof *table, key, t)

/* ------------------------------------------------------------------ formatting */
/** 14228 -> "14,228" */
static void commas(long n, char *out) {
    char raw[32];
    int len = snprintf(raw, sizeof raw, "%ld", n), o = 0;
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = '\0';
}

/** 8908 -> "8.9k", 433435 -> "433k", 942 -> "942" */
static void kfmt(long n, char *out, size_t cap) {
    if (n < 1000) { snprintf(out, cap, "%ld", n); return; }
    if (n >= 10000) { snprintf(out, cap, "%.0fk", n / 1000.0); return; }
    snprintf(out, cap, "%.1fk", n / 1000.0);
    char *dot = strstr(out, ".0k");
    if (dot) memmove(dot, dot + 2, 2); /* "9.0k" -> "9k" */
}

static void text(FILE *f, double x, double y, const char *s, const char *fill, double size,
                 const char *anchor, const char *extra) {
    fprintf(f, "<text x=\"%.1f\" y=\"%.1f\" font-size=\"%g\" fill=\"%s\" text-anchor=\"%s\"%s>%s</text>\n",
            x, y, size, fill, anchor, extra, s);
}

/* ------------------------------------------------------------------ columns */
/** packages: one AM strip per tool on a shared timeline, so each starts at its publish date. */
static void packages_col(FILE *f, double w, const Theme *t) {
    double label = w < 230 ? 66 : 76, x1 = w - 34, span = x1 - label, gmax = 1;
    int days = D.pkgs[0].ndays;
    for (int i = 0; i < D.npkgs; i++)
        for (int d = 0; d < D.pkgs[i].ndays; d++)
            if (D.pkgs[i].days[d] > gmax) gmax = (double)D.pkgs[i].days[d];
    for (int i = 0; i < D.npkgs && i < ROWS; i++) {
        const Pkg *p = &D.pkgs[i];
        double y = ROW_TOP + i * PITCH;
        int start = 0;
        while (start < p->ndays && p->days[start] == 0) start++;
        text(f, 0, y + 4, p->name, t->text, 12, "start", "");
        fprintf(f, "<polyline class=\"r\" style=\"animation-delay:%.1fs\" fill=\"none\" stroke=\"%s\" "
                   "stroke-opacity=\"%.2f\" stroke-width=\"1.1\" points=\"", i * 0.1, PKG_HUES[t->dark][i % 7], t->mark);
        int k = 0;
        for (double x = label + (double)start / days * span; x <= x1; x += 1.6, k++) {
            int idx = (int)((x - label) / span * days);
            if (idx > days - 1) idx = days - 1;
            double sum = 0;
            int cnt = 0;
            for (int j = idx - 3; j <= idx + 3; j++)
                if (j >= 0 && j < p->ndays) { sum += (double)p->days[j]; cnt++; }
            double a = fmax(0.5, sqrt(sum / cnt / gmax) * 6.5);
            fprintf(f, "%.1f,%.1f ", x, y + (k % 2 ? a : -a));
        }
        fprintf(f, "\"/>\n");
        char v[16];
        kfmt(p->total, v, sizeof v);
        text(f, w, y + 4, v, t->muted, 11.5, "end", " font-variant-numeric=\"tabular-nums\"");
    }
}

/** code: languages as tapered ribbons; length = share of repos, staggered from the right. */
static void code_col(FILE *f, double w, const Theme *t) {
    int n = D.nlangs < ROWS ? D.nlangs : ROWS;
    double total = 0, label = w < 230 ? 66 : 76, x1 = w - 34, span = x1 - label;
    for (int i = 0; i < n; i++) total += (double)D.langs[i].repos;
    double max = D.langs[0].repos / total;
    for (int i = 0; i < n; i++) {
        const Lang *l = &D.langs[i];
        double y = ROW_TOP + i * PITCH, share = l->repos / total, x0 = x1 - share / max * span;
        const char *c = HUE(LANG_HUES, l->name, t);
        char lower[32], pct[8];
        size_t j = 0;
        for (; l->name[j] && j < sizeof lower - 1; j++) lower[j] = (char)tolower((unsigned char)l->name[j]);
        lower[j] = '\0';
        text(f, 0, y + 4, lower, t->text, 12, "start", "");
        fprintf(f, "<defs><linearGradient id=\"c%d\" x1=\"0\" x2=\"1\"><stop offset=\"0\" stop-color=\"%s\" stop-opacity=\"0\"/>"
                   "<stop offset=\"1\" stop-color=\"%s\" stop-opacity=\"%.2f\"/></linearGradient></defs>\n", i, c, c, t->mark);
        fprintf(f, "<rect class=\"r\" style=\"animation-delay:%.1fs\" x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"4\" rx=\"2\" fill=\"url(#c%d)\"/>\n",
                i * 0.1, x0, y - 2, x1 - x0, i);
        snprintf(pct, sizeof pct, "%d%%", (int)lround(share * 100));
        text(f, w, y + 4, pct, t->muted, 11.5, "end", " font-variant-numeric=\"tabular-nums\"");
    }
}

typedef struct { char maker[32]; int idx[MAX_MODELS]; int n; double tokens; } Maker;

static double model_tokens(const Model *m) { return m->tokens < 0 ? 0 : (double)m->tokens; }

static double family_total(const Maker *mk, const char *family) {
    double s = 0;
    for (int i = 0; i < mk->n; i++)
        if (!strcmp(D.models[mk->idx[i]].family, family)) s += model_tokens(&D.models[mk->idx[i]]);
    return s;
}

static const Maker *sort_maker; /* qsort context: models are clustered by family, then by tokens */
static int cmp_model(const void *a, const void *b) {
    const Model *x = &D.models[*(const int *)a], *y = &D.models[*(const int *)b];
    double fx = family_total(sort_maker, x->family), fy = family_total(sort_maker, y->family);
    if (fx != fy) return fx < fy ? 1 : -1;
    int c = strcmp(x->family, y->family);
    if (c) return c;
    return (x->tokens < y->tokens) - (x->tokens > y->tokens);
}
static int cmp_maker(const void *a, const void *b) {
    const Maker *x = a, *y = b;
    if (x->n != y->n) return y->n - x->n;
    return (x->tokens < y->tokens) - (x->tokens > y->tokens);
}

static int maker_count(void) {
    int n = 0;
    for (int i = 0; i < D.nmodels; i++) {
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) seen = !strcmp(D.models[i].maker, D.models[j].maker);
        n += !seen;
    }
    return n;
}

/** ai: top makers (one per row); one square per model, color = family, size = usage (log). */
static void ai_col(FILE *f, double w, const Theme *t) {
    static Maker mk[MAX_MODELS];
    int nmk = 0;
    for (int i = 0; i < D.nmodels; i++) {
        int k = 0;
        while (k < nmk && strcmp(mk[k].maker, D.models[i].maker)) k++;
        if (k == nmk) { memset(&mk[k], 0, sizeof mk[k]); snprintf(mk[k].maker, sizeof mk[k].maker, "%s", D.models[i].maker); nmk++; }
        mk[k].idx[mk[k].n++] = i;
        mk[k].tokens += model_tokens(&D.models[i]);
    }
    for (int k = 0; k < nmk; k++) { sort_maker = &mk[k]; qsort(mk[k].idx, (size_t)mk[k].n, sizeof *mk[k].idx, cmp_model); }
    qsort(mk, (size_t)nmk, sizeof *mk, cmp_maker);
    int rows = nmk < ROWS ? nmk : ROWS;
    double label = w < 230 ? 66 : 76, x1 = w - 26, pitch = (x1 - label) / mk[0].n, cell = fmin(pitch * 0.8, 10);
    for (int r = 0; r < rows; r++) {
        double y = ROW_TOP + r * PITCH;
        text(f, 0, y + 4, mk[r].maker, t->text, 12, "start", "");
        fprintf(f, "<g class=\"r\" style=\"animation-delay:%.1fs\">", r * 0.1);
        for (int j = 0; j < mk[r].n; j++) {
            const Model *m = &D.models[mk[r].idx[j]];
            double tok = m->tokens < 0 ? 0 : (double)m->tokens;
            double scale = tok < 1e3 ? 0.45 : tok < 1e6 ? 0.6 : tok < 1e9 ? 0.8 : 1.0;
            double side = cell * scale, cx = x1 - j * pitch - cell / 2;
            fprintf(f, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"1.2\" fill=\"%s\" fill-opacity=\"%.2f\"/>",
                    cx - side / 2, y - side / 2, side, side, HUE(FAMILY_HUES, m->family, t), t->mark);
        }
        fprintf(f, "</g>\n");
        char v[8];
        snprintf(v, sizeof v, "%d", mk[r].n);
        text(f, w, y + 4, v, t->muted, 11.5, "end", " font-variant-numeric=\"tabular-nums\"");
    }
}

/* ------------------------------------------------------------------ card */
typedef void (*Column)(FILE *, double, const Theme *);

static void block(FILE *f, const char *title, Column col, const char *stats, double w, const Theme *t) {
    text(f, 0, 11, title, t->muted, 10, "start", " letter-spacing=\".12em\" font-weight=\"600\"");
    col(f, w, t);
    text(f, w, BLOCK_H - 2, stats, t->muted, 11, "end", "");
}

typedef enum { DESKTOP, TABLET, PHONE } Layout;
static const char *LAYOUT_NAMES[] = { "desktop", "tablet", "phone" };

static void card(const char *dir, Layout layout, const Theme *t) {
    char path[512], a[32], b[32], c[32], stats[3][96];
    snprintf(path, sizeof path, "%s/card-%s-%s.svg", dir, LAYOUT_NAMES[layout], t->dark ? "dark" : "light");
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write card svg");

    commas(D.installs, a);
    snprintf(stats[0], sizeof stats[0], "%s npm installs, %ld daily", a, D.daily);
    commas(D.commits, a); kfmt(D.lines, b, sizeof b); commas(D.stars, c);
    snprintf(stats[1], sizeof stats[1], "%s commits, %s lines, %s stars", a, b, c);
    snprintf(stats[2], sizeof stats[2], "%d models, %d makers", D.nmodels, maker_count());

    static const char *titles[] = { "PACKAGES", "CODE", "AI" };
    static const Column cols[] = { packages_col, code_col, ai_col };
    int stacked = layout == PHONE;
    double W = layout == DESKTOP ? 830 : layout == TABLET ? 640 : 390;
    double gap = layout == TABLET ? 20 : 28, colW = stacked ? W : (W - 2 * gap) / 3;
    double H = (stacked ? 3 * BLOCK_H + 2 * 18 : BLOCK_H) + 6;

    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%g\" height=\"%g\" viewBox=\"0 0 %g %g\" font-family=\"%s\">\n",
            W, H, W, H, FONT);
    fprintf(f, "<style>@keyframes in{from{opacity:0;transform:translateX(6px)}to{opacity:1;transform:none}}\n"
               ".r{animation:in .7s cubic-bezier(.16,1,.3,1) both}\n"
               "@media (prefers-reduced-motion:reduce){*{animation:none!important}}</style>\n");
    for (int i = 0; i < 3; i++) {
        double x = stacked ? 0 : i * (colW + gap), y = stacked ? i * (BLOCK_H + 18) : 0;
        fprintf(f, "<g transform=\"translate(%.1f,%g)\">\n", x, y);
        block(f, titles[i], cols[i], stats[i], colW, t);
        fprintf(f, "</g>\n");
    }
    fprintf(f, "</svg>\n");
    fclose(f);
}

/** ᓚᘏᗢ at 16px; the tail flicks up around its joint every 4.5s (negative rotate = up). */
static void cat(const char *dir, const Theme *t) {
    char path[512];
    snprintf(path, sizeof path, "%s/cat-%s.svg", dir, t->dark ? "dark" : "light");
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write cat svg");
    const double h = 16, vh = CAT_MAX_Y - CAT_MIN_Y;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%ld\" height=\"%g\" viewBox=\"0 %d %d %g\">\n",
            lround(h * CAT_ADVANCE / vh), h, -CAT_MAX_Y, CAT_ADVANCE, vh);
    fprintf(f, "<style>@keyframes flick{0%%,62%%,100%%{transform:rotate(0)}72%%{transform:rotate(-11deg)}"
               "80%%{transform:rotate(-2deg)}88%%{transform:rotate(-6.6deg)}}\n"
               ".tail{transform-box:fill-box;transform-origin:100%% 0%%;animation:flick 4.5s ease-in-out infinite}\n"
               "@media (prefers-reduced-motion:reduce){.tail{animation:none}}</style>\n");
    fprintf(f, "<g transform=\"scale(1,-1)\" fill=\"%s\">\n", t->muted);
    fprintf(f, "<g class=\"tail\"><path transform=\"translate(%d,0)\" d=\"%s\"/></g>\n", CAT_AT[0], CAT_PATHS[0]);
    for (int i = 1; i < 3; i++) fprintf(f, "<path transform=\"translate(%d,0)\" d=\"%s\"/>\n", CAT_AT[i], CAT_PATHS[i]);
    fprintf(f, "</g>\n</svg>\n");
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s data.txt out_dir\n", argv[0]); return 2; }
    load(argv[1]);
    for (int t = 0; t < 2; t++) {
        for (Layout l = DESKTOP; l <= PHONE; l++) card(argv[2], l, &THEMES[t]);
        cat(argv[2], &THEMES[t]);
    }
    for (int i = 0; i < D.npkgs; i++) free(D.pkgs[i].days);
    printf("card: %ld installs, %d langs, %d models -> %s\n", D.installs, D.nlangs, D.nmodels, argv[2]);
    return 0;
}
