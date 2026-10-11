import { useMemo, useState } from 'react';
import { useCurrentStatement, useStore } from '../store';
import { PlanGraph } from '../visualizations/PlanGraph';
import { AstGraph, SourceView, nodeAt } from '../visualizations/AstGraph';
import { OperatorInspector } from '../components/OperatorInspector';
import { EmptyState, Segmented } from '../components/common';
import { useReplay } from '../hooks/useReplay';
import { FAMILY_COLOR, FAMILY_NAME, type Family } from '../visualizations/format';
import { flatten } from '../visualizations/treeLayout';
import type { AstNode, PlanNode } from '../types/api';

function Legend() {
  return (
    <div className="space-y-4 text-sm">
      <p className="text-haze leading-relaxed">
        Select an operator to inspect it. Rows flow upward: each pipe connects an operator to the one that consumes its rows.
      </p>
      <ul className="space-y-1.5">
        {(Object.keys(FAMILY_COLOR) as Family[]).map((f) => (
          <li key={f} className="flex items-center gap-2">
            <span className="h-2.5 w-2.5 rounded-full" style={{ background: FAMILY_COLOR[f] }} />
            <span className="text-haze">{FAMILY_NAME[f]}</span>
          </li>
        ))}
      </ul>
      <div className="space-y-2">
        <svg width="200" height="44" aria-hidden>
          <path d="M8 12 H190" stroke="#3A4762" strokeWidth="9" opacity="0.55" />
          <path d="M8 12 H190" stroke="#63CDB0" strokeWidth="4" />
          <path d="M8 34 H190" stroke="#3A4762" strokeWidth="5" strokeDasharray="6 5" />
        </svg>
        <p className="text-xs text-haze leading-relaxed">
          Pipe width grows with row count (log scale). The faint wide pipe is the optimizer's estimate; the coloured one is what actually flowed.
          A dashed pipe means the query was planned but not run.
        </p>
      </div>
    </div>
  );
}

function ReplayBar({ replay, truncated, note }: { replay: ReturnType<typeof useReplay>; truncated: boolean; note: string }) {
  const cur = replay.current;
  return (
    <div className="flex flex-wrap items-center gap-2 px-3 py-2 border-b border-rule bg-slab/50 text-sm">
      <span className="text-haze mr-1" title={note}>
        Replay of {replay.total.toLocaleString()} events recorded during execution{truncated ? ' (later row events not recorded)' : ''}
      </span>
      {!replay.active && !replay.playing ? (
        <button className="btn" onClick={replay.start} disabled={replay.total === 0}>
          Start replay
        </button>
      ) : replay.playing ? (
        <button className="btn" onClick={replay.pause}>
          Pause
        </button>
      ) : (
        <button className="btn" onClick={replay.resume} disabled={replay.cursor >= replay.total - 1}>
          Resume
        </button>
      )}
      <button className="btn" onClick={replay.step} disabled={replay.cursor >= replay.total - 1}>
        Next event
      </button>
      <button className="btn" onClick={replay.reset} disabled={!replay.active}>
        Reset
      </button>
      <label className="flex items-center gap-1.5 text-xs text-haze">
        Speed
        <input type="range" min={5} max={400} value={replay.speed} onChange={(e) => replay.setSpeed(Number(e.target.value))} aria-label="Replay speed" />
      </label>
      <input
        type="range"
        className="flex-1 min-w-[120px]"
        min={-1}
        max={Math.max(0, replay.total - 1)}
        value={replay.cursor}
        onChange={(e) => replay.seek(Number(e.target.value))}
        aria-label="Replay position"
      />
      {cur && (
        <span className="code text-xs text-haze w-60 truncate">
          #{cur.seq} node {cur.nodeId} {cur.type}
          {cur.type === 'row' ? ` ${cur.rows}` : ''} at {cur.tMs.toFixed(3)} ms
        </span>
      )}
    </div>
  );
}

