import type { OptimizerStep, PlanNode } from '../types/api';
import { FAMILY_COLOR, FAMILY_NAME, familyOf, fmtBytes, fmtCost, fmtEst, fmtInt, fmtMs, misestimate, pct } from '../visualizations/format';
import { flatten } from '../visualizations/treeLayout';

function findParent(root: PlanNode, id: number): PlanNode | null {
  for (const n of flatten(root)) {
    if (n.children.some((c) => c.id === id)) return n;
  }
  return null;
}

function Row({ k, v, mono = false }: { k: string; v: React.ReactNode; mono?: boolean }) {
  return (
    <div className="grid grid-cols-[8.5rem_1fr] gap-2 py-0.5">
      <dt className="text-haze text-sm">{k}</dt>
      <dd className={`text-sm text-chalk break-words ${mono ? 'code' : ''}`}>{v}</dd>
    </div>
  );
}

const DETAIL_LABELS: Record<string, string> = {
  table: 'Table',
  alias: 'Alias',
  columnsRead: 'Columns read',
  filter: 'Filter',
  pushedPredicate: 'Pushed filter',
  index: 'Index',
  indexDefinition: 'Index definition',
  indexCondition: 'Index condition',
  indexHeight: 'Tree height',
  indexEntries: 'Index entries',
  predicate: 'Predicate',
  expressions: 'Expressions',
  joinType: 'Join type',
  hashKeys: 'Hash keys',
  buildSide: 'Build side',
  probeSide: 'Probe side',
  residual: 'Residual condition',
  condition: 'Condition',
  groupBy: 'Group by',
  aggregates: 'Aggregates',
  keys: 'Sort keys',
  limit: 'Limit',
  offset: 'Offset',
  assignments: 'Assignments',
  tableRows: 'Table rows',
  tablePages: 'Table pages',
};

const MONO = new Set(['filter', 'pushedPredicate', 'indexCondition', 'predicate', 'expressions', 'hashKeys', 'residual', 'condition', 'groupBy', 'aggregates', 'keys', 'indexDefinition', 'assignments']);

