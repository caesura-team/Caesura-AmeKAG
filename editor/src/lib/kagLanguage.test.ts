// Editor KAG language-service deep tests for kagLanguage.ts — KAG Neo-Genesis
// Monaco language definition. Covers layers beyond the earlier smoke tests:
//
//   1. Monarch TOKEN CLASSIFICATION — a faithful mini-harness mirrors Monaco's
//      matching algorithm over the exact tokenizer config that
//      registerKagLanguage() installs, so we can assert how real tags /
//      labels / comments / strings / interpolation / blocktext / params tokenize.
//      (Monaco itself is unavailable in jsdom, so we drive the installed config.)
//   2. RE-EXPORT contract — KAG_COMMANDS is KNOWN_COMMANDS (same reference) and
//      the keyword array Monaco is handed is that same array (consistency guard).
//   3. IDEMPOTENT registration under a Monaco mock.
//   4. HIGHLIGHTER-vs-SCHEMA consistency — the KNOWN_COMMANDS keyword set covers
//      every declarative command contract in the generated
//      docs/api/command-contracts.md (auto-generated from kag/schema.lua).
//
// Note: actual completion items are produced engine-side by scripts/kag/lsp.lua
// (driven by the schema registry); the editor's local surface is this tokenizer +
// the KAG_COMMANDS keyword set. So command-completion coverage here is the
// exhaustive-tag test that proves every known command highlights as a valid tag.
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { readFileSync } from 'node:fs'
import * as monaco from 'monaco-editor'
import { KNOWN_COMMANDS } from '../lib/commandLint'
import { KAG_COMMANDS, registerKagLanguage } from '../ide/kagLanguage'

// ---- Minimal Monaco facade capturing what registerKagLanguage installs. ----
vi.mock('monaco-editor', () => ({
  languages: {
    getLanguages: vi.fn(() => []),
    register: vi.fn(),
    setMonarchTokensProvider: vi.fn(),
    setLanguageConfiguration: vi.fn(),
  },
}))

const langs = monaco.languages as any
const configs: any[] = []
const langConfigs: any[] = []

function installMonarch() {
  configs.length = 0
  langConfigs.length = 0
  langs.getLanguages.mockReturnValue([])
  langs.setMonarchTokensProvider.mockImplementation((_id: string, cfg: any) => {
    configs.push(cfg)
  })
  langs.setLanguageConfiguration.mockImplementation((_id: string, cfg: any) => {
    langConfigs.push(cfg)
  })
}

beforeEach(() => {
  vi.clearAllMocks()
  installMonarch()
})

// ============================================================================
// A faithful-enough Monarch tokenizer. Monaco's real algorithm (monarchLexer.js)
// compiles each rule regex to ^(?:<src>) and, at the current position in a line,
// matches against line.slice(pos), taking the FIRST rule that matches. A rule whose
// regex starts with ^ is only considered at pos 0. Actions: a string token name, a
// group array (one token per capture group), or an object with { cases } / { token,
// next }. next: '@pop' pops a state; any other value pushes that (named) state.
// ============================================================================
interface Span {
  start: number
  end: number
  token: string
}

// Resolve a Monarch { cases } action. This tokenizer uses two guards:
//   '@default'    -> always
//   '$1@keywords' -> capture group 1 is a known command name
function valueToToken(v: unknown): string {
  if (typeof v === 'string') return v;
  if (v && typeof v === 'object') return (v as { token?: string }).token ?? '';
  return '';
}

function resolveTagCases(cases: Record<string, unknown>, match: RegExpMatchArray, keywords: readonly string[]): string {
  const group1 = match[1] ?? ''
  const set = new Set(keywords)
  for (const key of Object.keys(cases)) {
    if (key === '@default' || key === '@' || key === '') {
      return valueToToken(cases[key]);
    }
    if (key === '$1@keywords' && set.has(group1)) {
      return valueToToken(cases[key]);
    }
  }
  return '';
}

