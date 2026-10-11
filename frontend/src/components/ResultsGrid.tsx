import { useEffect, useState } from 'react';
import type { StatementResult } from '../types/api';
import { cellText, fmtInt } from '../visualizations/format';

const PAGE = 200;

export function ResultsGrid({ result }: { result: StatementResult }) {
  const [shown, setShown] = useState(PAGE);
  useEffect(() => setShown(PAGE), [result]);
  const cols = result.columns ?? [];
  const rows = result.rows ?? [];
  if (cols.length === 0) {
    return <div className="p-4 text-sm text-haze">{result.message ?? 'Statement completed.'}</div>;
  }
  return (
    <div className="h-full overflow-auto">
      <table className="min-w-full text-sm border-separate border-spacing-0">
        <thead className="sticky top-0 z-10 bg-slab">
          <tr>
            <th className="w-12 px-2 py-1.5 text-right text-xs font-normal text-haze/70 border-b border-rule">#</th>
            {cols.map((c, i) => (
              <th key={i} className="px-3 py-1.5 text-left font-medium border-b border-rule whitespace-nowrap">
                {c.name}
                <span className="ml-1.5 text-[0.68rem] font-normal text-haze">{c.type.toLowerCase()}</span>
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.slice(0, shown).map((r, i) => (
            <tr key={i} className="hover:bg-[#1A2130]">
              <td className="px-2 py-1 text-right text-xs text-haze/60 border-b border-rule/50">{i + 1}</td>
              {r.map((v, j) => (
                <td
                  key={j}
                  className={`px-3 py-1 border-b border-rule/50 whitespace-nowrap code ${v === null ? 'text-haze/60 italic' : typeof v === 'number' ? 'text-right' : ''}`}
                >
                  {cellText(v)}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
      {rows.length === 0 && <div className="p-4 text-sm text-haze">The query returned no rows.</div>}
      {(rows.length > shown || result.truncated) && (
        <div className="p-3 text-sm text-haze flex items-center gap-3">
          <span>
            Showing {fmtInt(Math.min(shown, rows.length))} of {fmtInt(result.rowCount)} rows
            {result.truncated ? ` (the response kept the first ${fmtInt(rows.length)}; raise the row limit in settings to keep more)` : ''}.
          </span>
          {rows.length > shown && (
            <button className="btn" onClick={() => setShown((s) => s + 500)}>
              Show 500 more
            </button>
          )}
        </div>
      )}
    </div>
  );
}
