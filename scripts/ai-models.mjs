// OPTIONAL: rebuilds data/ai.json — every AI model with local evidence of use. Not automated.
//
// The ai column is a frozen snapshot (2026-11-02: 42 models, 9 makers) because its sources
// only exist on the Mac, never on GitHub's runners:
//   ~/.claude/stats-cache.json (Claude Code tokens per model), ~/.codex/sessions (Codex),
//   the `llm` CLI's logs.db, and models found in ~/.cache/huggingface.
// The CODE list below was found by searching the user's own repos for model identifiers.
// To refresh: `node scripts/ai-models.mjs` on the Mac, then commit + push data/ai.json;
// the daily Action picks it up on its next run. Automating it would need a local
// scheduled job (launchd), which was deliberately not installed (owner: no maintenance).
import { execFileSync } from 'node:child_process';
import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const HOME = homedir();

/** Claude Code: tokens = input + output + cache read + cache creation, per model. */
function claudeModels() {
  const { modelUsage } = JSON.parse(readFileSync(join(HOME, '.claude/stats-cache.json'), 'utf8'));
  return Object.entries(modelUsage).map(([id, u]) => ({
    id, maker: 'anthropic',
    tokens: u.inputTokens + u.outputTokens + u.cacheReadInputTokens + u.cacheCreationInputTokens,
  }));
}

const walk = (dir) => readdirSync(dir).flatMap((name) => {
  const path = join(dir, name);
  return statSync(path).isDirectory() ? walk(path) : [path];
});

/** Codex: each session's final cumulative total_tokens, credited to its most-used model. */
function codexModels() {
  const totals = new Map();
  for (const file of walk(join(HOME, '.codex/sessions')).filter((f) => f.endsWith('.jsonl'))) {
    const text = readFileSync(file, 'utf8');
    const models = [...text.matchAll(/"model":"([^"]+)"/g)].map((m) => m[1]);
    if (models.length === 0) continue;
    const counts = models.reduce((m, id) => m.set(id, (m.get(id) ?? 0) + 1), new Map());
    const main = [...counts].sort((a, b) => b[1] - a[1])[0][0];
    const usage = [...text.matchAll(/"total_token_usage":\{[^}]*"total_tokens":(\d+)/g)].map((m) => Number(m[1]));
    totals.set(main, (totals.get(main) ?? 0) + (usage.at(-1) ?? 0));
  }
  return [...totals].map(([id, tokens]) => ({ id, maker: 'openai', tokens }));
}

/** `llm` CLI: input + output tokens per model from its SQLite log. */
function llmModels() {
  const db = join(HOME, 'Library/Application Support/io.datasette.llm/logs.db');
  const rows = execFileSync('sqlite3', ['-readonly', '-separator', '|', db,
    'select model, sum(coalesce(input_tokens,0) + coalesce(output_tokens,0)) from responses group by model;'],
  { encoding: 'utf8' }).trim().split('\n').filter(Boolean);
  const MAKERS = { nvidia: 'nvidia', google: 'google' };
  return rows.map((row) => {
    const [model, tokens] = row.split('|');
    const id = model.split('/').at(-1).replace(/:free$/, '');
    const maker = MAKERS[model.split('/')[1]] ?? model.split('/')[1];
    return { id, maker, tokens: Number(tokens) };
  });
}

// Found in ~/.cache/huggingface/hub (MLX builds run locally); no token logs exist for them.
const LOCAL = [
  { id: 'whisper-large-v3-turbo', maker: 'openai', tokens: null },
  { id: 'qwen3-4b-instruct', maker: 'alibaba', tokens: null },
  { id: 'soprano-80m', maker: 'alibaba', tokens: null }, // maker unverified; grouped with the other local TTS/LLM row
];

// Referenced in his own code (GitHub code search; some repos private, so repo names stay out).
// No token logs: shown at the lightest shade. Unverified mentions are excluded.
const CODE = [
  { id: 'claude-3-sonnet', maker: 'anthropic' }, { id: 'claude-haiku-3-5', maker: 'anthropic' },
  { id: 'gpt-4-turbo', maker: 'openai' }, { id: 'gpt-4o', maker: 'openai' },
  { id: 'gpt-4o-mini', maker: 'openai' }, { id: 'gpt-3.5-turbo', maker: 'openai' },
  { id: 'gemini-2.5-flash', maker: 'google' }, { id: 'inception-v3', maker: 'google' },
  { id: 'xception', maker: 'google' },
  { id: 'llama-3.3-70b', maker: 'meta' },
  { id: 'deepseek-chat', maker: 'deepseek' }, { id: 'deepseek-r1', maker: 'deepseek' },
  { id: 'qwen3-coder', maker: 'alibaba' }, { id: 'qwen3-8b', maker: 'alibaba' },
  { id: 'qwen3.5-9b', maker: 'alibaba' },
  { id: 'vgg16', maker: 'oxford' }, { id: 'vgg19', maker: 'oxford' },
  { id: 'resnet50', maker: 'microsoft' },
].map((m) => ({ ...m, tokens: null }));

const FAMILIES = [
  [/opus/, 'opus'], [/sonnet/, 'sonnet'], [/fable/, 'fable'], [/haiku/, 'haiku'],
  [/whisper/, 'whisper'], [/^gpt/, 'gpt'], [/nemotron/, 'nemotron'], [/gemma/, 'gemma'],
  [/qwen/, 'qwen'], [/soprano/, 'soprano'], [/gemini/, 'gemini'], [/llama/, 'llama'],
  [/deepseek/, 'deepseek'], [/vgg/, 'vgg'], [/resnet/, 'resnet'], [/inception|xception/, 'inception'],
];
const family = (id) => FAMILIES.find(([re]) => re.test(id))?.[1] ?? 'other';

// Dedupe by id: logs win over code mentions (they carry tokens).
const seen = new Set();
const models = [...claudeModels(), ...codexModels(), ...llmModels(), ...LOCAL, ...CODE]
  .filter((m) => !seen.has(m.id) && seen.add(m.id))
  .map((m) => ({ ...m, family: family(m.id) }));
writeFileSync(new URL('../data/ai.json', import.meta.url), JSON.stringify(models, null, 1));
const byMaker = models.reduce((m, x) => m.set(x.maker, [...(m.get(x.maker) ?? []), x]), new Map());
for (const [maker, list] of byMaker) {
  console.log(maker.padEnd(10), list.length, 'models:', list.map((x) => `${x.id}(${x.tokens ?? 'local'})`).join(', '));
}
console.log('total', models.length, 'models,', byMaker.size, 'makers');
