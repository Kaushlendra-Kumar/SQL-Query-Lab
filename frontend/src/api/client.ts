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

async function request<T>(method: string, path: string, body?: unknown, signal?: AbortSignal): Promise<T> {
  let res: Response;
  try {
    res = await fetch(BASE + path, {
      method,
      headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
      body: body !== undefined ? JSON.stringify(body) : undefined,
      signal,
    });
  } catch (e) {
    if ((e as Error).name === 'AbortError') throw e;
    throw new RequestError('Cannot reach the QueryLab server. Start it with scripts/start.sh (or querylab_server) and reload.', 0);
  }
  const text = await res.text();
  let data: unknown = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    throw new RequestError(`The server returned an unreadable response (HTTP ${res.status}).`, res.status);
  }
  if (!res.ok) {
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
