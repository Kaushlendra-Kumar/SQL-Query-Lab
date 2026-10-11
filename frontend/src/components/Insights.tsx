import { useState } from 'react';
import { api } from '../api/client';
import { useStore } from '../store';
import type { Advisor, Explanation } from '../types/api';
import { fmtCost } from '../visualizations/format';

const TOPIC_TITLES: Record<string, string> = {
  intent: 'What the query asks for',
  tables: 'Tables and columns',
  filtering: 'Filtering',
  indexes: 'Indexes considered',
  operators: 'Operators chosen',
  rowflow: 'How rows flowed',
  hotspot: 'Where the time went',
  estimates: 'Estimates vs reality',
  timing: 'About the timings',
};
const ORDER = ['intent', 'tables', 'filtering', 'indexes', 'operators', 'rowflow', 'hotspot', 'estimates', 'timing'];

export function ExplanationPanel({ explanation, onFocusNode }: { explanation: Explanation; onFocusNode?: (id: number) => void }) {
  const groups = ORDER.map((t) => ({ topic: t, facts: explanation.facts.filter((f) => f.topic === t) })).filter((g) => g.facts.length);
  return (
    <div className="space-y-5 max-w-3xl">
      <p className="text-lg text-chalk">{explanation.summary}</p>
      <section>
        <h3 className="text-sm font-medium text-chalk">Observed in this run</h3>
        <p className="text-xs text-haze mb-2">Every statement below comes from the engine's plan metadata and measurements.</p>
        <dl className="space-y-3">
          {groups.map((g) => (
            <div key={g.topic}>
              <dt className="text-sm text-haze">{TOPIC_TITLES[g.topic] ?? g.topic}</dt>
              {g.facts.map((f, i) => (
                <dd key={i} className={`text-sm leading-relaxed ${g.topic === 'estimates' ? 'text-coral' : 'text-chalk'}`}>
                  {f.text}
                  {f.nodeId !== undefined && onFocusNode && (
                    <button className="ml-2 text-xs text-join hover:underline" onClick={() => onFocusNode(f.nodeId!)}>
                      Show in plan
                    </button>
                  )}
                </dd>
              ))}
            </div>
          ))}
        </dl>
      </section>
      {explanation.guidance.length > 0 && (
        <section className="border-l-2 border-rule pl-3">
          <h3 className="text-sm font-medium text-chalk">General guidance</h3>
          <p className="text-xs text-haze mb-1">Educational background, not a measurement of this query.</p>
          <ul className="space-y-1.5">
            {explanation.guidance.map((g, i) => (
              <li key={i} className="text-sm text-haze leading-relaxed">
                {g}
              </li>
            ))}
          </ul>
        </section>
      )}
    </div>
  );
}

export function AdvisorPanel({ advisor, sql }: { advisor: Advisor; sql: string }) {
  const refreshCatalog = useStore((s) => s.refreshCatalog);
  const setPage = useStore((s) => s.setPage);
  const [status, setStatus] = useState<Record<string, string>>({});

  const create = async (def: string, table: string, cols: string[]) => {
    const name = def.split(' ')[2];
    setStatus((s) => ({ ...s, [def]: 'Creating…' }));
    try {
      const r = await api.createIndex(name, table, cols);
      setStatus((s) => ({ ...s, [def]: r.success ? r.results[0].message ?? 'Created.' : r.error?.message ?? 'Failed.' }));
      await refreshCatalog();
    } catch (e) {
      setStatus((s) => ({ ...s, [def]: (e as Error).message }));
    }
  };

  const openLab = (def: string) => {
    sessionStorage.setItem('querylab.lab', JSON.stringify({ sql, index: def }));
    setPage('lab');
  };

  if (advisor.suggestions.length === 0 && advisor.notes.length === 0) {
    return <p className="text-sm text-haze">No index suggestions: the plan has no filter that an index could answer.</p>;
  }
  return (
    <div className="space-y-4 max-w-3xl">
      {advisor.suggestions.map((s) => (
        <article key={s.definition} className="panel p-4">
          <p className="code text-scan">{s.definition}</p>
          <div className="mt-3 grid gap-3 sm:grid-cols-2">
            <div>
              <h4 className="text-sm text-haze">Evidence from the plan</h4>
              <ul className="mt-1 space-y-1 text-sm">
                {s.evidence.map((e, i) => (
                  <li key={i}>{e}</li>
                ))}
              </ul>
            </div>
            <div>
              <h4 className="text-sm text-haze">Expected benefit (estimate)</h4>
              <p className="mt-1 text-sm">{s.expectedBenefit}</p>
              <p className="mt-1 text-xs text-haze">
                Cost {fmtCost(s.estimatedCost.current)} now, about {fmtCost(s.estimatedCost.withIndex)} with the index.
              </p>
            </div>
            <div>
              <h4 className="text-sm text-haze">Write and storage overhead</h4>
              <p className="mt-1 text-sm">{s.overhead}</p>
            </div>
            <div>
              <h4 className="text-sm text-haze">Limitations</h4>
              <ul className="mt-1 space-y-1 text-sm">
                {s.limitations.map((l, i) => (
                  <li key={i}>{l}</li>
                ))}
              </ul>
            </div>
          </div>
          <div className="mt-3">
            <h4 className="text-sm text-haze">How to verify</h4>
            <ol className="mt-1 list-decimal list-inside space-y-0.5 text-sm">
              {s.verify.map((v, i) => (
                <li key={i}>{v}</li>
              ))}
            </ol>
          </div>
          <div className="mt-3 flex flex-wrap items-center gap-2">
            <button className="btn btn-primary" onClick={() => openLab(s.definition)}>
              Test in the Performance Lab
            </button>
            <button className="btn" onClick={() => create(s.definition, s.table, s.columns)}>
              Create index now
            </button>
            {status[s.definition] && <span className="text-sm text-haze">{status[s.definition]}</span>}
          </div>
        </article>
      ))}
      {advisor.notes.length > 0 && (
        <section>
          <h4 className="text-sm text-haze mb-1">Why nothing else is suggested</h4>
          <ul className="space-y-1.5">
            {advisor.notes.map((n, i) => (
              <li key={i} className="text-sm text-haze leading-relaxed">
                {n.text}
              </li>
            ))}
          </ul>
        </section>
      )}
    </div>
  );
}
