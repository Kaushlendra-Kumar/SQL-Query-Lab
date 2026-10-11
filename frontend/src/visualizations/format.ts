// Formatting helpers and the operator colour vocabulary shared by every view.

export type Family = 'scan' | 'join' | 'reshape' | 'flow';

export const FAMILY_COLOR: Record<Family, string> = {
  scan: '#E9B44C',
  join: '#5FB8E8',
  reshape: '#B49BEA',
  flow: '#63CDB0',
};

export const FAMILY_NAME: Record<Family, string> = {
  scan: 'reads stored data',
  join: 'combines inputs',
  reshape: 'buffers and reorganizes rows',
  flow: 'streams rows through',
};

export function familyOf(op: string): Family {
  if (/Scan|Values|Empty/.test(op)) return 'scan';
  if (/Join/.test(op)) return 'join';
  if (/Aggregate|Sort|Distinct/.test(op)) return 'reshape';
  return 'flow';
}

export function fmtInt(n: number | undefined | null): string {
  if (n === undefined || n === null || Number.isNaN(n)) return '—';
  return Math.round(n).toLocaleString('en-US');
}

/** Estimates are rounded and shown with a "~" so they never look measured. */
export function fmtEst(n: number | undefined): string {
  if (n === undefined) return '—';
  if (n < 10) return `~${n.toFixed(n < 1 && n > 0 ? 2 : 1)}`;
  return `~${fmtInt(n)}`;
}

export function fmtMs(ms: number | undefined): string {
  if (ms === undefined || ms === null) return '—';
  if (ms < 0.01) return `${(ms * 1000).toFixed(1)} µs`;
  if (ms < 10) return `${ms.toFixed(3)} ms`;
  if (ms < 1000) return `${ms.toFixed(1)} ms`;
  return `${(ms / 1000).toFixed(2)} s`;
}

export function fmtBytes(b: number | undefined): string {
  if (!b) return '0 B';
  if (b < 1024) return `${b} B`;
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`;
  return `${(b / 1024 / 1024).toFixed(1)} MB`;
}

export function fmtCost(c: number | undefined): string {
  if (c === undefined) return '—';
  return c < 10 ? c.toFixed(2) : c.toFixed(1);
}

export function pct(part: number, whole: number): string {
  if (!whole) return '0%';
  const p = (part / whole) * 100;
  return `${p < 1 && p > 0 ? p.toFixed(2) : p.toFixed(1)}%`;
}

export function cellText(v: unknown): string {
  if (v === null || v === undefined) return 'NULL';
  if (typeof v === 'boolean') return v ? 'TRUE' : 'FALSE';
  // Doubles print with up to 12 significant digits (hides binary noise like 0.30000000000000004).
  if (typeof v === 'number' && !Number.isInteger(v)) return String(Number(v.toPrecision(12)));
  return String(v);
}

/** Ratio between estimate and actual, >= 1, for highlighting mis-estimates. */
export function misestimate(est: number, act: number): number {
  const hi = Math.max(est, act);
  const lo = Math.max(1, Math.min(est, act));
  return hi / lo;
}
