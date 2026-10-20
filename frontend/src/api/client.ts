import type {
  BenchmarkResult,
  Catalog,
  Dataset,
  HistoryEntry,
  IndexDetail,
  PageDetail,
  QueryOptions,
  ScriptResult,
  Settings,
  StorageInfo,
  TableSchema,
  Token,
  AstNode,
  ApiError,
} from '../types/api';

const BASE = '/api/v1';

/** Error raised for transport failures and non-2xx responses. */
export class RequestError extends Error {
  constructor(
    message: string,
    public readonly status: number,
    public readonly apiError?: ApiError,
  ) {
    super(message);
  }
}

/** A transport returns the HTTP status and the JSON response text. */
type Transport = (method: string, path: string, body: string | undefined, signal?: AbortSignal) => Promise<{ status: number; text: string }>;

/** The C++ HTTP server (scripts/start.sh, or scripts/dev.sh with VITE_BACKEND=server). */
const serverTransport: Transport = async (method, path, body, signal) => {
  let res: Response;
  try {
    res = await fetch(BASE + path, {
      method,
      headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
      body,
      signal,
    });
  } catch (e) {
    if ((e as Error).name === 'AbortError') throw e;
    throw new RequestError('Cannot reach the QueryLab server. Start it with scripts/start.sh (or querylab_server) and reload.', 0);
  }
  return { status: res.status, text: await res.text() };
};

/** The engine compiled to WebAssembly, running in a Web Worker (see engine.worker.ts). */
function workerTransport(): Transport {
  let worker: Worker | null = null;
  let nextId = 0;
  const pending = new Map<number, { resolve: (r: { status: number; text: string }) => void; reject: (e: Error) => void }>();
  const start = () => {
    const w = new Worker(new URL('./engine.worker.ts', import.meta.url), { type: 'module' });
    w.onmessage = (e: MessageEvent) => {
      const { id, status, body, error } = e.data;
      const p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      if (error !== undefined) p.reject(new RequestError(`The in-browser engine failed: ${error}`, 0));
      else p.resolve({ status, text: body });
    };
    w.postMessage({ init: new URL('wasm/querylab.js', document.baseURI).href });
    return w;
  };
  return (method, path, body, signal) =>
    new Promise((resolve, reject) => {
      worker ??= start();
      const id = nextId++;
      pending.set(id, { resolve, reject });
      signal?.addEventListener('abort', () => {
        if (pending.delete(id)) reject(new DOMException('Aborted', 'AbortError'));
      });
      worker.postMessage({ id, method, path, body });
    });
}

const transport: Transport = import.meta.env.VITE_BACKEND === 'server' ? serverTransport : workerTransport();

async function request<T>(method: string, path: string, body?: unknown, signal?: AbortSignal): Promise<T> {
  const res = await transport(method, path, body !== undefined ? JSON.stringify(body) : undefined, signal);
  let data: unknown = null;
  try {
    data = res.text ? JSON.parse(res.text) : null;
  } catch {
    throw new RequestError(`The server returned an unreadable response (HTTP ${res.status}).`, res.status);
  }
  if (res.status < 200 || res.status >= 300) {
    const err = (data as { error?: ApiError } | null)?.error;
    throw new RequestError(err?.message ?? `Request failed with HTTP ${res.status}`, res.status, err);
  }
  return data as T;
}

export const api = {
  health: () => request<{ status: string; storage: string }>('GET', '/health'),
  query: (sql: string, options?: QueryOptions, signal?: AbortSignal) => request<ScriptResult>('POST', '/query', { sql, options }, signal),
  explain: (sql: string, options?: QueryOptions) => request<ScriptResult>('POST', '/explain', { sql, options }),
  parse: (sql: string, signal?: AbortSignal) =>
    request<{ success: boolean; tokens?: Token[]; statements?: AstNode[]; error?: ApiError }>('POST', '/parse', { sql }, signal),
  catalog: () => request<Catalog>('GET', '/catalog'),
  schema: (table: string) => request<TableSchema>('GET', `/tables/${encodeURIComponent(table)}/schema`),
  rows: (table: string, offset: number, limit: number) =>
    request<{ table: string; columns: { name: string; type: string }[]; rows: { rid: { page: number; slot: number }; values: unknown[] }[]; total: number }>(
      'GET',
      `/tables/${encodeURIComponent(table)}/rows?offset=${offset}&limit=${limit}`,
    ),
  page: (table: string, index: number) => request<PageDetail>('GET', `/tables/${encodeURIComponent(table)}/pages/${index}`),
  index: (name: string) => request<IndexDetail>('GET', `/indexes/${encodeURIComponent(name)}`),
  createIndex: (name: string, table: string, columns: string[], unique = false) =>
    request<ScriptResult>('POST', '/indexes', { name, table, columns, unique }),
  dropIndex: (name: string) => request<ScriptResult>('DELETE', `/indexes/${encodeURIComponent(name)}`),
  storage: () => request<StorageInfo>('GET', '/storage'),
  datasets: () => request<Dataset[]>('GET', '/datasets'),
  loadDataset: (name: string, scale: number) =>
    request<{ dataset: string; scale: number; tables: Record<string, number>; ms: number }>('POST', `/datasets/${name}/load`, { scale }),
  reset: () => request<{ ok: boolean }>('POST', '/reset'),
  benchmark: (body: unknown) => request<BenchmarkResult>('POST', '/benchmarks', body),
  history: () => request<HistoryEntry[]>('GET', '/history'),
  settings: () => request<Settings>('GET', '/settings'),
  updateSettings: (patch: Partial<Settings>) => request<Settings>('PUT', '/settings', patch),
  cancel: () => request<{ success: boolean }>('POST', '/cancel'),
};
