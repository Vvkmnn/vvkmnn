/*
 * ai.c — OPTIONAL, Mac-only: rebuilds data/ai.json, every AI model with evidence of use.
 *
 * The ai column on the card is a frozen snapshot (2026-11-02: 42 models, 9 makers) because
 * its sources exist only on the Mac, never on GitHub's runners, and the owner chose no
 * maintenance (no local scheduled job). To refresh by hand, from the repo root:
 *
 *   cc -std=c11 -O2 -Wall -Wextra -o ai scripts/ai.c && ./ai && git commit data/ai.json
 *
 * Sources (read-only):
 *   ~/.claude/stats-cache.json   Claude Code: tokens per model (input+output+cache)
 *   ~/.codex/sessions (*.jsonl) Codex: each session's final total_tokens, credited to its
 *                                most-used model
 *   llm logs.db (via sqlite3)    `llm` CLI: input+output tokens per model
 *   LOCAL                        models run locally (Hugging Face cache); no token logs
 *   CODE                         models referenced in the owner's own repos (GitHub code
 *                                search, some private, so repo names stay out); no tokens
 * Pure C: the only external tool is the sqlite3 CLI (ships with macOS).
 */
#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

enum { MAX_MODELS = 256 };

typedef struct { char id[96]; char maker[32]; double tokens; } Model; /* tokens < 0: unknown */
static Model models[MAX_MODELS];
static int nmodels;

static void die(const char *msg) { fprintf(stderr, "ai: %s\n", msg); exit(1); }

/** Adds a model unless its id is already present (log sources come first, so they win). */
static void add(const char *id, const char *maker, double tokens) {
    for (int i = 0; i < nmodels; i++)
        if (!strcmp(models[i].id, id)) return;
    if (nmodels == MAX_MODELS) die("too many models");
    Model *m = &models[nmodels++];
    snprintf(m->id, sizeof m->id, "%s", id);
    snprintf(m->maker, sizeof m->maker, "%s", maker);
    m->tokens = tokens;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = jalloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ Claude Code */
static void claude(const char *home) {
    char path[1024];
    snprintf(path, sizeof path, "%s/.claude/stats-cache.json", home);
    char *text = slurp(path);
    if (!text) { fprintf(stderr, "ai: no %s, skipping Claude Code\n", path); return; }
    JVal v;
    if (!jparse(text, &v)) die("invalid stats-cache.json");
    const JVal *usage = jget(&v, "modelUsage");
    const char *fields[] = { "inputTokens", "outputTokens", "cacheReadInputTokens", "cacheCreationInputTokens" };
    for (size_t i = 0; usage && i < usage->n; i++) {
        double t = 0;
        for (size_t f = 0; f < 4; f++) t += jnum(jget(&usage->items[i], fields[f]), 0);
        add(usage->keys[i], "anthropic", t);
    }
}

/* ------------------------------------------------------------------ Codex */
typedef struct { char id[96]; double tokens; } Tally;
static Tally codex_totals[64];
static int ncodex;

/** One session file: most-used "model" string gets the session's final total_tokens. */
static int codex_file(const char *path, const struct stat *st, int kind, struct FTW *ftw) {
    (void)st; (void)ftw;
    size_t len = strlen(path);
    if (kind != FTW_F || len < 6 || strcmp(path + len - 6, ".jsonl")) return 0;
    char *text = slurp(path);
    if (!text) return 0;
    char ids[32][96];
    int counts[32] = { 0 }, nids = 0;
    for (const char *p = text; (p = strstr(p, "\"model\":\"")); ) {
        p += 9;
        size_t n = strcspn(p, "\"");
        if (n == 0 || n >= sizeof ids[0]) continue;
        int k = 0;
        while (k < nids && (strncmp(ids[k], p, n) || ids[k][n])) k++;
        if (k == nids && nids < 32) { memcpy(ids[k], p, n); ids[k][n] = '\0'; nids++; }
        if (k < nids) counts[k]++;
    }
    double last = 0;
    for (const char *p = text; (p = strstr(p, "\"total_token_usage\":{")); ) {
        const char *end = strchr(p, '}'), *t = strstr(p, "\"total_tokens\":");
        if (t && end && t < end) last = strtod(t + 15, NULL);
        p += 21;
    }
    free(text);
    if (!nids) return 0;
    int best = 0;
    for (int k = 1; k < nids; k++) if (counts[k] > counts[best]) best = k;
    int k = 0;
    while (k < ncodex && strcmp(codex_totals[k].id, ids[best])) k++;
    if (k == ncodex) {
        if (ncodex == 64) return 0;
        snprintf(codex_totals[ncodex++].id, sizeof codex_totals[k].id, "%s", ids[best]);
    }
    codex_totals[k].tokens += last;
    return 0;
}

static void codex(const char *home) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.codex/sessions", home);
    if (nftw(dir, codex_file, 16, FTW_PHYS) != 0) { fprintf(stderr, "ai: no %s, skipping Codex\n", dir); return; }
    for (int k = 0; k < ncodex; k++) add(codex_totals[k].id, "openai", codex_totals[k].tokens);
}

