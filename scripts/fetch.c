/*
 * fetch.c — gathers every live number on the profile card and prints data.txt for card.c.
 *
 * Pure C, no libraries: data comes from the `gh` and `curl` CLIs (preinstalled on GitHub's
 * runners) and is parsed with json.h. Run from the repo root:
 *
 *   ./fetch > data/data.txt        daily, in Actions (GH_TOKEN = the workflow token)
 *   ./fetch --rebaseline           optional, on the Mac with the owner's own `gh` login:
 *                                  rewrites data/baseline.txt with exact lifetime commits
 *
 * data.txt records (space-separated, first field = type; card.c accepts any order):
 *   installs N | daily N | stars N | forks N | langs_total N | commits N | lines N
 *   pkg NAME TOTAL V1 V2 ...   daily installs on a shared axis from the first publish
 *   lang NAME REPOS            (spaces in NAME become '_')
 *   model MAKER FAMILY TOKENS  (TOKENS = -1 when no usage was logged)
 *
 * Commits + lines = baseline + increment. The baseline was computed with the owner's login
 * (full contribution history, generated-activity repos excluded). The workflow token can't see all history
 * (org repos are hidden; ~1k commits short), so Actions only ADDS what it can see after
 * baseline_date: public commits authored by the user (commit search, default branches)
 * and the additions in them, skipping notebooks, lockfiles, minified files and SVGs (so
 * regenerated card assets never count as written code), recorded in a ledger
 * (data/increments.txt, see below). Accepted gap: post-baseline commits to private repos /
 * non-default branches are not counted; --rebaseline re-anchors and empties the ledger.
 *
 * The ai column (data/ai.json) is a frozen snapshot; see ai.c for how it is built.
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra -o fetch scripts/fetch.c -lm
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "json.h"

#define USER_LOGIN "Vvkmnn"
#define NPM_AUTHOR "vvkmnn"
/* Generated-activity repos, excluded from every number (they are not real work):
 *   paint  backdated gitfiti art on the contribution graph
 *   d      formerly "decoy": ghdecoy generator, 1,336 identical "ghdecoy" commits */
static const char *const EXCLUDED_REPOS[] = { "paint", "d" };

static int is_excluded(const char *name) {
    for (size_t i = 0; i < sizeof EXCLUDED_REPOS / sizeof *EXCLUDED_REPOS; i++)
        if (name && !strcmp(name, EXCLUDED_REPOS[i])) return 1;
    return 0;
}
#define LEGACY_PACKAGE "claude-historian" /* original name of historian-mcp; merged into it */
#define FIRST_YEAR 2012                   /* account creation year */

enum { MAX_PKGS = 64, MAX_DAYS = 4096, MAX_LANGS = 128 };

static void die(const char *msg) { fprintf(stderr, "fetch: %s\n", msg); exit(1); }

/** Runs a shell command built from fmt and returns its whole stdout; dies if it fails. */
static char *run(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= sizeof cmd) die("command too long");
    FILE *p = popen(cmd, "r");
    if (!p) die("popen failed");
    size_t cap = 1 << 16, n = 0;
    char *buf = jalloc(cap);
    for (size_t got; (got = fread(buf + n, 1, cap - n - 1, p)) > 0;) {
        n += got;
        if (n + 1 == cap) {
            buf = realloc(buf, cap *= 2);
            if (!buf) die("out of memory");
        }
    }
    buf[n] = '\0';
    if (pclose(p) != 0) { fprintf(stderr, "fetch: command failed: %s\n", cmd); exit(1); }
    return buf;
}

static void parse(const char *text, JVal *v) {
    if (!jparse(text, v)) die("invalid JSON from command");
}

