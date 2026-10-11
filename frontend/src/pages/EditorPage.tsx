import { useEffect, useState } from 'react';
import { useCurrentStatement, useStore } from '../store';
import { SqlEditor } from '../components/SqlEditor';
import { ErrorBox, PipelineTimeline, Stat, Toggle } from '../components/common';
import { ResultsGrid } from '../components/ResultsGrid';
import { AdvisorPanel, ExplanationPanel } from '../components/Insights';
import { fmtBytes, fmtInt, fmtMs, pct } from '../visualizations/format';
import type { StatementResult } from '../types/api';

type Tab = 'results' | 'explanation' | 'advisor' | 'tokens';

function StatementPicker() {
  const result = useStore((s) => s.result);
  const idx = useStore((s) => s.statementIndex);
  const select = useStore((s) => s.selectStatement);
  if (!result || result.results.length < 2) return null;
  return (
    <div className="flex gap-1 overflow-x-auto px-3 pt-2" aria-label="Statements in the script">
      {result.results.map((r, i) => (
        <button
          key={r.queryId}
          onClick={() => select(i)}
          className={`shrink-0 rounded px-2 py-0.5 text-xs border ${i === idx ? 'border-join text-chalk' : 'border-rule text-haze hover:text-chalk'} ${
            r.success ? '' : 'text-coral'
          }`}
          title={r.sql}
        >
          {i + 1}. {r.kind}
          {r.success ? '' : ' failed'}
        </button>
      ))}
    </div>
  );
}

function RunSummary({ r }: { r: StatementResult }) {
  const m = r.metrics;
  if (!m) {
    return (
      <div className="space-y-2 text-sm">
        <p className="text-chalk">{r.message ?? (r.success ? 'Done.' : 'The statement failed.')}</p>
        {r.affectedRows !== undefined && <Stat label="Rows affected" value={fmtInt(r.affectedRows)} />}
      </div>
    );
  }
  return (
    <div className="space-y-4">
      <div className="grid grid-cols-2 gap-x-4 gap-y-3">
        <Stat label="Rows returned" value={fmtInt(m.rowsReturned)} />
        <Stat label="Execution" value={fmtMs(m.executionMs)} hint="Engine execution only: excludes parsing, planning, JSON and network" />
        <Stat label="Rows examined" value={fmtInt(m.rowsScanned)} hint="Stored rows read by scan operators" />
        <Stat label="Planning" value={fmtMs(m.planningMs)} hint="Lexer + parser + binder + optimizer + physical planner" />
        <Stat label="Page reads" value={fmtInt(m.pagesRead)} hint="Buffer-pool page accesses by operators" />
        <Stat label="Pool misses" value={`${fmtInt(m.bufferPool.misses)} (${pct(m.bufferPool.misses, m.bufferPool.logicalReads)})`} hint="Pages that had to be loaded into the buffer pool" />
        {m.indexEntriesExamined > 0 && <Stat label="Index entries" value={fmtInt(m.indexEntriesExamined)} />}
        {m.totalBufferedBytes > 0 && <Stat label="Buffered (approx.)" value={fmtBytes(m.totalBufferedBytes)} />}
      </div>
      {r.explanation && <p className="text-sm text-haze leading-relaxed">{r.explanation.summary}</p>}
      <p className="text-xs text-haze leading-relaxed">
        {m.timing
          ? `Per-operator timers were read ${fmtInt(m.timerCalls)} times (~${m.timerOverheadNs.toFixed(0)} ns each, about ${fmtMs(m.estimatedTimerOverheadMs)} in total). `
          : 'Per-operator timers were off for this run. '}
        {m.storage === 'memory' ? 'Pages live in memory, so a pool miss is a memory copy, not disk I/O.' : 'Pool misses read from the database file.'}
        {m.coldCache ? ' The buffer pool was emptied before this run.' : ''}
      </p>
    </div>
  );
}