export function ExplainPage() {
  const r = useCurrentStatement();
  const planView = useStore((s) => s.planView);
  const setPlanView = useStore((s) => s.setPlanView);
  const showActual = useStore((s) => s.showActual);
  const setShowActual = useStore((s) => s.setShowActual);
  const selectedId = useStore((s) => s.selectedNodeId);
  const selectNode = useStore((s) => s.selectNode);
  const setPage = useStore((s) => s.setPage);
  const [logicalStage, setLogicalStage] = useState<'before' | 'after'>('after');
  const [astSel, setAstSel] = useState<AstNode | null>(null);
  const replay = useReplay(r?.events?.events);

  const plan: PlanNode | undefined =
    planView === 'physical' ? r?.physicalPlan : logicalStage === 'before' ? r?.logicalPlan : r?.optimizedPlan;
  const selected = useMemo(() => (plan && selectedId !== null ? flatten(plan).find((n) => n.id === selectedId) ?? null : null), [plan, selectedId]);
  const changed = useMemo(() => (r?.optimizer ?? []).filter((s) => s.applied && s.phase === 'logical').flatMap((s) => s.nodeIds), [r]);

  if (!r) {
    return <EmptyState title="Nothing to explain yet" action={<button className="btn btn-primary" onClick={() => setPage('editor')}>Go to the editor</button>}>Run a query and its plan appears here.</EmptyState>;
  }
  if (!r.ast) {
    return <EmptyState title="This statement could not be parsed">Fix the syntax error shown in the editor and run it again.</EmptyState>;
  }

  const executed = !!r.physicalPlan?.actual;
  const offset = r.span?.begin ?? 0;

  return (
    <div className="flex h-full min-h-0">
      <div className="flex-1 min-w-0 flex flex-col">
        <div className="flex flex-wrap items-center gap-3 px-3 py-2 border-b border-rule">
          <Segmented
            value={planView}
            onChange={setPlanView}
            options={[
              { value: 'physical', label: 'Physical plan' },
              { value: 'logical', label: 'Logical plan' },
              { value: 'ast', label: 'Syntax tree' },
            ]}
          />
          {planView === 'logical' && (
            <Segmented
              value={logicalStage}
              onChange={setLogicalStage}
              options={[
                { value: 'before', label: 'Before optimization' },
                { value: 'after', label: 'After optimization' },
              ]}
            />
          )}
          {planView === 'physical' && executed && (
            <Segmented
              value={showActual ? 'actual' : 'estimated'}
              onChange={(v) => setShowActual(v === 'actual')}
              options={[
                { value: 'actual', label: 'Actual' },
                { value: 'estimated', label: 'Estimated' },
              ]}
            />
          )}
          <span className="code text-xs text-haze truncate max-w-[40ch]" title={r.sql}>
            {r.sql.replace(/\s+/g, ' ')}
          </span>
          {!executed && planView !== 'ast' && r.physicalPlan && <span className="text-xs text-haze">Planned only: numbers are estimates.</span>}
        </div>

        {planView === 'physical' && executed && r.events && (
          <ReplayBar replay={replay} truncated={r.events.truncated} note={r.events.note} />
        )}

        <div className="flex-1 min-h-0 relative">
          {planView === 'ast' ? (
            <div className="h-full flex flex-col">
              <div className="flex-1 min-h-0">
                <AstGraph ast={r.ast} selectedId={astSel?.id ?? null} onSelect={setAstSel} />
              </div>
              <div className="p-3 border-t border-rule max-h-[35%] overflow-auto">
                <p className="text-xs text-haze mb-1">Click a token to find its node. The selected node's source is highlighted.</p>
                <SourceView
                  sql={r.sql}
                  offset={offset}
                  tokens={r.tokens ?? []}
                  span={astSel?.span}
                  onPick={(pos) => setAstSel(nodeAt(r.ast!, pos))}
                />
              </div>
            </div>
          ) : plan ? (
            <PlanGraph
              plan={plan}
              showActual={planView === 'physical' && showActual}
              selectedId={selectedId}
              onSelect={selectNode}
              highlightIds={planView === 'logical' && logicalStage === 'after' ? changed : []}
              replay={planView === 'physical' && replay.active ? replay.state : undefined}
            />
          ) : (
            <EmptyState title="No query plan for this statement">{r.kind} statements run directly against the catalog.</EmptyState>
          )}
        </div>
      </div>

      <aside className="w-[22rem] shrink-0 border-l border-rule p-4 overflow-auto" aria-label="Inspector">
        {planView === 'ast' ? (
          astSel ? (
            <div className="space-y-3">
              <h3 className="text-lg font-semibold">{astSel.type}</h3>
              <p className="code text-chalk break-words">{astSel.label}</p>
              <dl className="text-sm space-y-1">
                <div>
                  <dt className="text-haze inline">Source location: </dt>
                  <dd className="inline">
                    line {astSel.span.line}, column {astSel.span.column} (bytes {astSel.span.begin}–{astSel.span.end})
                  </dd>
                </div>
                {Object.entries(astSel.details).map(([k, v]) => (
                  <div key={k}>
                    <dt className="text-haze inline">{k}: </dt>
                    <dd className="inline code">{typeof v === 'object' ? JSON.stringify(v) : String(v)}</dd>
                  </div>
                ))}
                <div>
                  <dt className="text-haze inline">Children: </dt>
                  <dd className="inline">{astSel.children.length}</dd>
                </div>
              </dl>
              <p className="text-xs text-haze">
                Types are resolved later by the binder; the syntax tree only records what was written.
              </p>
            </div>
          ) : (
            <p className="text-sm text-haze leading-relaxed">
              This is the tree the C++ parser built from your SQL. Select a node to see its source location and details.
            </p>
          )
        ) : selected && plan ? (
          <OperatorInspector node={selected} root={plan} steps={r.optimizer ?? []} showActual={showActual} />
        ) : (
          <Legend />
        )}
      </aside>
    </div>
  );
}
