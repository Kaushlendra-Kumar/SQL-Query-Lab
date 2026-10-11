// DTOs returned by the QueryLab C++ engine (see docs/api.md).

export type Cell = string | number | boolean | null;

export interface Span {
  begin: number;
  end: number;
  line: number;
  column: number;
}

export interface ApiError {
  stage: string;
  message: string;
  display: string;
  span?: Span;
}

export interface Token {
  type: string;
  lexeme: string;
  value: string;
  span: Span;
}

export interface AstNode {
  id: number;
  type: string;
  label: string;
  span: Span;
  details: Record<string, unknown>;
  children: AstNode[];
}

export interface ColumnMeta {
  id: number;
  name: string;
  type: string;
  qualifier?: string;
  source?: string;
}

export interface Estimates {
  rows: number;
  cost: number;
  selfCost: number;
}

export interface Actual {
  executed: boolean;
  /** false when a parent (e.g. LIMIT) stopped pulling rows early */
  completed?: boolean;
  rowsIn?: number;
  rowsOut?: number;
  rowsFiltered?: number;
  rowsScanned?: number;
  pagesRead?: number;
  pageMisses?: number;
  indexNodesVisited?: number;
  indexEntriesExamined?: number;
  nextCalls?: number;
  buildRows?: number;
  groups?: number;
  timeMs?: number;
  selfTimeMs?: number;
  peakMemoryBytes?: number;
}

export interface PlanNode {
  id: number;
  logicalId?: number;
  kind: 'logical' | 'physical';
  op: string;
  label: string;
  description: string;
  details: Record<string, unknown>;
  output: ColumnMeta[];
  estimates: Estimates;
  actual?: Actual;
  notes: string[];
  children: PlanNode[];
}

export interface Alternative {
  name: string;
  cost: number;
  chosen: boolean;
  detail: string;
  applicable: boolean;
}

export interface OptimizerStep {
  index: number;
  rule: string;
  phase: 'logical' | 'physical';
  applied: boolean;
  reason: string;
  details: string[];
  costBefore: number;
  costAfter: number;
  alternatives: Alternative[];
  nodeIds: number[];
  before?: PlanNode;
  after?: PlanNode;
}

export interface TimelineStage {
  stage: string;
  ms: number;
  ok: boolean;
}

export interface ExecEvent {
  seq: number;
  nodeId: number;
  type: 'open' | 'row' | 'done' | 'close' | 'build_done' | 'sort_done' | 'aggregate_done';
  rows: number;
  tMs: number;
}

export interface Fact {
  topic: string;
  text: string;
  nodeId?: number;
}

export interface Explanation {
  summary: string;
  facts: Fact[];
  guidance: string[];
  estimateWarnings: { nodeId: number; op: string; estimatedRows: number; actualRows: number; factor: number }[];
  hotspot: { nodeId: number; op: string; selfTimeMs: number; share: number } | null;
}

export interface Suggestion {
  table: string;
  columns: string[];
  definition: string;
  nodeId: number;
  evidence: string[];
  expectedBenefit: string;
  estimatedCost: { current: number; withIndex: number };
  overhead: string;
  limitations: string[];
  verify: string[];
}

export interface Advisor {
  suggestions: Suggestion[];
  notes: { kind: string; nodeId: number; text: string }[];
}

export interface Metrics {
  executionMs: number;
  planningMs: number;
  totalEngineMs: number;
  rowsScanned: number;
  rowsReturned: number;
  pagesRead: number;
  pageMisses: number;
  indexEntriesExamined: number;
  peakOperatorMemoryBytes: number;
  totalBufferedBytes: number;
  bufferPool: { logicalReads: number; hits: number; misses: number; evictions: number; hitRate: number; capacityPages: number };
  coldCache: boolean;
  timing: boolean;
  timerCalls: number;
  timerOverheadNs: number;
  estimatedTimerOverheadMs: number;
  storage: string;
}

