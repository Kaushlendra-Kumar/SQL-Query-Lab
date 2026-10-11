import { useEffect, useState } from 'react';
import { CartesianGrid, ResponsiveContainer, Scatter, ScatterChart, Tooltip, XAxis, YAxis, ZAxis } from 'recharts';
import { api } from '../api/client';
import { useStore } from '../store';
import { ErrorBox, Segmented, Toggle } from '../components/common';
import { PlanGraph } from '../visualizations/PlanGraph';
import type { ApiError, BenchmarkResult, BenchmarkVariant, HistoryEntry, PlanNode } from '../types/api';
import { fmtInt, fmtMs } from '../visualizations/format';
import { flatten } from '../visualizations/treeLayout';

interface VariantInput {
  label: string;
  sql: string;
  disableIndexScans: boolean;
  coldCache: boolean;
  disableHashJoin: boolean;
}

const COLORS = ['#5FB8E8', '#E9B44C'];

function usesIndex(plan: PlanNode | undefined): string | null {
  const scan = plan ? flatten(plan).find((n) => n.op === 'Index Scan') : undefined;
  return scan ? String(scan.details.index) : null;
}

/** Strip plot of every timed iteration, plus the median as a tick. */
function SampleChart({ variants }: { variants: BenchmarkVariant[] }) {
  const data = variants.map((v, vi) => v.samplesMs.map((ms, i) => ({ x: vi + 1 + ((i % 7) - 3) * 0.035, y: ms, run: i + 1, label: v.label })));
  const maxY = Math.max(...variants.flatMap((v) => v.samplesMs), 0.001);
  return (
    <div className="h-64">
      <ResponsiveContainer width="100%" height="100%">
        <ScatterChart margin={{ top: 10, right: 20, bottom: 10, left: 10 }}>
          <CartesianGrid stroke="#2A3448" vertical={false} />
          <XAxis
            type="number"
            dataKey="x"
            domain={[0.5, variants.length + 0.5]}
            ticks={variants.map((_, i) => i + 1)}
            tickFormatter={(t: number) => variants[t - 1]?.label ?? ''}
            stroke="#8D9AB0"
            tick={{ fontSize: 12 }}
          />
          <YAxis type="number" dataKey="y" domain={[0, maxY * 1.1]} stroke="#8D9AB0" tick={{ fontSize: 12 }} tickFormatter={(v: number) => fmtMs(v)} width={70} />
          <ZAxis range={[28, 28]} />
          <Tooltip
            cursor={false}
            contentStyle={{ background: '#1C2433', border: '1px solid #2A3448', fontSize: 12 }}
            formatter={(v: number, name: string) => (name === 'y' ? [fmtMs(v), 'time'] : [v, name])}
            labelFormatter={() => ''}
          />
          {data.map((d, i) => (
            <Scatter key={i} data={d} fill={COLORS[i % COLORS.length]} fillOpacity={0.75} isAnimationActive={false} />
          ))}
          {variants.map((v, i) => (
            <Scatter
              key={`m${i}`}
              data={[{ x: i + 1, y: v.stats.median }]}
              shape={(p: { cx?: number; cy?: number }) => <rect x={(p.cx ?? 0) - 26} y={(p.cy ?? 0) - 1.5} width={52} height={3} fill="#DDE4EF" />}
              isAnimationActive={false}
            />
          ))}
        </ScatterChart>
      </ResponsiveContainer>
    </div>
  );
}

