import { useEffect, useState } from 'react';
import { api } from '../api/client';
import { useStore, type Page } from '../store';
import type { Settings } from '../types/api';
import { fmtInt } from '../visualizations/format';

const NAV: { page: Page; label: string }[] = [
  { page: 'editor', label: 'Editor' },
  { page: 'explain', label: 'Visual Explain' },
  { page: 'optimizer', label: 'Optimizer' },
  { page: 'storage', label: 'Storage Explorer' },
  { page: 'lab', label: 'Benchmarks' },
];

function SettingsPopover({ onClose }: { onClose: () => void }) {
  const [s, setS] = useState<Settings | null>(null);
  const [msg, setMsg] = useState<string | null>(null);
  useEffect(() => {
    api.settings().then(setS).catch((e) => setMsg(e.message));
  }, []);
  const save = async () => {
    if (!s) return;
    try {
      setS(await api.updateSettings({ bufferPoolPages: s.bufferPoolPages, timeoutMs: s.timeoutMs, maxResultRows: s.maxResultRows, maxEvents: s.maxEvents }));
      setMsg('Settings saved.');
    } catch (e) {
      setMsg((e as Error).message);
    }
  };
  const field = (key: keyof Settings, label: string, hint: string) => (
    <label className="block">
      <span className="label">{label}</span>
      <input
        type="number"
        className="input w-full mt-0.5"
        value={s ? String(s[key]) : ''}
        onChange={(e) => s && setS({ ...s, [key]: Number(e.target.value) })}
      />
      <span className="text-[0.7rem] text-haze/80">{hint}</span>
    </label>
  );
  return (
    <div className="absolute right-3 top-12 z-50 w-80 panel p-4 shadow-2xl space-y-3" role="dialog" aria-label="Engine settings">
      <div className="flex justify-between items-center">
        <h2 className="font-medium">Engine settings</h2>
        <button className="btn-ghost btn px-2 py-0.5" onClick={onClose} aria-label="Close settings">
          Close
        </button>
      </div>
      {field('bufferPoolPages', 'Buffer pool size (4 KB pages)', 'Smaller pools cause more misses. Resizing empties the pool.')}
      {field('timeoutMs', 'Query time limit (ms)', 'Running queries are stopped after this long.')}
      {field('maxResultRows', 'Rows kept per result', 'Rows beyond this are counted but not returned.')}
      {field('maxEvents', 'Recorded row events', 'Caps the execution replay trace.')}
      {s && <p className="text-xs text-haze">Timer overhead on this machine: about {s.timerOverheadNs.toFixed(0)} ns per measurement.</p>}
      <div className="flex items-center gap-2">
        <button className="btn btn-primary" onClick={save} disabled={!s}>
          Save settings
        </button>
        {msg && <span className="text-xs text-haze">{msg}</span>}
      </div>
    </div>
  );
}

export function TopBar() {
  const page = useStore((s) => s.page);
  const setPage = useStore((s) => s.setPage);
  const catalog = useStore((s) => s.catalog);
  const transportError = useStore((s) => s.transportError);
  const [open, setOpen] = useState(false);
  return (
    <header className="relative flex items-center gap-6 border-b border-rule bg-slab px-4 h-12 shrink-0">
      <div className="flex items-center gap-2">
        <svg width="20" height="20" viewBox="0 0 32 32" aria-hidden>
          <path d="M6 26V8h5v18zM13.5 26V14h5v12zM21 26v-7h5v7z" fill="#E9B44C" />
        </svg>
        <span className="font-semibold tracking-tight text-[1.05rem]">QueryLab</span>
      </div>
      <nav className="flex h-full" aria-label="Main">
        {NAV.map((n) => (
          <button
            key={n.page}
            className={`h-full px-3 text-sm border-b-2 ${page === n.page ? 'border-scan text-chalk' : 'border-transparent text-haze hover:text-chalk'}`}
            aria-current={page === n.page ? 'page' : undefined}
            onClick={() => setPage(n.page)}
          >
            {n.label}
          </button>
        ))}
      </nav>
      <div className="ml-auto flex items-center gap-3 text-xs text-haze">
        {transportError ? (
          <span className="text-coral">Server unreachable</span>
        ) : catalog ? (
          <span>
            {catalog.storage === 'file' ? `File ${catalog.path}` : 'In-memory database'}, {catalog.tables.length} tables
          </span>
        ) : null}
        <button className="btn btn-ghost px-2 py-1" onClick={() => setOpen((o) => !o)} aria-expanded={open}>
          Settings
        </button>
      </div>
      {open && <SettingsPopover onClose={() => setOpen(false)} />}
    </header>
  );
}

const SCALES = [100, 1000, 10000, 50000];