/** API-provided names go into shell commands: allow only [A-Za-z0-9._/-]. */
static const char *safe(const char *s) {
    if (!s || !*s) die("missing name in API data");
    for (const char *c = s; *c; c++)
        if (!(*c == '.' || *c == '_' || *c == '/' || *c == '-' || (*c >= '0' && *c <= '9') ||
              (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')))
            die("unexpected character in API-provided name");
    return s;
}

/** UTC date `days_ahead` days from now, as YYYY-MM-DD. */
static void utc_date(char out[11], int days_ahead) {
    time_t t = time(NULL) + (time_t)days_ahead * 86400;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 11, "%Y-%m-%d", &tm);
}

static void today(char out[11]) { utc_date(out, 0); }

/** The calendar day before `date` (YYYY-MM-DD); noon avoids DST edge cases in mktime. */
static void day_before(const char *date, char out[11]) {
    struct tm tm = { 0 };
    tm.tm_year = atoi(date) - 1900;
    tm.tm_mon = atoi(date + 5) - 1;
    tm.tm_mday = atoi(date + 8) - 1;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    mktime(&tm);
    strftime(out, 11, "%Y-%m-%d", &tm);
}

/* ------------------------------------------------------------------ npm */
typedef struct { char name[64]; long total; int ndays; long days[MAX_DAYS]; } Pkg;
static Pkg pkgs[MAX_PKGS];
static int npkgs;

/** "claude-historian-mcp" -> "historian" (and the legacy "claude-historian" too). */
static void short_name(const char *pkg, char *out, size_t cap) {
    const char *s = strncmp(pkg, "claude-", 7) ? pkg : pkg + 7;
    snprintf(out, cap, "%s", s);
    size_t len = strlen(out);
    if (len > 4 && !strcmp(out + len - 4, "-mcp")) out[len - 4] = '\0';
}

static void add_package(const char *pkg, const char *end_day) {
    JVal v;
    parse(run("curl -fsS --retry 5 --retry-delay 4 'https://api.npmjs.org/downloads/range/2025-06-01:%s/%s'", end_day, safe(pkg)), &v);
    const JVal *days = jget(&v, "downloads");
    if (!days || days->type != J_ARR) die("npm range response has no downloads");
    char name[64];
    short_name(pkg, name, sizeof name);
    int k = 0;
    while (k < npkgs && strcmp(pkgs[k].name, name)) k++;
    if (k == npkgs) {
        if (npkgs == MAX_PKGS) die("too many packages");
        snprintf(pkgs[npkgs++].name, sizeof pkgs[k].name, "%s", name);
    }
    Pkg *p = &pkgs[k];
    if (p->ndays && p->ndays != (int)days->n) die("npm ranges differ in length");
    if (days->n > MAX_DAYS) die("too many days");
    p->ndays = (int)days->n;
    for (size_t i = 0; i < days->n; i++) {
        long n = (long)jnum(jget(jat(days, i), "downloads"), 0);
        p->days[i] += n;
        p->total += n;
    }
}

static int cmp_pkg(const void *a, const void *b) {
    const Pkg *x = a, *y = b;
    if (x->total != y->total) return x->total < y->total ? 1 : -1;
    return strcmp(x->name, y->name);
}

static void npm(void) {
    char day[11];
    today(day);
    JVal v;
    parse(run("curl -fsS --retry 5 --retry-delay 4 'https://registry.npmjs.org/-/v1/search?text=maintainer:%s&size=100'", NPM_AUTHOR), &v);
    const JVal *objs = jget(&v, "objects");
    int have_legacy = 0;
    for (size_t i = 0; objs && i < objs->n; i++) {
        const char *name = jstr(jpath(jat(objs, i), "package.name"));
        if (!name) continue;
        have_legacy |= !strcmp(name, LEGACY_PACKAGE);
        add_package(name, day);
    }
    if (!have_legacy) add_package(LEGACY_PACKAGE, day); /* deprecated, so search omits it */
    if (!npkgs) die("no npm packages found");

    int first = pkgs[0].ndays;
    long total = 0;
    for (int k = 0; k < npkgs; k++) {
        for (int d = 0; d < pkgs[k].ndays && d < first; d++)
            if (pkgs[k].days[d] > 0) { first = d; break; }
        total += pkgs[k].total;
    }
    int span = pkgs[0].ndays - first;
    printf("installs %ld\n", total);
    printf("daily %ld\n", span > 0 ? lround((double)total / span) : 0);
    qsort(pkgs, (size_t)npkgs, sizeof *pkgs, cmp_pkg);
    for (int k = 0; k < npkgs; k++) {
        printf("pkg %s %ld", pkgs[k].name, pkgs[k].total);
        for (int d = first; d < pkgs[k].ndays; d++) printf(" %ld", pkgs[k].days[d]);
        putchar('\n');
    }
}

/* ------------------------------------------------------------------ GitHub repos */
typedef struct { char name[64]; int repos; } Lang;

static const char *const MARKUP[] = { "HTML", "CSS", "SCSS", "Less", "TeX", "Makefile", "Dockerfile",
                                      "CMake", "Just", "Starlark", "XSLT", "Liquid", "Metal" };

static int is_markup(const char *lang) {
    for (size_t i = 0; i < sizeof MARKUP / sizeof *MARKUP; i++)
        if (!strcmp(MARKUP[i], lang)) return 1;
    return 0;
}

static int cmp_lang(const void *a, const void *b) {
    const Lang *x = a, *y = b;
    if (x->repos != y->repos) return y->repos - x->repos;
    return strcmp(x->name, y->name);
}

static void repos(void) {
    JVal v;
    parse(run("gh api graphql -f query='{ user(login:\"%s\"){ repositories(ownerAffiliations:OWNER,"
              "isFork:false,privacy:PUBLIC,first:100){ nodes{ name stargazerCount forkCount "
              "languages(first:30){ nodes{ name } } } } } }'", USER_LOGIN), &v);
    const JVal *nodes = jpath(&v, "data.user.repositories.nodes");
    if (!nodes) die("GitHub repos query returned nothing");
    static Lang langs[MAX_LANGS];
    int nlangs = 0;
    long stars = 0, forks = 0;
    for (size_t i = 0; i < nodes->n; i++) {
        const JVal *r = jat(nodes, i);
        const char *rname = jstr(jget(r, "name"));
        if (!rname || is_excluded(rname)) continue; /* malformed or generated activity */
        stars += (long)jnum(jget(r, "stargazerCount"), 0);
        forks += (long)jnum(jget(r, "forkCount"), 0);
        const JVal *ls = jpath(r, "languages.nodes");
        char seen[32][64]; /* per-repo dedupe after mapping notebooks to Python */
        int nseen = 0;
        for (size_t j = 0; ls && j < ls->n && nseen < 32; j++) {
            const char *name = jstr(jget(jat(ls, j), "name"));
            if (!name) continue;
            if (!strcmp(name, "Jupyter Notebook")) name = "Python";
            if (is_markup(name)) continue;
            int dup = 0;
            for (int s = 0; s < nseen && !dup; s++) dup = !strcmp(seen[s], name);
            if (dup) continue;
            snprintf(seen[nseen++], sizeof seen[0], "%s", name);
            int k = 0;
            while (k < nlangs && strcmp(langs[k].name, name)) k++;
            if (k == nlangs) {
                if (nlangs == MAX_LANGS) die("too many languages");
                snprintf(langs[nlangs++].name, sizeof langs[k].name, "%s", name);
            }
            langs[k].repos++;
        }
    }
    qsort(langs, (size_t)nlangs, sizeof *langs, cmp_lang);
    printf("stars %ld\nforks %ld\nlangs_total %d\n", stars, forks, nlangs);
    for (int k = 0; k < nlangs; k++) {
        char name[sizeof langs[k].name];
        memcpy(name, langs[k].name, sizeof name); /* same size: exact copy, NUL included */
        for (char *c = name; *c; c++) if (*c == ' ') *c = '_';
        printf("lang %s %d\n", name, langs[k].repos);
    }
}

/* ------------------------------------------------------------------ commits + lines */
static int skipped_file(const char *f) {
    if (!f) return 1; /* malformed entry: count nothing */
    size_t n = strlen(f);
    const char *suffixes[] = { ".ipynb", "lock.json", ".lock", ".svg" };
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
        size_t m = strlen(suffixes[i]);
        if (n >= m && !strcmp(f + n - m, suffixes[i])) return 1;
    }
    return strstr(f, ".min.") != NULL;
}

typedef struct { char date[11]; long commits, lines; } Baseline;

static Baseline read_baseline(void) {
    Baseline b = { "", -1, -1 };
    FILE *f = fopen("data/baseline.txt", "r");
    if (!f) die("cannot open data/baseline.txt (run from the repo root)");
    char key[32], val[32];
    while (fscanf(f, "%31s %31s", key, val) == 2) {
        if (!strcmp(key, "baseline_date")) {
            if (strlen(val) != 10) die("baseline_date must be YYYY-MM-DD");
            memcpy(b.date, val, sizeof b.date); /* 10 chars + NUL */
        }
        else if (!strcmp(key, "commits")) b.commits = strtol(val, NULL, 10);
        else if (!strcmp(key, "lines")) b.lines = strtol(val, NULL, 10);
    }
    fclose(f);
    if (!*b.date || b.commits < 0 || b.lines < 0) die("data/baseline.txt is incomplete");
    return b;
}

/*
 * Ledger of post-baseline commits already counted: data/increments.txt, one line each:
 *   YYYY-MM-DD SHA ADDITIONS
 * Each run searches only from the newest ledger day (inclusive, deduped by SHA), oldest
 * first, so it never approaches the search API's 1,000-result ceiling and makes only a
 * handful of API calls per day. Totals = baseline + ledger.
 *
 * Flood guard: author: search trusts commit metadata, so anyone could push commits that
 * claim this author. At most MAX_NEW_PER_RUN are added per run; the rest wait for the next
 * run (oldest first, nothing lost) — a spoofing flood can't spike the card overnight.
 */
#define LEDGER "data/increments.txt"
enum { MAX_LEDGER = 100000, MAX_NEW_PER_RUN = 200 };

typedef struct { char date[11]; char sha[41]; long additions; } Entry;
static Entry ledger[MAX_LEDGER];
static int nledger;

/** Exactly "YYYY-MM-DD" (digits and dashes in place); anything else is rejected. */
static int is_date(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? s[i] != '-' : (s[i] < '0' || s[i] > '9')) return 0;
    return 1;
}