// Tokenize a source using the config captured from registerKagLanguage().
function tokenizeSource(source: string, cfg: any): Span[] {
  const tokens: Span[] = [];
  const stack: string[] = []; // empty = root
  const stateNames = cfg.tokenizer as Record<string, unknown>;
  let lineOffset = 0;
  for (const rawLine of source.split('\n')) {
    const line = rawLine;
    let pos = 0;
    while (pos < line.length) {
      const stateName = stack.length === 0 ? 'root' : stack[stack.length - 1];
      const rules = (stateNames[stateName] as unknown as Array<[RegExp, unknown]>) ?? [];
      const rest = line.slice(pos);
      let applied = false;
      for (const [ruleRe, actionRaw] of rules) {
        const src = (ruleRe as RegExp).source;
        const atLineStart = src.length > 0 && src[0] === '^';
        if (atLineStart && pos !== 0) continue;
        const body = atLineStart ? src.slice(1) : src;
        const re = new RegExp('^(?:' + body + ')', '');
        const m = rest.match(re);
        if (!m) continue;
        const matched = m[0].length;
        const action = actionRaw as unknown;
        if (typeof action === 'string') {
          tokens.push({ start: lineOffset + pos, end: lineOffset + pos + matched, token: action });
          pos += matched;
        } else if (Array.isArray(action)) {
          let off = pos;
          for (let g = 1; g < m.length; g++) {
            const text = m[g] ?? '';
            const gact = action[g - 1] as unknown;
            const gname = typeof gact === 'string' ? gact : valueToToken(gact);
            tokens.push({ start: lineOffset + off, end: lineOffset + off + text.length, token: gname });
            off += text.length;
          }
          pos += matched;
        } else if (action && typeof action === 'object' && !Array.isArray(action)) {
          const obj = action as { cases?: Record<string, unknown>; token?: string; next?: string };
          let tname: string;
          if (obj.cases) {
            tname = resolveTagCases(obj.cases, m, cfg.keywords as readonly string[]);
          } else {
            tname = obj.token ?? '';
          }
          tokens.push({ start: lineOffset + pos, end: lineOffset + pos + matched, token: tname });
          const next = obj.next;
          if (next === '@pop') {
            stack.pop();
          } else if (typeof next === 'string' && next.length > 0) {
            stack.push(next.startsWith('@') ? next.slice(1) : next);
          }
          pos += matched;
        } else {
          pos += matched;
        }
        applied = true;
        break;
      }
      if (!applied) pos += 1;
    }
    lineOffset += line.length + 1;
  }
  return tokens;
}

function tokenize(src: string): Span[] {
  registerKagLanguage();
  return tokenizeSource(src, configs[0]);
}


// ============================================================================
// 1. TOKENIZER CLASSIFICATION
// ============================================================================
describe('Monarch tokenizer classification', () => {
  it('tags a known command and marks an unknown one tag.invalid', () => {
    const known = tokenize('[bg]\n');
    expect(known[0]).toMatchObject({ token: 'tag', start: 0, end: 3 }); // '[bg'
    expect(known[1]).toMatchObject({ token: 'delimiter.bracket', start: 3, end: 4 }); // ']'
    const ch = tokenize('[ch]\n');
    expect(ch[0].token).toBe('tag');
    const unknown = tokenize('[notarealcommand]\n');
    expect(unknown[0].token).toBe('tag.invalid');
  });

  it('classifies every KAG_COMMANDS entry as a valid tag (exhaustive)', () => {
    for (const cmd of KAG_COMMANDS) {
      const toks = tokenize('[' + cmd + ']\n');
      expect(toks[0]?.token, '[' + cmd + '] should be tag, got ' + toks[0]?.token).toBe('tag');
    }
  });

  it('classifies labels, comments, and quoted strings', () => {
    const toks = tokenize('*start\n; a comment\n"hello" \'single\'\n');
    expect(toks[0].token).toBe('type.identifier'); // *start
    expect(toks.some((t) => t.token === 'comment')).toBe(true);
    expect(toks.some((t) => t.token === 'string')).toBe(true);
  });

  it('parses named params into attribute.name / operator and quoted values as string', () => {
    const toks = tokenize('[ch name="Hero" text="Hi"]\n');
    const attrs = toks.filter((t) => t.token === 'attribute.name');
    const ops = toks.filter((t) => t.token === 'operator');
    const strs = toks.filter((t) => t.token === 'string');
    expect(attrs.length).toBe(2);
    expect(ops.length).toBe(2);
    expect(strs.length).toBe(2);
  });

  it('highlights numbers and multi-param numeric commands', () => {
    const toks = tokenize('[position x=10 y=20.5 layer="bg"]\n');
    expect(toks.some((t) => t.token === 'number')).toBe(true);
    expect(toks.filter((t) => t.token === 'attribute.name').length).toBe(3);
  });

  it('highlights ${...} interpolation spans as variable (expr state, pops on })', () => {
    // unquoted interpolation: ${ enters the @expr state, the body and the closing
    // } both tokenize as variable
    const toks = tokenize('[eval exp=${f.hp + 1}]\n');
    const vars = toks.filter((t) => t.token === 'variable');
    expect(vars.length).toBeGreaterThanOrEqual(3); // '${', body, '}'
  });

  it('absorbs ${...} inside a double-quoted param value into the string token', () => {
    // a quoted param value is consumed whole by the string rule (correct Monarch
    // ordering: the string rule precedes the interpolation rule in root)
    const toks = tokenize('[ch text="Hi ${name}"]\n');
    const strs = toks.filter((t) => t.token === 'string');
    expect(strs.length).toBe(1);
    expect(strs[0].end - strs[0].start).toBe('"Hi ${name}"'.length);
  });

  it('highlights plain text content and whitespace', () => {
    const toks = tokenize('Hello world\n');
    expect(toks.some((t) => t.token === 'text')).toBe(true);
    expect(toks.some((t) => t.token === 'white')).toBe(true);
  });

  it('tokenizes blocktext """ across lines as strings (state stays in @blocktext)', () => {
    const toks = tokenize('"""\nline one\nline two\n"""\n');
    expect(toks[0].token).toBe('string'); // opening """
    expect(toks.every((t) => t.token === 'string')).toBe(true);
  });

  it('does not misclassify a label mid-line (labels are line-start anchored)', () => {
    const toks = tokenize('text *notalabel\n');
    expect(toks.every((t) => t.token !== 'type.identifier')).toBe(true);
  });
});

