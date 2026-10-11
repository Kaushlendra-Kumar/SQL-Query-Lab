import { useEffect, useState } from 'react';
import { useCurrentStatement, useStore } from '../store';
import { PlanGraph } from '../visualizations/PlanGraph';
import { EmptyState } from '../components/common';
import { fmtCost } from '../visualizations/format';
import type { OptimizerStep } from '../types/api';

function CostChange({ s }: { s: OptimizerStep }) {
  if (s.phase === 'physical') return null;
  const delta = s.costAfter - s.costBefore;
  const pct = s.costBefore > 0 ? (delta / s.costBefore) * 100 : 0;
  return (
    <p className="text-sm">
      <span className="text-haze">Estimated plan cost </span>
      {fmtCost(s.costBefore)} <span className="text-haze">to</span> {fmtCost(s.costAfter)}
      {Math.abs(delta) > 1e-9 && <span className={delta < 0 ? 'text-flow' : 'text-coral'}> ({pct > 0 ? '+' : ''}{pct.toFixed(1)}%)</span>}
    </p>
  );
}

function Alternatives({ s }: { s: OptimizerStep }) {
  const max = Math.max(...s.alternatives.map((a) => a.cost), 1);
  return (
    <table className="w-full text-sm">
      <thead>
        <tr className="text-xs text-haze text-left">
          <th className="font-normal py-1">Alternative</th>
          <th className="font-normal w-[40%]">Estimated cost</th>
        </tr>
      </thead>
      <tbody>
        {s.alternatives.map((a, i) => (
          <tr key={i} className="align-top">
            <td className="py-1.5 pr-3">
              <div className={a.chosen ? 'text-chalk font-medium' : 'text-haze'}>
                {a.name}
                {a.chosen && <span className="ml-2 text-xs text-flow">chosen</span>}
              </div>
              <div className="text-xs text-haze">{a.detail}</div>
            </td>
            <td className="py-1.5">
              {a.cost > 0 ? (
                <div className="flex items-center gap-2">
                  <div className="h-2 rounded-sm" style={{ width: `${Math.max(2, (a.cost / max) * 100)}%`, background: a.chosen ? '#63CDB0' : '#3A4762' }} />
                  <span className="text-xs text-haze">{fmtCost(a.cost)}</span>
                </div>
              ) : (
                <span className="text-xs text-haze">not costed</span>
              )}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function OptimizerPage() {
  const r = useCurrentStatement();
  const setPage = useStore((s) => s.setPage);
  const steps = r?.optimizer ?? [];
  const [sel, setSel] = useState(0);
  useEffect(() => {
    const firstApplied = steps.findIndex((s) => s.applied);
    setSel(firstApplied >= 0 ? firstApplied : 0);
  }, [r]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!r || steps.length === 0) {
    return (
      <EmptyState title="No optimizer trace yet" action={<button className="btn btn-primary" onClick={() => setPage('editor')}>Go to the editor</button>}>
        Run a SELECT, UPDATE or DELETE to see every rule the optimizer considered.
      </EmptyState>
    );
  }
  const s = steps[Math.min(sel, steps.length - 1)];

  return (
    <div className="flex h-full min-h-0">
      <nav className="w-72 shrink-0 border-r border-rule overflow-auto p-2" aria-label="Optimizer steps">
        {(['logical', 'physical'] as const).map((phase) => (
          <div key={phase} className="mb-3">
            <h2 className="px-2 py-1 text-sm font-medium">{phase === 'logical' ? 'Logical rewrites' : 'Physical decisions'}</h2>
            <ol>
              {steps.map((st, i) =>
                st.phase !== phase ? null : (
                  <li key={i}>
                    <button
                      onClick={() => setSel(i)}
                      className={`w-full text-left rounded px-2 py-1.5 ${i === sel ? 'bg-[#26324A]' : 'hover:bg-slab'}`}
                      aria-current={i === sel}
                    >
                      <div className="flex items-center gap-2 text-sm">
                        <span className={`h-2 w-2 rounded-full ${st.applied ? 'bg-flow' : 'bg-rule'}`} aria-hidden />
                        <span className={st.applied ? 'text-chalk' : 'text-haze'}>{st.rule}</span>
                      </div>
                      <div className="ml-4 text-xs text-haze truncate">
                        {st.phase === 'physical'
                          ? st.alternatives.find((a) => a.chosen)?.name ?? 'decided'
                          : st.applied
                            ? 'changed the plan'
                            : 'not applicable'}
                      </div>
                    </button>
                  </li>
                ),
              )}
            </ol>
          </div>
        ))}
        <p className="px-2 text-xs text-haze leading-relaxed">
          Rules run in order, top to bottom. Each rule is applied only when it cannot change the query's result.
        </p>
      </nav>

      <div className="flex-1 min-w-0 flex flex-col overflow-auto">
        <header className="p-4 border-b border-rule space-y-2">
          <h1 className="text-xl font-semibold">{s.rule}</h1>
          <p className="text-sm text-chalk max-w-3xl leading-relaxed">{s.reason}</p>
          <CostChange s={s} />
          {s.details.length > 0 && (
            <ul className="code text-sm text-haze space-y-0.5 max-w-3xl">
              {s.details.map((d, i) => (
                <li key={i}>{d}</li>
              ))}
            </ul>
          )}
        </header>
        {s.phase === 'physical' ? (
          <div className="p-4 max-w-3xl">
            <Alternatives s={s} />
            <p className="mt-3 text-xs text-haze">
              Costs come from a transparent heuristic model (pages read, rows processed, hash and sort work). They rank alternatives; they are not time predictions.
            </p>
          </div>
        ) : s.before && s.after ? (
          <div className="flex-1 min-h-[420px] grid grid-cols-1 xl:grid-cols-2 gap-px bg-rule">
            <section className="bg-ink flex flex-col min-h-[380px]">
              <h2 className="px-3 py-2 text-sm text-haze">Before, cost {fmtCost(s.costBefore)}</h2>
              <div className="flex-1 min-h-0">
                <PlanGraph plan={s.before} showActual={false} compact />
              </div>
            </section>
            <section className="bg-ink flex flex-col min-h-[380px]">
              <h2 className="px-3 py-2 text-sm text-haze">After, cost {fmtCost(s.costAfter)}</h2>
              <div className="flex-1 min-h-0">
                <PlanGraph plan={s.after} showActual={false} highlightIds={s.nodeIds} compact />
              </div>
            </section>
          </div>
        ) : (
          <p className="p-4 text-sm text-haze">This rule left the plan unchanged, so there is nothing to compare.</p>
        )}
      </div>
    </div>
  );
}