/** Loads the ledger, dropping malformed or future-dated lines (they are not rewritten). */
static void read_ledger(const char *now) {
    FILE *f = fopen(LEDGER, "r");
    if (!f) return; /* first run: no ledger yet */
    Entry e;
    while (nledger < MAX_LEDGER && fscanf(f, "%10s %40s %ld", e.date, e.sha, &e.additions) == 3)
        if (is_date(e.date) && strcmp(e.date, now) <= 0 && strlen(e.sha) == 40 && e.additions >= 0)
            ledger[nledger++] = e;
    fclose(f);
}

static int in_ledger(const char *sha) {
    for (int i = 0; i < nledger; i++)
        if (!strcmp(ledger[i].sha, sha)) return 1;
    return 0;
}

static void commits_and_lines(void) {
    Baseline b = read_baseline();
    /* Committer dates are written in the committer's local timezone (up to UTC+14), so a
     * genuine commit can read as "tomorrow" in UTC. The latest acceptable date is therefore
     * tomorrow (UTC); anything later is spoofed or clock-skewed. */
    char now[11];
    utc_date(now, 1);
    read_ledger(now);
    /* Resume from the newest counted day (inclusive; dedupe handles overlap), else after the
     * baseline. Committer dates are author-controlled, so the resume day is clamped to that
     * limit and future-dated commits never enter the ledger: one fake "2099" commit must not
     * be able to push the cursor past every real commit. */
    const char *since = b.date, *op = ">";
    for (int i = 0; i < nledger; i++)
        if (is_date(ledger[i].date) && strcmp(ledger[i].date, now) <= 0 && strcmp(ledger[i].date, since) >= 0) {
            since = ledger[i].date;
            op = ">=";
        }
    /* Ledger dates are committer-local (up to a day ahead of UTC) but search filters in UTC,
     * so step the resume day back one day; already-ledgered SHAs are skipped anyway. */
    char resume[11];
    if (!strcmp(op, ">=")) {
        day_before(since, resume);
        since = resume;
    }

    JVal pages;
    if (!jparse_all(run("gh api -X GET search/commits --paginate -f per_page=100 "
                        "-f sort=committer-date -f order=asc "
                        "-f q='author:%s committer-date:%s%s -repo:%s/%s -repo:%s/%s'",
                        USER_LOGIN, op, safe(since), USER_LOGIN, EXCLUDED_REPOS[0],
                        USER_LOGIN, EXCLUDED_REPOS[1]), &pages))
        die("invalid JSON from commit search");

    int added = 0;
    for (size_t p = 0; p < pages.n && added < MAX_NEW_PER_RUN; p++) {
        const JVal *items = jget(jat(&pages, p), "items");
        for (size_t i = 0; items && i < items->n && added < MAX_NEW_PER_RUN; i++) {
            const JVal *it = jat(items, i);
            const char *sha = jstr(jget(it, "sha")), *repo = jstr(jpath(it, "repository.full_name"));
            const char *when = jstr(jpath(it, "commit.committer.date"));
            if (!sha || !repo || strlen(sha) != 40 || !is_date(when)) continue; /* malformed */
            if (strncmp(when, now, 10) > 0) continue; /* future-dated: spoofed or clock-skewed */
            if (in_ledger(sha)) continue;
            if (nledger == MAX_LEDGER) die("ledger full");
            JVal c;
            parse(run("gh api 'repos/%s/commits/%s'", safe(repo), safe(sha)), &c);
            Entry *e = &ledger[nledger++];
            memcpy(e->date, when, 10);
            e->date[10] = '\0';
            memcpy(e->sha, sha, 41);
            e->additions = 0;
            const JVal *files = jget(&c, "files");
            for (size_t f = 0; files && f < files->n; f++) {
                const JVal *file = jat(files, f);
                if (!skipped_file(jstr(jget(file, "filename")))) e->additions += (long)jnum(jget(file, "additions"), 0);
            }
            added++;
        }
    }
    if (added == MAX_NEW_PER_RUN) fprintf(stderr, "fetch: %d new commits capped; rest next run\n", added);

    FILE *f = fopen(LEDGER, "w");
    if (!f) die("cannot write " LEDGER);
    long lines = 0;
    for (int i = 0; i < nledger; i++) {
        fprintf(f, "%s %s %ld\n", ledger[i].date, ledger[i].sha, ledger[i].additions);
        lines += ledger[i].additions;
    }
    fclose(f);
    printf("commits %ld\nlines %ld\n", b.commits + nledger, b.lines + lines);
}