export function OperatorInspector({ node, root, steps, showActual }: { node: PlanNode; root: PlanNode; steps: OptimizerStep[]; showActual: boolean }) {
  const parent = findParent(root, node.id);
  const fam = familyOf(node.op);
  const a = node.actual;
  const executed = !!a?.executed;
  // Optimizer steps refer to logical node ids; physical nodes carry theirs in logicalId.
  const logicalId = node.kind === 'physical' ? node.logicalId : node.id;
  const related = steps.filter((s) => logicalId !== undefined && s.nodeIds.includes(logicalId) && (s.applied || s.phase === 'physical'));
  const input = node.children.flatMap((c) => c.output);
  const details = Object.entries(node.details).filter(([k]) => k !== 'selectivityEstimate' && k !== 'tableColumns' && k !== 'indexPredicates');

  return (
    <div className="space-y-4">
      <header>
        <div className="flex items-center gap-2">
          <span className="h-2.5 w-2.5 rounded-full" style={{ background: FAMILY_COLOR[fam] }} />
          <h3 className="text-lg font-semibold" style={{ color: FAMILY_COLOR[fam] }}>
            {node.op}
          </h3>
          <span className="text-xs text-haze">
            {node.kind} operator, {FAMILY_NAME[fam]}
          </span>
        </div>
        <p className="mt-1 text-sm text-haze leading-relaxed">{node.description}</p>
      </header>

      <section>
        <h4 className="text-sm font-medium mb-1">Estimated vs actual</h4>
        <table className="w-full text-sm">
          <thead>
            <tr className="text-haze text-xs">
              <th className="text-left font-normal py-0.5" />
              <th className="text-right font-normal">Estimated</th>
              <th className="text-right font-normal">Actual</th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td className="text-haze">Rows out</td>
              <td className="text-right">{fmtEst(node.estimates.rows)}</td>
              <td className="text-right">{executed && showActual ? fmtInt(a!.rowsOut) : '—'}</td>
            </tr>
            <tr>
              <td className="text-haze">Cost / time</td>
              <td className="text-right" title="Cumulative cost in abstract units; self cost in brackets">
                {fmtCost(node.estimates.cost)} <span className="text-haze">({fmtCost(node.estimates.selfCost)})</span>
              </td>
              <td className="text-right" title="Inclusive time; self time in brackets">
                {executed ? (
                  <>
                    {fmtMs(a!.timeMs)} <span className="text-haze">({fmtMs(a!.selfTimeMs)})</span>
                  </>
                ) : (
                  '—'
                )}
              </td>
            </tr>
          </tbody>
        </table>
        {executed && !a!.completed && (
          <p className="mt-1.5 text-xs text-haze">This operator stopped early because its parent needed no more rows, so its actual count is not comparable to the estimate.</p>
        )}
        {executed && a!.completed && misestimate(node.estimates.rows, a!.rowsOut ?? 0) >= 2 && (
          <p className="mt-1.5 text-xs text-coral">
            The estimate is off by {misestimate(node.estimates.rows, a!.rowsOut ?? 0).toFixed(1)}×. Statistics assume values are evenly distributed and
            conditions are independent, which this data does not satisfy.
          </p>
        )}
        {typeof node.details.selectivityEstimate === 'string' && (
          <p className="mt-1.5 text-xs text-haze">
            <span className="text-chalk">How the estimate was made: </span>
            <span className="code">{node.details.selectivityEstimate}</span>
          </p>
        )}
        <p className="mt-1.5 text-xs text-haze">Costs are abstract planner units, not milliseconds. Actual numbers were measured during this run.</p>
      </section>

      {executed && (
        <section>
          <h4 className="text-sm font-medium mb-1">Measured work</h4>
          <dl>
            <Row k="Rows in" v={fmtInt(a!.rowsIn)} />
            {(a!.rowsScanned ?? 0) > 0 && <Row k="Rows examined" v={fmtInt(a!.rowsScanned)} />}
            {(a!.rowsFiltered ?? 0) > 0 && <Row k="Rows rejected" v={`${fmtInt(a!.rowsFiltered)} (${pct(a!.rowsFiltered!, (a!.rowsFiltered ?? 0) + (a!.rowsOut ?? 0))})`} />}
            {(a!.pagesRead ?? 0) > 0 && <Row k="Page reads" v={`${fmtInt(a!.pagesRead)}, ${fmtInt(a!.pageMisses)} from the page store`} />}
            {(a!.indexEntriesExamined ?? 0) > 0 && <Row k="Index work" v={`${fmtInt(a!.indexNodesVisited)} nodes, ${fmtInt(a!.indexEntriesExamined)} entries`} />}
            {(a!.buildRows ?? 0) > 0 && <Row k="Rows buffered" v={fmtInt(a!.buildRows)} />}
            {(a!.groups ?? 0) > 0 && <Row k="Distinct keys" v={fmtInt(a!.groups)} />}
            {(a!.peakMemoryBytes ?? 0) > 0 && <Row k="Memory (approx.)" v={fmtBytes(a!.peakMemoryBytes)} />}
            <Row k="next() calls" v={fmtInt(a!.nextCalls)} />
          </dl>
        </section>
      )}

      {details.length > 0 && (
        <section>
          <h4 className="text-sm font-medium mb-1">Details</h4>
          <dl>
            {details.map(([k, v]) => (
              <Row key={k} k={DETAIL_LABELS[k] ?? k} mono={MONO.has(k)} v={Array.isArray(v) ? (v.length ? v.join(', ') : 'none') : v === null ? 'none' : String(v)} />
            ))}
          </dl>
        </section>
      )}

      <section>
        <h4 className="text-sm font-medium mb-1">Schemas</h4>
        <dl>
          <Row k="Input" mono v={input.length ? input.map((c) => `${c.qualifier ? c.qualifier + '.' : ''}${c.name} ${c.type.toLowerCase()}`).join(', ') : node.children.length ? 'no columns' : 'reads from storage'} />
          <Row k="Output" mono v={node.output.length ? node.output.map((c) => `${c.name} ${c.type.toLowerCase()}`).join(', ') : 'no columns (row count only)'} />
          <Row k="Parent" v={parent ? parent.op : 'none (returns rows to the client)'} />
          <Row k="Children" v={node.children.length ? node.children.map((c) => c.op).join(', ') : 'none'} />
        </dl>
      </section>

      {(related.length > 0 || node.notes.length > 0) && (
        <section>
          <h4 className="text-sm font-medium mb-1">Optimizer decisions</h4>
          <ul className="space-y-2">
            {related.map((s) => (
              <li key={s.index} className="text-sm">
                <span className="text-flow">{s.rule}</span>
                <span className="text-haze">: {s.reason}</span>
                {s.alternatives.length > 0 && (
                  <ul className="mt-1 ml-3 space-y-0.5">
                    {s.alternatives.map((alt, i) => (
                      <li key={i} className={`text-xs ${alt.chosen ? 'text-chalk' : 'text-haze'}`}>
                        {alt.chosen ? 'Chosen: ' : ''}
                        {alt.name}
                        {alt.cost > 0 ? `, cost ${fmtCost(alt.cost)}` : ''} ({alt.detail})
                      </li>
                    ))}
                  </ul>
                )}
              </li>
            ))}
            {node.notes
              .filter((n) => !n.endsWith('changed this operator'))
              .map((n, i) => (
                <li key={`n${i}`} className="text-sm text-haze">
                  {n}
                </li>
              ))}
          </ul>
        </section>
      )}
    </div>
  );
}
