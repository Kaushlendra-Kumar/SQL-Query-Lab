import { useMemo } from 'react';
import {
  BaseEdge,
  Controls,
  Handle,
  Position,
  ReactFlow,
  getBezierPath,
  type Edge,
  type EdgeProps,
  type Node,
  type NodeProps,
} from '@xyflow/react';
import type { PlanNode } from '../types/api';
import type { ReplayMap } from '../hooks/useReplay';
import { FAMILY_COLOR, familyOf, fmtEst, fmtInt, fmtMs, fmtCost, misestimate } from './format';
import { flatten, layoutTree } from './treeLayout';

const NODE_W = 236;
const NODE_H = 92;

interface OpData extends Record<string, unknown> {
  plan: PlanNode;
  showActual: boolean;
  selected: boolean;
  highlighted: boolean;
  replay?: ReplayMap;
  compact: boolean;
}

/** The one-line detail shown under the operator name. */
function detailLine(n: PlanNode): string {
  const d = n.details as Record<string, string | string[] | number>;
  if (d.index) return `${d.index}: ${d.indexCondition}`;
  if (d.table) return `${d.table}${d.alias && d.alias !== d.table ? ` ${d.alias}` : ''}${d.filter || d.pushedPredicate ? ` where ${d.filter ?? d.pushedPredicate}` : ''}`;
  if (d.predicate) return String(d.predicate);
  if (d.hashKeys) return (d.hashKeys as string[]).join(' AND ');
  if (d.condition) return String(d.condition);
  if (d.keys) return (d.keys as string[]).join(', ');
  if (d.groupBy || d.aggregates) return [...((d.groupBy as string[]) ?? []), ...((d.aggregates as string[]) ?? [])].join(', ');
  if (d.expressions) return (d.expressions as string[]).join(', ');
  if (d.limit !== undefined) return `limit ${d.limit ?? 'all'}${d.offset ? ` offset ${d.offset}` : ''}`;
  return '';
}

function OpNode({ data }: NodeProps<Node<OpData>>) {
  const { plan, showActual, selected, highlighted, replay, compact } = data;
  const color = FAMILY_COLOR[familyOf(plan.op)];
  const a = plan.actual;
  const executed = !!a?.executed;
  const rep = replay?.get(plan.id);
  const factor = executed && showActual && a!.completed ? misestimate(plan.estimates.rows, a!.rowsOut ?? 0) : 1;
  const logical = plan.kind === 'logical';

  let phaseRing = '';
  if (replay) {
    if (!rep || rep.phase === 'waiting') phaseRing = 'opacity-45';
    else if (rep.phase === 'producing' || rep.phase === 'open') phaseRing = 'ring-2 ring-offset-0';
  }

  return (
    <div
      className={`relative rounded-md border bg-slab text-left transition-shadow ${selected ? 'border-chalk' : 'border-rule'} ${phaseRing} ${
        logical ? 'border-dashed' : ''
      }`}
      style={{ width: NODE_W, minHeight: compact ? 70 : NODE_H, ...(phaseRing.includes('ring') ? { ['--tw-ring-color' as string]: color } : {}) }}
    >
      <Handle type="source" position={Position.Top} className="!bg-transparent !border-0" />
      <Handle type="target" position={Position.Bottom} className="!bg-transparent !border-0" />
      <div className="absolute left-0 top-0 bottom-0 w-1 rounded-l-md" style={{ background: color }} />
      <div className="pl-3 pr-2 py-1.5">
        <div className="flex items-center justify-between gap-2">
          <span className="font-semibold text-sm" style={{ color }}>
            {plan.op}
          </span>
          {highlighted && <span className="text-[0.65rem] text-flow">changed</span>}
          {factor >= 2 && !replay && (
            <span className="text-[0.65rem] text-coral" title="Estimated and actual row counts differ by 2× or more">
              {factor >= 10 ? `${Math.round(factor)}×` : `${factor.toFixed(1)}×`} off
            </span>
          )}
        </div>
        <div className="code text-haze truncate" title={detailLine(plan)}>
          {detailLine(plan) || ' '}
        </div>
        <div className="mt-1 flex items-baseline gap-3 text-xs">
          {replay ? (
            <>
              <span className="text-chalk">{fmtInt(rep?.rows ?? 0)} rows</span>
              <span className="text-haze">{rep ? (rep.note ?? rep.phase) : 'waiting'}</span>
            </>
          ) : showActual && executed ? (
            <>
              <span className="text-chalk" title="Actual rows produced">
                {fmtInt(a!.rowsOut)} rows
              </span>
              <span className="text-haze" title="Optimizer estimate">
                est {fmtEst(plan.estimates.rows)}
              </span>
              <span className="text-haze ml-auto" title="Inclusive time (includes children)">
                {fmtMs(a!.timeMs)}
              </span>
            </>
          ) : (
            <>
              <span className="text-chalk" title="Estimated rows">
                est {fmtEst(plan.estimates.rows)} rows
              </span>
              <span className="text-haze ml-auto" title="Estimated cumulative cost (abstract units)">
                cost {fmtCost(plan.estimates.cost)}
              </span>
            </>
          )}
        </div>
        {!compact && showActual && executed && !replay && ((a!.pagesRead ?? 0) > 0 || (a!.indexEntriesExamined ?? 0) > 0) && (
          <div className="text-[0.7rem] text-haze">
            {fmtInt(a!.pagesRead)} page reads{(a!.indexEntriesExamined ?? 0) > 0 ? `, ${fmtInt(a!.indexEntriesExamined)} index entries` : ''}
          </div>
        )}
      </div>
    </div>
  );
}