export function EditorPage() {
  const sql = useStore((s) => s.sql);
  const setSql = useStore((s) => s.setSql);
  const run = useStore((s) => s.run);
  const cancel = useStore((s) => s.cancel);
  const running = useStore((s) => s.running);
  const options = useStore((s) => s.options);
  const setOption = useStore((s) => s.setOption);
  const result = useStore((s) => s.result);
  const transportError = useStore((s) => s.transportError);
  const catalog = useStore((s) => s.catalog);
  const setPage = useStore((s) => s.setPage);
  const selectNode = useStore((s) => s.selectNode);
  const r = useCurrentStatement();
  const [tab, setTab] = useState<Tab>('results');

  useEffect(() => {
    if (r && !r.success) setTab('results');
  }, [r]);

  const scriptError = result && !result.success ? result.error : null;
  const tabs: { key: Tab; label: string; show: boolean }[] = [
    { key: 'results', label: r?.columns?.length ? `Results${r.rowCount !== undefined ? ` (${fmtInt(r.rowCount)})` : ''}` : 'Output', show: true },
    { key: 'explanation', label: 'Explanation', show: !!r?.explanation },
    { key: 'advisor', label: `Index advisor${r?.advisor?.suggestions.length ? ` (${r.advisor.suggestions.length})` : ''}`, show: !!r?.advisor },
    { key: 'tokens', label: 'Tokens', show: !!r?.tokens },
  ];

  return (
    <div className="flex h-full min-h-0">
      <div className="flex-1 min-w-0 flex flex-col">
        <div className="h-[38%] min-h-[180px] border-b border-rule">
          <SqlEditor value={sql} onChange={setSql} onRun={() => run()} onExplain={() => run({ explain: true })} error={scriptError} catalog={catalog} />
        </div>
        <div className="flex flex-wrap items-center gap-3 px-3 py-2 border-b border-rule bg-slab/50">
          <button className="btn btn-primary" onClick={() => run()} disabled={running} title="Ctrl/Cmd + Enter">
            {running ? 'Running…' : 'Run'}
          </button>
          <button className="btn" onClick={() => run({ explain: true })} disabled={running} title="Ctrl/Cmd + Shift + Enter">
            Explain without running
          </button>
          {running && (
            <button className="btn" onClick={cancel}>
              Cancel
            </button>
          )}
          <div className="flex flex-wrap gap-4 ml-2">
            <Toggle checked={!!options.disableIndexScans} onChange={(v) => setOption('disableIndexScans', v)} label="No index scans" hint="Force sequential scans to compare plans" />
            <Toggle checked={!!options.disableHashJoin} onChange={(v) => setOption('disableHashJoin', v)} label="No hash joins" hint="Force nested-loop joins" />
            <Toggle checked={!!options.coldCache} onChange={(v) => setOption('coldCache', v)} label="Cold buffer pool" hint="Empty the buffer pool before executing" />
            <Toggle checked={options.timing !== false} onChange={(v) => setOption('timing', v)} label="Operator timers" hint="Time each operator (adds small overhead)" />
          </div>
        </div>
        <div className="px-3 py-2">
          <PipelineTimeline result={r} />
        </div>
        <StatementPicker />
        {transportError && (
          <div className="mx-3 mt-2 rounded-md border border-coral/50 bg-coral/10 px-3 py-2 text-sm" role="alert">
            {transportError}
          </div>
        )}
        {r && (
          <div className="flex items-center border-b border-rule px-2 mt-1" role="tablist">
            {tabs
              .filter((t) => t.show)
              .map((t) => (
                <button key={t.key} role="tab" aria-selected={tab === t.key} className={`tab ${tab === t.key ? 'tab-active' : ''}`} onClick={() => setTab(t.key)}>
                  {t.label}
                </button>
              ))}
            {r.physicalPlan && (
              <button className="ml-auto text-sm text-join hover:underline px-2" onClick={() => setPage('explain')}>
                Open the plan graph
              </button>
            )}
          </div>
        )}
        <div className="flex-1 min-h-0 overflow-auto">
          {!r && !transportError && (
            <div className="p-6 text-sm text-haze max-w-xl leading-relaxed">
              Write SQL above and press Run (Ctrl/Cmd + Enter). Each run shows the result, how the query moved through the pipeline, the plan the
              optimizer chose, and what the engine measured while executing it.
            </div>
          )}
          {r && !r.success && r.error && (
            <div className="p-3">
              <ErrorBox error={r.error} sql={sql} />
            </div>
          )}
          {r && r.success && tab === 'results' && (r.explain ? <div className="p-4 text-sm text-haze">{r.message} Open Visual Explain to see the plan.</div> : <ResultsGrid result={r} />)}
          {r && tab === 'explanation' && r.explanation && (
            <div className="p-4">
              <ExplanationPanel
                explanation={r.explanation}
                onFocusNode={(id) => {
                  selectNode(id);
                  setPage('explain');
                }}
              />
            </div>
          )}
          {r && tab === 'advisor' && r.advisor && (
            <div className="p-4">
              <AdvisorPanel advisor={r.advisor} sql={r.sql} />
            </div>
          )}
          {r && tab === 'tokens' && r.tokens && (
            <table className="text-sm m-3">
              <thead>
                <tr className="text-xs text-haze text-left">
                  <th className="pr-4 font-normal">Type</th>
                  <th className="pr-4 font-normal">Text</th>
                  <th className="pr-4 font-normal">Value</th>
                  <th className="font-normal">Line:column</th>
                </tr>
              </thead>
              <tbody>
                {r.tokens.map((t, i) => (
                  <tr key={i}>
                    <td className="pr-4 text-haze">{t.type}</td>
                    <td className="pr-4 code">{t.lexeme}</td>
                    <td className="pr-4 code text-haze">{t.value !== t.lexeme ? t.value : ''}</td>
                    <td className="text-haze">
                      {t.span.line}:{t.span.column}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      </div>
      <aside className="w-80 shrink-0 border-l border-rule p-4 overflow-auto hidden lg:block" aria-label="Run summary">
        <h2 className="text-sm font-medium mb-3">This run</h2>
        {r ? <RunSummary r={r} /> : <p className="text-sm text-haze">Metrics for the selected statement appear here.</p>}
      </aside>
    </div>
  );
}