// ============================================================================
// 2. RE-EXPORT CONTRACT — KAG_COMMANDS === KNOWN_COMMANDS (same reference).
// ============================================================================
describe('KAG_COMMANDS re-export contract', () => {
  it('re-exports KNOWN_COMMANDS from commandLint under the same reference', () => {
    expect(KAG_COMMANDS).toBe(KNOWN_COMMANDS);
  });

  it('hands Monaco the exact exported KAG_COMMANDS array as its keyword set', () => {
    registerKagLanguage();
    expect(configs[0].keywords).toBe(KAG_COMMANDS);
    expect(configs[0].keywords).toBe(KNOWN_COMMANDS);
  });

  it('contains no duplicate command names', () => {
    const seen = new Set<string>();
    for (const cmd of KAG_COMMANDS) {
      expect(seen.has(cmd), 'duplicate command: ' + cmd).toBe(false);
      seen.add(cmd);
    }
  });
});

// ============================================================================
// 3. IDEMPOTENT registration under a Monaco mock.
// ============================================================================
describe('registerKagLanguage (Monaco wiring & idempotency)', () => {
  it('installs the kag language once with the .ks extension', () => {
    registerKagLanguage();
    expect(langs.register).toHaveBeenCalledTimes(1);
    expect(langs.register).toHaveBeenCalledWith({ id: 'kag', extensions: ['.ks'] });
    expect(configs).toHaveLength(1);
    expect(langConfigs).toHaveLength(1);
    expect(langs.setMonarchTokensProvider.mock.calls[0][0]).toBe('kag');
  });

  it('skips re-registration when kag is already registered (idempotent)', () => {
    langs.getLanguages.mockReturnValue([{ id: 'kag' }]);
    registerKagLanguage();
    expect(langs.register).not.toHaveBeenCalled();
    expect(configs).toHaveLength(0);
    expect(langConfigs).toHaveLength(0);
  });

  it('installs the expected language config (; comment, bracket pairs)', () => {
    registerKagLanguage();
    expect(langConfigs[0].comments).toEqual({ lineComment: ';' });
    expect(langConfigs[0].brackets).toEqual([['[', ']'], ['{', '}']]);
  });
});

// ============================================================================
// 4. HIGHLIGHTER-vs-SCHEMA consistency — every command in the generated
//    docs/api/command-contracts.md must live in KNOWN_COMMANDS so its [cmd] tag highlights
//    instead of falling to tag.invalid.
// ============================================================================
const contractDocument = readFileSync(new URL('../../../docs/api/command-contracts.md', import.meta.url), 'utf8')
const schemaCommands = [...contractDocument.matchAll(/^### `\[([a-z0-9_]+)\]`/gm)].map((match) => match[1])

describe('KNOWN_COMMANDS vs generated schema contracts', () => {
  it('covers at least the schema contract count', () => {
    expect(schemaCommands.length).toBeGreaterThan(0);
    expect(KAG_COMMANDS.length).toBeGreaterThanOrEqual(schemaCommands.length);
  });

  it('covers every declarative command contract', () => {
    const set = new Set(KAG_COMMANDS);
    for (const cmd of schemaCommands) {
      expect(set.has(cmd), 'KNOWN_COMMANDS is missing schema contract: ' + cmd).toBe(true);
    }
  });

  it('tokens every schema contract command as a valid tag (not tag.invalid)', () => {
    for (const cmd of schemaCommands) {
      const toks = tokenize('[' + cmd + ']\n');
      expect(toks[0]?.token, '[' + cmd + '] should be tag, got ' + toks[0]?.token).toBe('tag');
    }
  });
});