export function Sidebar() {
  const catalog = useStore((s) => s.catalog);
  const datasets = useStore((s) => s.datasets);
  const loading = useStore((s) => s.loadingDataset);
  const loadDataset = useStore((s) => s.loadDataset);
  const setSql = useStore((s) => s.setSql);
  const setPage = useStore((s) => s.setPage);
  const refreshCatalog = useStore((s) => s.refreshCatalog);
  const [dataset, setDataset] = useState('ecommerce');
  const [scale, setScale] = useState(10000);
  const [expanded, setExpanded] = useState<string | null>(null);

  const ds = datasets.find((d) => d.name === dataset);
  const loaded = new Set(catalog?.tables.map((t) => t.name));
  const datasetLoaded = !!ds && ds.tables.every((t) => loaded.has(t));

  return (
    <aside className="w-64 shrink-0 border-r border-rule bg-slab/60 flex flex-col overflow-hidden" aria-label="Database">
      <section className="p-3 border-b border-rule space-y-2">
        <h2 className="text-sm font-medium">Sample data</h2>
        <select className="input w-full" value={dataset} onChange={(e) => setDataset(e.target.value)} aria-label="Dataset">
          {datasets.map((d) => (
            <option key={d.name} value={d.name}>
              {d.title}
            </option>
          ))}
        </select>
        {ds && <p className="text-xs text-haze leading-snug">{ds.description}</p>}
        <div className="flex gap-2">
          <select className="input flex-1" value={scale} onChange={(e) => setScale(Number(e.target.value))} aria-label="Rows in the main table">
            {SCALES.map((s) => (
              <option key={s} value={s}>
                {fmtInt(s)} rows
              </option>
            ))}
          </select>
          <button className="btn btn-primary" disabled={!!loading} onClick={() => loadDataset(dataset, scale)}>
            {loading ? 'Loading…' : datasetLoaded ? 'Reload' : 'Load'}
          </button>
        </div>
        {ds && datasetLoaded && (
          <div className="pt-1">
            <h3 className="text-xs text-haze mb-1">Try a query</h3>
            <ul className="space-y-0.5">
              {ds.sampleQueries.map((q) => (
                <li key={q.title}>
                  <button
                    className="text-left text-sm text-join hover:underline"
                    onClick={() => {
                      setSql(q.sql);
                      setPage('editor');
                    }}
                  >
                    {q.title}
                  </button>
                </li>
              ))}
            </ul>
          </div>
        )}
      </section>

      <section className="flex-1 overflow-auto p-3">
        <div className="flex items-center justify-between mb-1">
          <h2 className="text-sm font-medium">Tables</h2>
          <button className="text-xs text-haze hover:text-chalk" onClick={() => refreshCatalog()}>
            Refresh
          </button>
        </div>
        {!catalog || catalog.tables.length === 0 ? (
          <p className="text-sm text-haze">No tables yet. Load sample data above or run CREATE TABLE in the editor.</p>
        ) : (
          <ul className="space-y-0.5">
            {catalog.tables.map((t) => (
              <li key={t.name}>
                <button
                  className="w-full flex items-baseline justify-between text-left text-sm py-0.5 hover:text-chalk"
                  onClick={() => setExpanded(expanded === t.name ? null : t.name)}
                  aria-expanded={expanded === t.name}
                >
                  <span className="text-scan">{t.name}</span>
                  <span className="text-xs text-haze">{fmtInt(t.rowCount)}</span>
                </button>
                {expanded === t.name && (
                  <div className="ml-2 mb-2 border-l border-rule pl-2">
                    <ul>
                      {t.columns.map((c) => (
                        <li key={c.name} className="flex justify-between text-xs py-px" title={`${fmtInt(c.stats.distinct)} distinct, ${fmtInt(c.stats.nulls)} NULL`}>
                          <span className="code text-chalk">{c.name}</span>
                          <span className="text-haze">{c.type.toLowerCase()}</span>
                        </li>
                      ))}
                    </ul>
                    {t.indexes.length > 0 && (
                      <ul className="mt-1">
                        {t.indexes.map((i) => (
                          <li key={i.name} className="text-xs text-haze" title={i.definition}>
                            index <span className="text-reshape">{i.name}</span> ({i.columns.join(', ')})
                          </li>
                        ))}
                      </ul>
                    )}
                    <button
                      className="mt-1 text-xs text-join hover:underline"
                      onClick={() => {
                        setSql(`SELECT *\nFROM ${t.name}\nLIMIT 50;`);
                        setPage('editor');
                      }}
                    >
                      Query this table
                    </button>
                  </div>
                )}
              </li>
            ))}
          </ul>
        )}
      </section>
    </aside>
  );
}
