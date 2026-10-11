import type { ReactNode } from 'react';
import type { ApiError, StatementResult, TimelineStage } from '../types/api';
import { fmtMs } from '../visualizations/format';

export function EmptyState({ title, children, action }: { title: string; children?: ReactNode; action?: ReactNode }) {
  return (
    <div className="h-full flex items-center justify-center p-8">
      <div className="max-w-md text-center">
        <p className="text-lg text-chalk">{title}</p>
        {children && <div className="mt-2 text-sm text-haze">{children}</div>}
        {action && <div className="mt-4">{action}</div>}
      </div>
    </div>
  );
}

export function ErrorBox({ error, sql }: { error: ApiError; sql?: string }) {
  // Show the offending line with a caret under the error column.
  let excerpt: ReactNode = null;
  if (sql && error.span) {
    const lineStart = sql.lastIndexOf('\n', Math.max(0, error.span.begin - 1)) + 1;
    const lineEnd = sql.indexOf('\n', error.span.begin);
    const line = sql.slice(lineStart, lineEnd === -1 ? undefined : lineEnd);
    const col = error.span.begin - lineStart;
    const width = Math.max(1, Math.min(error.span.end, lineEnd === -1 ? sql.length : lineEnd) - error.span.begin);
    excerpt = (
      <pre className="code mt-2 text-chalk/90 overflow-x-auto">
        {line}
        {'\n'}
        <span className="text-coral">{' '.repeat(Math.max(0, col)) + '^'.repeat(width)}</span>
      </pre>
    );
  }
  return (
    <div className="rounded-md border border-coral/50 bg-coral/10 px-3 py-2" role="alert">
      <div className="text-sm">
        <span className="text-coral font-medium">{stageTitle(error.stage)} error</span>
        {error.span && (
          <span className="text-haze">
            {' '}
            at line {error.span.line}, column {error.span.column}
          </span>
        )}
      </div>
      <div className="text-sm text-chalk mt-0.5">{error.message}</div>
      {excerpt}
    </div>
  );
}

export function stageTitle(stage: string): string {
  const map: Record<string, string> = {
    lexer: 'Lexer',
    parser: 'Syntax',
    binder: 'Name resolution',
    planner: 'Planning',
    optimizer: 'Optimizer',
    executor: 'Execution',
    storage: 'Storage',
    catalog: 'Catalog',
    api: 'Request',
    internal: 'Internal',
  };
  return map[stage] ?? stage;
}

const STAGES: { key: string; name: string; hint: string }[] = [
  { key: 'lexer', name: 'Lexer', hint: 'SQL text to tokens' },
  { key: 'parser', name: 'Parser', hint: 'tokens to syntax tree' },
  { key: 'binder', name: 'Binder', hint: 'resolve tables, columns, types' },
  { key: 'optimizer', name: 'Optimizer', hint: 'rewrite the logical plan' },
  { key: 'planner', name: 'Planner', hint: 'choose physical operators' },
  { key: 'executor', name: 'Executor', hint: 'run the operators' },
];

/** The compiler pipeline as a strip: what ran, how long, where it stopped. */
export function PipelineTimeline({ result }: { result: StatementResult | null }) {
  const byStage = new Map<string, TimelineStage>();
  for (const s of result?.timeline ?? []) byStage.set(s.stage, s);
  const failedStage = result && !result.success ? result.error?.stage : undefined;
  const failedIdx = STAGES.findIndex((s) => s.key === failedStage || (failedStage === 'catalog' && s.key === 'executor'));
  return (
    <ol className="flex items-stretch gap-px bg-rule rounded overflow-hidden text-xs" aria-label="Query pipeline">
      {STAGES.map((s, i) => {
        const t = byStage.get(s.key);
        const failed = failedIdx === i;
        const skipped = !t && !failed;
        return (
          <li
            key={s.key}
            className={`flex-1 px-2.5 py-1.5 ${failed ? 'bg-coral/20' : skipped ? 'bg-well' : 'bg-slab'}`}
            title={s.hint}
          >
            <div className={failed ? 'text-coral' : skipped ? 'text-haze/50' : 'text-chalk'}>{s.name}</div>
            <div className="text-haze">{failed ? 'failed here' : t ? fmtMs(t.ms) : result ? 'not needed' : s.hint}</div>
          </li>
        );
      })}
    </ol>
  );
}

export function Stat({ label, value, hint }: { label: string; value: ReactNode; hint?: string }) {
  return (
    <div title={hint}>
      <div className="label">{label}</div>
      <div className="text-chalk text-sm">{value}</div>
    </div>
  );
}

export function Toggle({ checked, onChange, label, hint }: { checked: boolean; onChange: (v: boolean) => void; label: string; hint?: string }) {
  return (
    <label className="inline-flex items-center gap-2 text-sm text-haze cursor-pointer select-none" title={hint}>
      <input type="checkbox" className="accent-[#5FB8E8] h-3.5 w-3.5" checked={checked} onChange={(e) => onChange(e.target.checked)} />
      {label}
    </label>
  );
}

export function Segmented<T extends string>({ value, options, onChange }: { value: T; options: { value: T; label: string }[]; onChange: (v: T) => void }) {
  return (
    <div className="inline-flex rounded border border-rule overflow-hidden" role="tablist">
      {options.map((o) => (
        <button
          key={o.value}
          role="tab"
          aria-selected={o.value === value}
          className={`px-3 py-1 text-sm ${o.value === value ? 'bg-[#26324A] text-chalk' : 'bg-transparent text-haze hover:text-chalk'}`}
          onClick={() => onChange(o.value)}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}