interface FlowData extends Record<string, unknown> {
  estRows: number;
  actRows: number | null;
  color: string;
  live: boolean;
}

/** Width of a "pipe" for a row count: logarithmic so 10 and 1,000,000 both read. */
function pipeWidth(rows: number): number {
  return 1.5 + 2.4 * Math.log10(Math.max(rows, 0) + 1);
}

/** Rows flowing from a child into its parent. The faint wide pipe is the
 *  estimate; the solid one is what actually flowed. */
function FlowEdge({ sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition, data }: EdgeProps<Edge<FlowData>>) {
  const [path, lx, ly] = getBezierPath({ sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition });
  const d = data!;
  const actual = d.actRows;
  return (
    <>
      <BaseEdge path={path} style={{ stroke: '#3A4762', strokeWidth: pipeWidth(d.estRows), strokeDasharray: actual === null ? '6 5' : undefined, opacity: actual === null ? 1 : 0.55 }} />
      {actual !== null && <BaseEdge path={path} style={{ stroke: d.color, strokeWidth: pipeWidth(actual), opacity: d.live ? 1 : 0.85 }} />}
      <foreignObject x={lx + 6} y={ly - 10} width={110} height={20} className="pointer-events-none overflow-visible">
        <div className="text-[0.68rem] text-haze whitespace-nowrap">{actual !== null ? fmtInt(actual) : fmtEst(d.estRows)}</div>
      </foreignObject>
    </>
  );
}

const nodeTypes = { op: OpNode };
const edgeTypes = { flow: FlowEdge };

interface Props {
  plan: PlanNode;
  showActual?: boolean;
  selectedId?: number | null;
  onSelect?: (id: number | null) => void;
  highlightIds?: number[];
  replay?: ReplayMap;
  compact?: boolean;
  className?: string;
}

export function PlanGraph({ plan, showActual = true, selectedId = null, onSelect, highlightIds = [], replay, compact = false, className }: Props) {
  const { nodes, edges } = useMemo(() => {
    const placed = layoutTree(plan, NODE_W, compact ? 70 : NODE_H, 26, compact ? 46 : 64);
    const nodes: Node<OpData>[] = placed.map((p) => ({
      id: String(p.node.id),
      type: 'op',
      position: { x: p.x, y: p.y },
      data: {
        plan: p.node,
        showActual,
        selected: p.node.id === selectedId,
        highlighted: highlightIds.includes(p.node.kind === 'physical' ? (p.node.logicalId ?? -1) : p.node.id),
        replay,
        compact,
      },
      draggable: false,
    }));
    const edges: Edge<FlowData>[] = [];
    for (const n of flatten(plan)) {
      for (const c of n.children) {
        const rep = replay?.get(c.id);
        const act = replay ? (rep?.rows ?? 0) : showActual && c.actual?.executed ? (c.actual.rowsOut ?? 0) : null;
        edges.push({
          id: `${c.id}-${n.id}`,
          source: String(c.id),
          target: String(n.id),
          type: 'flow',
          data: {
            estRows: c.estimates.rows,
            actRows: act,
            color: FAMILY_COLOR[familyOf(c.op)],
            live: !!rep && rep.phase === 'producing',
          },
        });
      }
    }
    return { nodes, edges };
  }, [plan, showActual, selectedId, highlightIds, replay, compact]);

  return (
    <div className={className ?? 'h-full w-full'}>
      <ReactFlow
        nodes={nodes}
        edges={edges}
        nodeTypes={nodeTypes}
        edgeTypes={edgeTypes}
        fitView
        fitViewOptions={{ padding: 0.15, maxZoom: 1.1, minZoom: compact ? 0.45 : 0.7 }}
        minZoom={0.2}
        maxZoom={1.8}
        nodesConnectable={false}
        elementsSelectable
        onNodeClick={(_, n) => onSelect?.(Number(n.id))}
        onPaneClick={() => onSelect?.(null)}
        proOptions={{ hideAttribution: false }}
      >
        {!compact && <Controls showInteractive={false} position="bottom-right" />}
      </ReactFlow>
    </div>
  );
}