/* ------------------------------------------------------------------ ai (frozen snapshot) */
static void models(void) {
    FILE *f = fopen("data/ai.json", "r");
    if (!f) die("cannot open data/ai.json");
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    JVal v;
    parse(buf, &v);
    for (size_t i = 0; i < v.n; i++) {
        const JVal *m = jat(&v, i), *tok = jget(m, "tokens");
        printf("model %s %s %.0f\n", jstr(jget(m, "maker")), jstr(jget(m, "family")),
               tok && tok->type == J_NUM ? tok->num : -1.0);
    }
}

/* ------------------------------------------------------------------ --rebaseline */
/** Exact lifetime commits (owner's login sees everything); lines carry forward from data.txt. */
static void rebaseline(void) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    long commits = 0;
    for (int year = FIRST_YEAR; year <= tm.tm_year + 1900; year++) {
        JVal v;
        parse(run("gh api graphql -f query='{ user(login:\"%s\"){ contributionsCollection("
                  "from:\"%d-01-01T00:00:00Z\", to:\"%d-12-31T23:59:59Z\"){ commitContributionsByRepository("
                  "maxRepositories:100){ repository{ name owner{ login } } contributions{ totalCount } } } } }'",
                  USER_LOGIN, year, year), &v);
        const JVal *repos = jpath(&v, "data.user.contributionsCollection.commitContributionsByRepository");
        for (size_t i = 0; repos && i < repos->n; i++) {
            const JVal *r = jat(repos, i);
            const char *rn = jstr(jpath(r, "repository.name")), *ro = jstr(jpath(r, "repository.owner.login"));
            int generated = rn && ro && is_excluded(rn) && !strcmp(ro, USER_LOGIN);
            if (!generated) commits += (long)jnum(jpath(r, "contributions.totalCount"), 0);
        }
    }
    long lines = -1;
    FILE *d = fopen("data/data.txt", "r");
    /* Line by line: pkg records ("pkg NAME ...") would derail a single fscanf pattern. */
    static char line[1 << 16];
    long val;
    while (d && fgets(line, sizeof line, d))
        if (sscanf(line, "lines %ld", &val) == 1) lines = val;
    if (d) fclose(d);
    if (lines < 0) die("no lines record in data/data.txt to carry forward");
    char day[11];
    today(day);
    FILE *out = fopen("data/baseline.txt", "w");
    if (!out) die("cannot write data/baseline.txt");
    fprintf(out, "baseline_date %s\ncommits %ld\nlines %ld\n", day, commits, lines);
    fclose(out);
    /* The new baseline already includes every ledgered commit, so start the ledger fresh. */
    FILE *l = fopen(LEDGER, "w");
    if (!l) die("cannot reset " LEDGER);
    fclose(l);
    printf("baseline_date %s\ncommits %ld\nlines %ld\n", day, commits, lines);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--rebaseline")) { rebaseline(); return 0; }
    if (argc != 1) { fprintf(stderr, "usage: %s [--rebaseline]\n", argv[0]); return 2; }
    npm();
    repos();
    commits_and_lines();
    models();
    return 0;
}