/* ------------------------------------------------------------------ llm CLI */
static void llm(const char *home) {
    if (strchr(home, '\'')) die("HOME contains a quote");
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "sqlite3 -readonly -separator '|' '%s/Library/Application Support/io.datasette.llm/logs.db' "
             "'select model, sum(coalesce(input_tokens,0) + coalesce(output_tokens,0)) from responses group by model;' 2>/dev/null",
             home);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char line[512];
    while (fgets(line, sizeof line, p)) {
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = '\0';
        double tokens = strtod(bar + 1, NULL);
        /* "openrouter/nvidia/nemotron-3-ultra-550b-a55b:free" -> maker nvidia, id nemotron-... */
        char *maker = strchr(line, '/'), *id = strrchr(line, '/');
        if (!maker || !id) continue;
        maker++;
        char mk[32];
        size_t mlen = strcspn(maker, "/");
        snprintf(mk, sizeof mk, "%.*s", (int)(mlen < sizeof mk ? mlen : sizeof mk - 1), maker);
        char *free_tag = strstr(id + 1, ":free");
        if (free_tag) *free_tag = '\0';
        add(id + 1, mk, tokens);
    }
    pclose(p);
}

/* ------------------------------------------------------------------ static evidence */
static const char *const LOCAL[][2] = { /* ~/.cache/huggingface/hub (MLX builds run locally) */
    { "whisper-large-v3-turbo", "openai" }, { "qwen3-4b-instruct", "alibaba" },
    { "soprano-80m", "alibaba" }, /* maker unverified; grouped with the other local row */
};
static const char *const CODE[][2] = {
    { "claude-3-sonnet", "anthropic" }, { "claude-haiku-3-5", "anthropic" },
    { "gpt-4-turbo", "openai" }, { "gpt-4o", "openai" }, { "gpt-4o-mini", "openai" },
    { "gpt-3.5-turbo", "openai" }, { "gemini-2.5-flash", "google" }, { "inception-v3", "google" },
    { "xception", "google" }, { "llama-3.3-70b", "meta" }, { "deepseek-chat", "deepseek" },
    { "deepseek-r1", "deepseek" }, { "qwen3-coder", "alibaba" }, { "qwen3-8b", "alibaba" },
    { "qwen3.5-9b", "alibaba" }, { "vgg16", "oxford" }, { "vgg19", "oxford" }, { "resnet50", "microsoft" },
};

/** Family = first matching substring (gpt must be a prefix). Order matters: checked top-down. */
static const char *family(const char *id) {
    static const char *const FAMILIES[] = { "opus", "sonnet", "fable", "haiku", "whisper", "gpt", "nemotron",
                                            "gemma", "qwen", "soprano", "gemini", "llama", "deepseek", "vgg",
                                            "resnet", "inception", "xception" };
    for (size_t i = 0; i < sizeof FAMILIES / sizeof *FAMILIES; i++) {
        const char *f = FAMILIES[i];
        int hit = !strcmp(f, "gpt") ? !strncmp(id, "gpt", 3) : strstr(id, f) != NULL;
        if (hit) return !strcmp(f, "xception") ? "inception" : f;
    }
    return "other";
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home) die("HOME is not set");
    claude(home);
    codex(home);
    llm(home);
    for (size_t i = 0; i < sizeof LOCAL / sizeof *LOCAL; i++) add(LOCAL[i][0], LOCAL[i][1], -1);
    for (size_t i = 0; i < sizeof CODE / sizeof *CODE; i++) add(CODE[i][0], CODE[i][1], -1);

    FILE *out = fopen("data/ai.json", "w");
    if (!out) die("cannot write data/ai.json (run from the repo root)");
    fputs("[\n", out);
    for (int i = 0; i < nmodels; i++) {
        const Model *m = &models[i];
        fprintf(out, " {\"id\": \"%s\", \"maker\": \"%s\", \"tokens\": ", m->id, m->maker);
        if (m->tokens < 0) fputs("null", out); else fprintf(out, "%.0f", m->tokens);
        fprintf(out, ", \"family\": \"%s\"}%s\n", family(m->id), i + 1 < nmodels ? "," : "");
    }
    fputs("]\n", out);
    fclose(out);
    printf("ai: %d models -> data/ai.json\n", nmodels);
    return 0;
}