function Comparison({ result }: { result: BenchmarkResult }) {
  const v = result.variants;
  // Times are only called "better" when the middle 50% of runs do not overlap.
  const timeBetter = (a: BenchmarkVariant, b: BenchmarkVariant) => (a.stats.p75 < b.stats.p25 ? -1 : b.stats.p75 < a.stats.p25 ? 1 : 0);
  const rows: { label: string; get: (x: BenchmarkVariant) => string; better?: (a: BenchmarkVariant, b: BenchmarkVariant) => number }[] = [
    { label: 'Median execution', get: (x) => fmtMs(x.stats.median), better: timeBetter },
    { label: 'Middle 50% (p25 to p75)', get: (x) => `${fmtMs(x.stats.p25)} to ${fmtMs(x.stats.p75)}` },
    { label: 'Min / max', get: (x) => `${fmtMs(x.stats.min)} / ${fmtMs(x.stats.max)}` },
    { label: 'Variation (CV)', get: (x) => `${(x.stats.cv * 100).toFixed(1)}%` },
    { label: 'Rows returned', get: (x) => fmtInt(x.rowsReturned) },
    { label: 'Rows examined', get: (x) => fmtInt(x.rowsScanned), better: (a, b) => a.rowsScanned - b.rowsScanned },
    { label: 'Page reads per run', get: (x) => fmtInt(x.pagesRead), better: (a, b) => a.pagesRead - b.pagesRead },
    { label: 'Pool misses per run', get: (x) => x.avgPageMisses.toFixed(1) },
    { label: 'Index entries examined', get: (x) => fmtInt(x.indexEntriesExamined) },
    { label: 'Index used', get: (x) => usesIndex(x.plan) ?? 'none (sequential scan)' },
    { label: 'Timed iterations', get: (x) => `${x.iterations} (+${x.warmup} warm-up)` },
  ];
  return (
    <table className="w-full text-sm">
      <thead>
        <tr className="text-left">
          <th className="font-normal text-haze py-1" />
          {v.map((x, i) => (
            <th key={i} className="font-medium py-1" style={{ color: COLORS[i % COLORS.length] }}>
              {x.label}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((r) => {
          /* highlighted cells are better by a margin larger than run-to-run noise (times) or exactly (counts) */
          let best = -1;
          if (r.better && v.length === 2) {
            const d = r.better(v[0], v[1]);
            if (Math.abs(d) > 1e-9) best = d < 0 ? 0 : 1;
          }
          return (
            <tr key={r.label} className="border-t border-rule/60">
              <td className="py-1 text-haze">{r.label}</td>
              {v.map((x, i) => (
                <td key={i} className={`py-1 ${best === i ? 'text-flow' : ''}`}>
                  {r.get(x)}
                </td>
              ))}
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

function ComparisonNote({ result }: { result: BenchmarkResult }) {
  const [a, b] = result.variants;
  if (!a || !b) return null;
  const overlap = !(a.stats.p75 < b.stats.p25 || b.stats.p75 < a.stats.p25);
  return (
    <p className="mt-3 text-xs text-haze">
      {overlap
        ? 'The middle 50% of run times overlap, so the timing difference is within noise on this machine. Compare the exact work counts instead, or increase the data size.'
        : 'The middle 50% of run times do not overlap, so the timing difference is larger than run-to-run noise here.'}
    </p>
  );
}

function VariantEditor({ v, onChange, color }: { v: VariantInput; onChange: (v: VariantInput) => void; color: string }) {
  return (
    <div className="panel p-3 space-y-2">
      <input className="input w-full font-medium" style={{ color }} value={v.label} onChange={(e) => onChange({ ...v, label: e.target.value })} aria-label="Variant name" />
      <textarea className="input w-full code h-28 resize-y" value={v.sql} onChange={(e) => onChange({ ...v, sql: e.target.value })} aria-label="SQL" spellCheck={false} />
      <div className="flex flex-wrap gap-4">
        <Toggle checked={v.disableIndexScans} onChange={(b) => onChange({ ...v, disableIndexScans: b })} label="No index scans" />
        <Toggle checked={v.disableHashJoin} onChange={(b) => onChange({ ...v, disableHashJoin: b })} label="No hash joins" />
        <Toggle checked={v.coldCache} onChange={(b) => onChange({ ...v, coldCache: b })} label="Cold buffer pool" hint="Empty QueryLab's buffer pool before every iteration" />
      </div>
    </div>
  );
}

function CompareMode() {
  const sql = useStore((s) => s.sql);
  const [a, setA] = useState<VariantInput>({ label: 'A', sql: '', disableIndexScans: false, coldCache: false, disableHashJoin: false });
  const [b, setB] = useState<VariantInput>({ label: 'B', sql: '', disableIndexScans: true, coldCache: false, disableHashJoin: false });
  const [iterations, setIterations] = useState(15);
  const [warmup, setWarmup] = useState(2);
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<BenchmarkResult | null>(null);
  const [error, setError] = useState<ApiError | null>(null);
  const [history, setHistory] = useState<HistoryEntry[]>([]);

  useEffect(() => {
    const firstSelect = sql.split(';').map((s) => s.trim()).filter((s) => /^select/i.test(s.replace(/^--.*$/gm, '').trim())).pop() ?? '';
    const clean = firstSelect.replace(/^--.*$/gm, '').trim();
    setA((v) => (v.sql ? v : { ...v, label: 'With indexes', sql: clean }));
    setB((v) => (v.sql ? v : { ...v, label: 'Sequential scans only', sql: clean }));
    api.history().then(setHistory).catch(() => setHistory([]));
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const go = async () => {
    setBusy(true);
    setError(null);
    try {
      const r = await api.benchmark({
        iterations,
        warmup,
        variants: [a, b].map((v) => ({ label: v.label, sql: v.sql, disableIndexScans: v.disableIndexScans, coldCache: v.coldCache, disableHashJoin: v.disableHashJoin })),
      });
      if (!r.success && r.error) setError(r.error);
      else setResult(r);
    } catch (e) {
      setError({ stage: 'api', message: (e as Error).message, display: (e as Error).message });
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="space-y-5">
      <div className="grid gap-3 lg:grid-cols-2">
        <VariantEditor v={a} onChange={setA} color={COLORS[0]} />
        <VariantEditor v={b} onChange={setB} color={COLORS[1]} />
      </div>
      {history.length > 0 && (
        <details className="text-sm">
          <summary className="cursor-pointer text-haze">Pick from recent queries</summary>
          <ul className="mt-1 space-y-0.5">
            {history.slice(0, 10).map((h) => (
              <li key={h.queryId} className="flex gap-2 items-baseline">
                <button className="text-join hover:underline" onClick={() => setA({ ...a, sql: h.sql })}>
                  Use as A
                </button>
                <button className="text-join hover:underline" onClick={() => setB({ ...b, sql: h.sql })}>
                  Use as B
                </button>
                <span className="code text-xs text-haze truncate">{h.sql.replace(/\s+/g, ' ')}</span>
              </li>
            ))}
          </ul>
        </details>
      )}
      <div className="flex flex-wrap items-end gap-4">
        <label className="block">
          <span className="label">Timed iterations</span>
          <input type="number" min={1} max={500} className="input block w-24 mt-0.5" value={iterations} onChange={(e) => setIterations(Number(e.target.value))} />
        </label>
        <label className="block">
          <span className="label">Warm-up runs (discarded)</span>
          <input type="number" min={0} max={50} className="input block w-24 mt-0.5" value={warmup} onChange={(e) => setWarmup(Number(e.target.value))} />
        </label>
        <button className="btn btn-primary" onClick={go} disabled={busy || !a.sql.trim() || !b.sql.trim()}>
          {busy ? 'Running both variants…' : 'Compare A and B'}
        </button>
      </div>
      {error && <ErrorBox error={error} />}
      {result && <Results result={result} />}
    </div>
  );
}

function Results({ result }: { result: BenchmarkResult }) {
  return (
    <div className="space-y-5">
      <div className="grid gap-5 xl:grid-cols-[1fr_1.1fr]">
        <section className="panel p-4">
          <h2 className="font-medium">Every timed run</h2>
          <p className="text-xs text-haze mb-2">Dots are individual iterations; the white tick is the median.</p>
          <SampleChart variants={result.variants} />
        </section>
        <section className="panel p-4">
          <h2 className="font-medium mb-2">Side by side</h2>
          <Comparison result={result} />
          <ComparisonNote result={result} />
        </section>
      </div>
      <section className="grid gap-px bg-rule rounded overflow-hidden" style={{ gridTemplateColumns: `repeat(${result.variants.length}, minmax(0, 1fr))` }}>
        {result.variants.map((v, i) => (
          <div key={i} className="bg-ink h-[420px] flex flex-col">
            <h3 className="px-3 py-2 text-sm" style={{ color: COLORS[i % COLORS.length] }}>
              Plan for {v.label}
            </h3>
            <div className="flex-1 min-h-0">
              <PlanGraph plan={v.plan} showActual compact />
            </div>
          </div>
        ))}
      </section>
      <ul className="text-xs text-haze space-y-0.5">
        {result.notes.map((n, i) => (
          <li key={i}>{n}</li>
        ))}
        <li>Timer resolution on this machine: about {result.timerOverheadNs.toFixed(0)} ns per clock read.</li>
      </ul>
    </div>
  );
}

/** Before / create index / after, measured with identical settings. */
function IndexExperiment() {
  const refreshCatalog = useStore((s) => s.refreshCatalog);
  const preset = (() => {
    try {
      return JSON.parse(sessionStorage.getItem('querylab.lab') ?? 'null') as { sql: string; index: string } | null;
    } catch {
      return null;
    }
  })();
  const [sql, setSql] = useState(preset?.sql ?? 'SELECT *\nFROM orders\nWHERE customer_id = 42');
  const [indexSql, setIndexSql] = useState(preset?.index ?? 'CREATE INDEX idx_orders_customer ON orders(customer_id)');
  const [iterations, setIterations] = useState(15);
  const [step, setStep] = useState(0);
  const [before, setBefore] = useState<BenchmarkVariant | null>(null);
  const [after, setAfter] = useState<BenchmarkVariant | null>(null);
  const [msg, setMsg] = useState<string | null>(null);
  const [error, setError] = useState<ApiError | null>(null);
  const [busy, setBusy] = useState(false);

  const bench = async (label: string) => {
    const r = await api.benchmark({ iterations, warmup: 2, variants: [{ label, sql }] });
    if (!r.success) throw r.error ?? { stage: 'api', message: 'benchmark failed', display: 'benchmark failed' };
    return r.variants[0];
  };
  const wrap = async (fn: () => Promise<void>) => {
    setBusy(true);
    setError(null);
    try {
      await fn();
    } catch (e) {
      const err = e as ApiError & Error;
      setError(err.stage ? err : { stage: 'api', message: err.message, display: err.message });
    } finally {
      setBusy(false);
    }
  };
  const indexName = indexSql.match(/index\s+(?:if\s+not\s+exists\s+)?([A-Za-z_][A-Za-z0-9_]*)/i)?.[1];

  const steps = [
    {
      title: 'Measure the query as it is',
      action: 'Run baseline',
      run: () =>
        wrap(async () => {
          setBefore(await bench('Before'));
          setAfter(null);
          setStep(1);
        }),
    },
    {
      title: 'Create the index',
      action: 'Create index',
      run: () =>
        wrap(async () => {
          const r = await api.query(indexSql);
          if (!r.success) throw r.error;
          setMsg(r.results[0].message ?? 'Index created.');
          await refreshCatalog();
          setStep(2);
        }),
    },
    {
      title: 'Measure the same query again',
      action: 'Run again',
      run: () =>
        wrap(async () => {
          setAfter(await bench('After'));
          setStep(3);
        }),
    },
  ];

  const usedAfter = usesIndex(after?.plan);
  return (
    <div className="space-y-5">
      <div className="grid gap-3 lg:grid-cols-2">
        <label className="block">
          <span className="label">Query</span>
          <textarea className="input w-full code h-28 mt-0.5" value={sql} onChange={(e) => setSql(e.target.value)} spellCheck={false} disabled={step > 0} />
        </label>
        <label className="block">
          <span className="label">Index to test</span>
          <textarea className="input w-full code h-28 mt-0.5" value={indexSql} onChange={(e) => setIndexSql(e.target.value)} spellCheck={false} disabled={step > 1} />
        </label>
      </div>
      <label className="block w-40">
        <span className="label">Timed iterations per run</span>
        <input type="number" className="input block w-24 mt-0.5" min={1} max={200} value={iterations} onChange={(e) => setIterations(Number(e.target.value))} />
      </label>
      <ol className="grid gap-3 md:grid-cols-3">
        {steps.map((s, i) => (
          <li key={i} className={`panel p-3 ${i === step ? 'border-join' : ''} ${i > step ? 'opacity-50' : ''}`}>
            <div className="text-xs text-haze">Step {i + 1}</div>
            <div className="text-sm text-chalk mb-2">{s.title}</div>
            <button className="btn btn-primary" disabled={busy || i !== step} onClick={s.run}>
              {busy && i === step ? 'Working…' : s.action}
            </button>
            {i === 1 && msg && step > 1 && <p className="mt-2 text-xs text-haze">{msg}</p>}
          </li>
        ))}
      </ol>
      {error && <ErrorBox error={error} />}
      {before && after && (
        <section className="space-y-4">
          <div className={`rounded-md border px-4 py-3 ${usedAfter ? 'border-flow/50 bg-flow/10' : 'border-scan/50 bg-scan/10'}`}>
            {usedAfter ? (
              <p className="text-sm">
                The optimizer chose <span className="code">{usedAfter}</span>. Median time went from {fmtMs(before.stats.median)} to {fmtMs(after.stats.median)},
                and page reads from {fmtInt(before.pagesRead)} to {fmtInt(after.pagesRead)} per run.
                {after.stats.median > before.stats.median ? ' The index made this query slower here; see the notes below.' : ''}
              </p>
            ) : (
              <p className="text-sm">
                The optimizer did not use the new index: it estimated a sequential scan to be cheaper. Check the Optimizer tab for the costs it compared.
              </p>
            )}
          </div>
          <Results result={{ success: true, variants: [before, after], timerOverheadNs: 0, notes: [] }} />
          <div className="text-sm text-haze space-y-1 max-w-3xl">
            <p>An index does not always help. It tends not to when:</p>
            <ul className="list-disc list-inside space-y-0.5">
              <li>the filter matches a large share of rows, so fetching them one at a time touches nearly every page anyway;</li>
              <li>the table is small enough that reading it whole costs only a few pages;</li>
              <li>the condition cannot use the index (OR, LIKE, functions, or a filter only on a later column of a composite index);</li>
              <li>the workload writes much more than it reads, since every write must also update the index.</li>
            </ul>
          </div>
          <div className="flex gap-2">
            <button
              className="btn"
              onClick={() =>
                wrap(async () => {
                  if (indexName) {
                    await api.dropIndex(indexName);
                    await refreshCatalog();
                  }
                  setStep(0);
                  setBefore(null);
                  setAfter(null);
                  setMsg(null);
                })
              }
            >
              Drop the index and start over
            </button>
          </div>
        </section>
      )}
    </div>
  );
}

export function LabPage() {
  const [mode, setMode] = useState<'experiment' | 'compare'>(() => (sessionStorage.getItem('querylab.lab') ? 'experiment' : 'compare'));
  return (
    <div className="h-full overflow-auto">
      <div className="p-4 space-y-4 max-w-7xl">
        <div className="flex flex-wrap items-center gap-4">
          <h1 className="text-xl font-semibold">Performance lab</h1>
          <Segmented
            value={mode}
            onChange={setMode}
            options={[
              { value: 'compare', label: 'Compare two queries' },
              { value: 'experiment', label: 'Index experiment' },
            ]}
          />
        </div>
        <p className="text-sm text-haze max-w-3xl">
          Both sides run on the same data with the same settings, several times each. Times are engine execution only, with per-operator timers
          switched off; page reads and rows examined are exact counts and usually tell the clearer story at small scales.
        </p>
        {mode === 'compare' ? <CompareMode /> : <IndexExperiment />}
      </div>
    </div>
  );
}
