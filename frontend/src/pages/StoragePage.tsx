import { useCallback, useEffect, useState } from 'react';
import { Bar, BarChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts';
import { api } from '../api/client';
import { useStore } from '../store';
import { EmptyState, Stat } from '../components/common';
import type { IndexDetail, PageDetail, StorageInfo, TableSchema } from '../types/api';
import { cellText, fmtBytes, fmtInt, pct } from '../visualizations/format';

function PageMap({ table, onPick, selected }: { table: StorageInfo['tables'][number]; onPick: (i: number) => void; selected: number | null }) {
  return (
    <div>
      <div className="flex flex-wrap gap-[3px]" role="grid" aria-label={`Pages of ${table.name}`}>
        {table.pages.map((p, i) => {
          const fill = 1 - p.freeBytes / 4096;
          return (
            <button
              key={p.pageId}
              onClick={() => onPick(i)}
              title={`Page ${p.pageId}: ${p.live} live rows, ${p.freeBytes} bytes free${p.cached ? ', in buffer pool' : ''}`}
              className={`relative h-7 w-7 rounded-sm bg-well border ${selected === i ? 'border-chalk' : p.cached ? 'border-join' : 'border-rule'}`}
              aria-label={`Page ${i}`}
            >
              <span className="absolute bottom-0 left-0 right-0 bg-scan/70 rounded-b-sm" style={{ height: `${Math.round(fill * 100)}%` }} />
            </button>
          );
        })}
      </div>
      <p className="mt-2 text-xs text-haze">
        Each square is a 4 KB page; the amber fill is how full it is. A blue outline means the page is currently in the buffer pool.
        {table.pageCount > table.pages.length ? ` Showing the first ${table.pages.length} of ${fmtInt(table.pageCount)} pages.` : ''}
      </p>
    </div>
  );
}

function PageView({ page, columns }: { page: PageDetail; columns: string[] }) {
  const used = page.pageSize - page.freeBytes;
  return (
    <div className="space-y-3">
      <div className="grid grid-cols-4 gap-3">
        <Stat label="Page id" value={page.pageId} />
        <Stat label="Slots" value={`${page.slotCount} (${page.liveRows} live)`} />
        <Stat label="Used" value={`${fmtBytes(used)} of 4 KB`} />
        <Stat label="Was cached" value={page.wasCached ? 'yes' : 'no, loaded now'} />
      </div>
      <div className="h-3 w-full rounded-sm bg-well border border-rule flex overflow-hidden" aria-hidden>
        <div className="bg-join/60" style={{ width: `${((16 + page.slotCount * 4) / page.pageSize) * 100}%` }} title="Header and slot directory" />
        <div className="flex-1" title="Free space" />
        <div className="bg-scan/70" style={{ width: `${((used - 16 - page.slotCount * 4) / page.pageSize) * 100}%` }} title="Row data" />
      </div>
      <p className="text-xs text-haze">Slotted layout: the slot directory grows from the front, row data from the back. Deleted rows leave tombstones.</p>
      <div className="max-h-80 overflow-auto border border-rule rounded">
        <table className="text-sm min-w-full">
          <thead className="sticky top-0 bg-slab">
            <tr className="text-left text-xs text-haze">
              <th className="px-2 py-1 font-normal">Slot</th>
              <th className="px-2 py-1 font-normal">Bytes</th>
              {columns.map((c) => (
                <th key={c} className="px-2 py-1 font-normal">
                  {c}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {page.slots.map((s) => (
              <tr key={s.slot} className={s.live ? '' : 'text-haze/60'}>
                <td className="px-2 py-0.5">{s.slot}</td>
                <td className="px-2 py-0.5">{s.live ? s.bytes : 'deleted'}</td>
                {s.live && s.values?.map((v, i) => (
                  <td key={i} className="px-2 py-0.5 code whitespace-nowrap">
                    {cellText(v)}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

function BTree({ detail }: { detail: IndexDetail }) {
  return (
    <div className="space-y-3">
      <div className="grid grid-cols-4 gap-3">
        <Stat label="Height" value={detail.height} />
        <Stat label="Entries" value={fmtInt(detail.entries)} />
        <Stat label="Nodes" value={`${fmtInt(detail.nodeCount)} (${fmtInt(detail.leafCount)} leaves)`} />
        <Stat label="Size (approx.)" value={fmtBytes(detail.approximateBytes)} />
      </div>
      <p className="code text-sm text-reshape">{detail.definition}</p>
      <div className="space-y-2 overflow-x-auto">
        {detail.levels.map((lvl) => (
          <div key={lvl.depth}>
            <div className="text-xs text-haze mb-1">
              {lvl.depth === 0 ? 'Root' : lvl.leaf ? 'Leaves (linked left to right)' : `Level ${lvl.depth}`}, {fmtInt(lvl.nodeCount)} node{lvl.nodeCount === 1 ? '' : 's'}
            </div>
            <div className="flex gap-1.5">
              {lvl.nodes.map((n, i) => (
                <div key={i} className={`shrink-0 rounded border px-1.5 py-1 code text-[0.7rem] ${n.leaf ? 'border-reshape/40' : 'border-rule'} bg-well`}>
                  {n.keys.join(' | ')}
                  {n.truncated ? ' …' : ''}
                  <span className="text-haze"> ({n.entryCount})</span>
                </div>
              ))}
              {lvl.nodeCount > lvl.nodes.length && <span className="text-xs text-haze self-center">+{fmtInt(lvl.nodeCount - lvl.nodes.length)} more</span>}
            </div>
          </div>
        ))}
      </div>
      <p className="text-xs text-haze">
        Internal nodes hold separator keys; leaves hold every (key, row id) pair in order. Duplicate keys sit next to each other, ordered by row id.
        {detail.valid ? ' Structural check passed.' : ' Structural check FAILED.'}
      </p>
    </div>
  );
}

export function StoragePage() {
  const catalog = useStore((s) => s.catalog);
  const [info, setInfo] = useState<StorageInfo | null>(null);
  const [table, setTable] = useState<string | null>(null);
  const [schema, setSchema] = useState<TableSchema | null>(null);
  const [page, setPage] = useState<{ index: number; detail: PageDetail } | null>(null);
  const [index, setIndex] = useState<IndexDetail | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    try {
      const i = await api.storage();
      setInfo(i);
      setError(null);
      setTable((t) => t ?? i.tables[0]?.name ?? null);
    } catch (e) {
      setError((e as Error).message);
    }
  }, []);
  useEffect(() => {
    void load();
  }, [load, catalog]);

  useEffect(() => {
    setPage(null);
    setIndex(null);
    if (!table) return;
    api.schema(table).then(setSchema).catch((e) => setError(e.message));
  }, [table]);

  if (error) return <EmptyState title="Storage information is unavailable">{error}</EmptyState>;
  if (!info) return <EmptyState title="Loading storage information" />;
  if (info.tables.length === 0) return <EmptyState title="No tables yet">Load a sample dataset from the sidebar to explore pages, the buffer pool and indexes.</EmptyState>;

  const t = info.tables.find((x) => x.name === table) ?? info.tables[0];
  const tableIndexes = catalog?.tables.find((x) => x.name === t.name)?.indexes ?? [];
  const bp = info.bufferPool;

  return (
    <div className="h-full overflow-auto">
      <div className="p-4 space-y-6 max-w-6xl">
        <section className="flex flex-wrap items-end gap-6">
          <label className="block">
            <span className="label">Table</span>
            <select className="input block mt-0.5" value={t.name} onChange={(e) => setTable(e.target.value)}>
              {info.tables.map((x) => (
                <option key={x.name} value={x.name}>
                  {x.name}
                </option>
              ))}
            </select>
          </label>
          <Stat label="Rows" value={fmtInt(t.rowCount)} />
          <Stat label="Pages" value={fmtInt(t.pageCount)} />
          <Stat label="Rows per page" value={t.rowsPerPage.toFixed(1)} />
          <Stat label="Pages cached" value={`${fmtInt(t.cachedPages)} of ${fmtInt(t.pageCount)}`} />
          <button className="btn ml-auto" onClick={load}>
            Refresh
          </button>
        </section>

        <section className="panel p-4">
          <h2 className="font-medium mb-1">Buffer pool</h2>
          <div className="grid grid-cols-2 sm:grid-cols-5 gap-4">
            <Stat label="Capacity" value={`${fmtInt(bp.capacity)} pages (${fmtBytes(bp.capacity * 4096)})`} />
            <Stat label="Cached now" value={fmtInt(bp.cached)} />
            <Stat label="Hit rate (all time)" value={pct(bp.hits, bp.logicalReads)} />
            <Stat label="Evictions" value={fmtInt(bp.evictions)} />
            <Stat label="Pages written back" value={fmtInt(bp.writes)} />
          </div>
          <p className="mt-2 text-xs text-haze">{info.note}</p>
        </section>

        <section>
          <h2 className="font-medium mb-2">Pages of {t.name}</h2>
          <PageMap
            table={t}
            selected={page?.index ?? null}
            onPick={async (i) => {
              try {
                setPage({ index: i, detail: await api.page(t.name, i) });
                void load();
              } catch (e) {
                setError((e as Error).message);
              }
            }}
          />
          {page && (
            <div className="mt-4 panel p-4">
              <PageView page={page.detail} columns={schema?.columns.map((c) => c.name) ?? []} />
            </div>
          )}
        </section>

        {schema && (
          <section>
            <h2 className="font-medium mb-1">Column statistics</h2>
            <p className="text-xs text-haze mb-3">What the optimizer knows about each column. Histograms are equi-depth: each bar aims to cover the same number of rows, but one frequent value always gets a bar of its own.</p>
            <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
              {schema.columns.map((c) => (
                <div key={c.name} className="panel p-3">
                  <div className="flex items-baseline justify-between">
                    <span className="code text-chalk">{c.name}</span>
                    <span className="text-xs text-haze">{c.type.toLowerCase()}</span>
                  </div>
                  <div className="text-xs text-haze mt-0.5">
                    {fmtInt(c.stats.distinct)} distinct, {fmtInt(c.stats.nulls)} NULL, range {cellText(c.stats.min ?? null)} to {cellText(c.stats.max ?? null)}
                  </div>
                  {c.histogram.length > 1 && (
                    <div className="h-24 mt-2">
                      <ResponsiveContainer width="100%" height="100%">
                        <BarChart data={c.histogram.map((b) => ({ range: `${cellText(b.low)}–${cellText(b.high)}`, count: b.count }))}>
                          <XAxis dataKey="range" hide />
                          <YAxis hide />
                          <Tooltip
                            cursor={{ fill: '#243049' }}
                            contentStyle={{ background: '#1C2433', border: '1px solid #2A3448', fontSize: 12 }}
                            labelStyle={{ color: '#DDE4EF' }}
                          />
                          <Bar dataKey="count" fill="#E9B44C" fillOpacity={0.75} />
                        </BarChart>
                      </ResponsiveContainer>
                    </div>
                  )}
                </div>
              ))}
            </div>
          </section>
        )}

        <section>
          <h2 className="font-medium mb-2">Indexes on {t.name}</h2>
          {tableIndexes.length === 0 ? (
            <p className="text-sm text-haze">No indexes. Create one with CREATE INDEX in the editor, or from an index advisor suggestion.</p>
          ) : (
            <div className="flex gap-2 mb-3 flex-wrap">
              {tableIndexes.map((i) => (
                <button key={i.name} className={`btn ${index?.name === i.name ? 'border-reshape' : ''}`} onClick={async () => setIndex(await api.index(i.name))}>
                  {i.name} ({i.columns.join(', ')})
                </button>
              ))}
            </div>
          )}
          {index && (
            <div className="panel p-4">
              <BTree detail={index} />
            </div>
          )}
        </section>
      </div>
    </div>
  );
}