export interface StatementResult {
  queryId: number;
  index: number;
  kind: string;
  sql: string;
  span?: Span;
  explain?: boolean;
  success: boolean;
  error?: ApiError;
  timeline: TimelineStage[];
  tokens?: Token[];
  ast?: AstNode;
  logicalPlan?: PlanNode;
  optimizedPlan?: PlanNode;
  physicalPlan?: PlanNode;
  optimizer?: OptimizerStep[];
  columns?: { name: string; type: string }[];
  rows?: Cell[][];
  rowCount?: number;
  truncated?: boolean;
  affectedRows?: number;
  message?: string;
  metrics?: Metrics;
  events?: { mode: string; note: string; events: ExecEvent[]; truncated: boolean; maxEvents: number };
  explanation?: Explanation;
  advisor?: Advisor;
}

export interface ScriptResult {
  success: boolean;
  results: StatementResult[];
  error?: ApiError;
}

export interface QueryOptions {
  timing?: boolean;
  recordEvents?: boolean;
  disableIndexScans?: boolean;
  disableHashJoin?: boolean;
  coldCache?: boolean;
  maxRows?: number;
}

export interface CatalogColumn {
  name: string;
  type: string;
  stats: { distinct: number; nulls: number; min?: Cell; max?: Cell };
}

export interface CatalogIndex {
  name: string;
  columns: string[];
  unique: boolean;
  height: number;
  entries: number;
  definition: string;
}

export interface CatalogTable {
  name: string;
  rowCount: number;
  pageCount: number;
  columns: CatalogColumn[];
  indexes: CatalogIndex[];
}

export interface Catalog {
  tables: CatalogTable[];
  storage: string;
  path: string;
}

export interface Dataset {
  name: string;
  title: string;
  description: string;
  tables: string[];
  sampleQueries: { title: string; sql: string }[];
}

export interface StorageInfo {
  mode: string;
  path: string;
  pageSize: number;
  totalPages: number;
  bufferPool: { capacity: number; cached: number; logicalReads: number; hits: number; misses: number; evictions: number; writes: number };
  tables: {
    name: string;
    pageCount: number;
    rowCount: number;
    rowsPerPage: number;
    cachedPages: number;
    pages: { pageId: number; slots: number; live: number; freeBytes: number; cached: boolean }[];
  }[];
  note: string;
}

export interface PageDetail {
  table: string;
  pageIndex: number;
  pageId: number;
  slotCount: number;
  liveRows: number;
  freeBytes: number;
  pageSize: number;
  wasCached: boolean;
  slots: { slot: number; live: boolean; bytes?: number; values?: Cell[] }[];
}

export interface IndexDetail {
  name: string;
  table: string;
  definition: string;
  height: number;
  entries: number;
  fanout: number;
  nodeCount: number;
  leafCount: number;
  approximateBytes: number;
  valid: boolean;
  levels: { depth: number; nodeCount: number; leaf: boolean; nodes: { leaf: boolean; entryCount: number; keys: string[]; truncated: boolean }[] }[];
}

export interface TableSchema {
  name: string;
  rowCount: number;
  pageCount: number;
  columns: (CatalogColumn & { histogram: { low: Cell; high: Cell; count: number }[] })[];
  indexes: string[];
}

export interface BenchmarkVariant {
  label: string;
  sql: string;
  options: { disableIndexScans: boolean; disableHashJoin: boolean; coldCache: boolean };
  iterations: number;
  warmup: number;
  samplesMs: number[];
  stats: { min: number; p25: number; median: number; p75: number; max: number; mean: number; stddev: number; cv: number };
  rowsReturned: number;
  rowsScanned: number;
  pagesRead: number;
  avgPageMisses: number;
  indexEntriesExamined: number;
  planningMs: number;
  plan: PlanNode;
  planLabel: string;
}

export interface BenchmarkResult {
  success: boolean;
  error?: ApiError;
  variants: BenchmarkVariant[];
  timerOverheadNs: number;
  notes: string[];
}

export interface Settings {
  bufferPoolPages: number;
  timeoutMs: number;
  maxResultRows: number;
  maxEvents: number;
  maxMemoryMB: number;
  timerOverheadNs: number;
}

export interface HistoryEntry {
  queryId: number;
  sql: string;
  rowCount: number;
  executionMs: number;
  pagesRead: number;
  plan: string;
  shapeCount: number;
}
