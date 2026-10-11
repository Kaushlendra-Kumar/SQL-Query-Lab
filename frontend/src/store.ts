import { create } from 'zustand';
import { api, RequestError } from './api/client';
import type { Catalog, Dataset, QueryOptions, ScriptResult, StatementResult } from './types/api';

export type Page = 'editor' | 'explain' | 'optimizer' | 'storage' | 'lab';
export type PlanView = 'physical' | 'logical' | 'ast';

const DEFAULT_SQL = `-- Load a sample dataset from the sidebar, or create your own tables.
CREATE TABLE IF NOT EXISTS users (id INTEGER, name TEXT, age INTEGER, city TEXT);

SELECT name, age
FROM users
WHERE age > 25
ORDER BY age DESC
LIMIT 10;`;

function loadSql(): string {
  try {
    return localStorage.getItem('querylab.sql') ?? DEFAULT_SQL;
  } catch {
    return DEFAULT_SQL;
  }
}

interface State {
  page: Page;
  sql: string;
  running: boolean;
  result: ScriptResult | null;
  statementIndex: number;
  selectedNodeId: number | null;
  planView: PlanView;
  showActual: boolean;
  options: QueryOptions;
  catalog: Catalog | null;
  datasets: Dataset[];
  transportError: string | null;
  notice: string | null;
  loadingDataset: string | null;

  setPage: (p: Page) => void;
  setSql: (s: string) => void;
  setOption: <K extends keyof QueryOptions>(k: K, v: QueryOptions[K]) => void;
  run: (opts?: { explain?: boolean; sql?: string }) => Promise<void>;
  cancel: () => Promise<void>;
  selectStatement: (i: number) => void;
  selectNode: (id: number | null) => void;
  setPlanView: (v: PlanView) => void;
  setShowActual: (v: boolean) => void;
  refreshCatalog: () => Promise<void>;
  loadDatasets: () => Promise<void>;
  loadDataset: (name: string, scale: number) => Promise<void>;
  setNotice: (n: string | null) => void;
}

export const useStore = create<State>((set, get) => ({
  page: 'editor',
  sql: loadSql(),
  running: false,
  result: null,
  statementIndex: 0,
  selectedNodeId: null,
  planView: 'physical',
  showActual: true,
  options: { timing: true, recordEvents: true, disableIndexScans: false, disableHashJoin: false, coldCache: false },
  catalog: null,
  datasets: [],
  transportError: null,
  notice: null,
  loadingDataset: null,

  setPage: (page) => set({ page }),
  setSql: (sql) => {
    set({ sql });
    try {
      localStorage.setItem('querylab.sql', sql);
    } catch {
      /* storage unavailable: keep the text in memory only */
    }
  },
  setOption: (k, v) => set({ options: { ...get().options, [k]: v } }),

  run: async ({ explain = false, sql } = {}) => {
    const text = sql ?? get().sql;
    if (!text.trim() || get().running) return;
    set({ running: true, transportError: null });
    try {
      const res = explain ? await api.explain(text, get().options) : await api.query(text, get().options);
      // Focus the last statement that produced a plan, else the last one.
      let idx = res.results.length - 1;
      for (let i = res.results.length - 1; i >= 0; i--) {
        if (res.results[i].physicalPlan || !res.results[i].success) {
          idx = i;
          break;
        }
      }
      set({ result: res, statementIndex: Math.max(0, idx), selectedNodeId: null });
      const changedSchema = res.results.some((r) => r.kind !== 'SELECT');
      if (changedSchema) void get().refreshCatalog();
    } catch (e) {
      set({ transportError: (e as Error).message });
    } finally {
      set({ running: false });
    }
  },
  cancel: async () => {
    try {
      await api.cancel();
    } catch {
      /* the running request will report its own outcome */
    }
  },
  selectStatement: (statementIndex) => set({ statementIndex, selectedNodeId: null }),
  selectNode: (selectedNodeId) => set({ selectedNodeId }),
  setPlanView: (planView) => set({ planView, selectedNodeId: null }),
  setShowActual: (showActual) => set({ showActual }),

  refreshCatalog: async () => {
    try {
      const catalog = await api.catalog();
      set({ catalog, transportError: null });
    } catch (e) {
      set({ transportError: (e as Error).message });
    }
  },
  loadDatasets: async () => {
    try {
      set({ datasets: await api.datasets() });
    } catch (e) {
      set({ transportError: (e as Error).message });
    }
  },
  loadDataset: async (name, scale) => {
    set({ loadingDataset: name });
    try {
      const r = await api.loadDataset(name, scale);
      const total = Object.values(r.tables).reduce((a, b) => a + b, 0);
      set({ notice: `Loaded ${name}: ${total.toLocaleString()} rows in ${Object.keys(r.tables).length} tables (${r.ms.toFixed(0)} ms).` });
      await get().refreshCatalog();
    } catch (e) {
      set({ transportError: e instanceof RequestError ? e.message : String(e) });
    } finally {
      set({ loadingDataset: null });
    }
  },
  setNotice: (notice) => set({ notice }),
}));

/** The statement currently being inspected. */
export function useCurrentStatement(): StatementResult | null {
  return useStore((s) => s.result?.results[s.statementIndex] ?? null);
}
