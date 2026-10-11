import { useMemo } from 'react';
import { Handle, Position, ReactFlow, type Edge, type Node, type NodeProps } from '@xyflow/react';
import type { AstNode, Token } from '../types/api';
import { flatten, layoutTree } from './treeLayout';

const W = 176;
const H = 46;

/** AST node categories, coloured to match the operator vocabulary loosely. */
function categoryColor(type: string): string {
  if (type.endsWith('Statement')) return '#DDE4EF';
  if (['SelectList', 'From', 'Where', 'GroupBy', 'Having', 'OrderBy', 'Limit', 'Offset', 'JoinCondition', 'ValuesRow'].includes(type)) return '#8D9AB0';
  if (type === 'TableRef' || type === 'JoinClause') return '#E9B44C';
  if (type === 'ColumnRef' || type === 'Star' || type === 'Alias') return '#63CDB0';
  if (type === 'Literal') return '#B49BEA';
  return '#5FB8E8'; // operators, functions, predicates
}

interface AstData extends Record<string, unknown> {
  ast: AstNode;
  selected: boolean;
}

function AstBox({ data }: NodeProps<Node<AstData>>) {
  const color = categoryColor(data.ast.type);
  return (
    <div
      className={`rounded border bg-slab px-2 py-1 ${data.selected ? 'border-chalk' : 'border-rule'}`}
      style={{ width: W, minHeight: H }}
      title={data.ast.label}
    >
      <Handle type="target" position={Position.Top} className="!bg-transparent !border-0" />
      <Handle type="source" position={Position.Bottom} className="!bg-transparent !border-0" />
      <div className="text-[0.68rem] text-haze">{data.ast.type}</div>
      <div className="code truncate" style={{ color }}>
        {data.ast.label}
      </div>
    </div>
  );
}

const nodeTypes = { ast: AstBox };

export function AstGraph({ ast, selectedId, onSelect }: { ast: AstNode; selectedId: number | null; onSelect: (n: AstNode | null) => void }) {
  const byId = useMemo(() => new Map(flatten(ast).map((n) => [n.id, n])), [ast]);
  const { nodes, edges } = useMemo(() => {
    const placed = layoutTree(ast, W, H, 16, 36);
    const nodes: Node<AstData>[] = placed.map((p) => ({
      id: String(p.node.id),
      type: 'ast',
      position: { x: p.x, y: p.y },
      data: { ast: p.node, selected: p.node.id === selectedId },
      draggable: false,
    }));
    const edges: Edge[] = [];
    for (const n of flatten(ast)) {
      for (const c of n.children) {
        edges.push({ id: `${n.id}-${c.id}`, source: String(n.id), target: String(c.id), style: { stroke: '#3A4762', strokeWidth: 1.5 } });
      }
    }
    return { nodes, edges };
  }, [ast, selectedId]);

  return (
    <ReactFlow
      nodes={nodes}
      edges={edges}
      nodeTypes={nodeTypes}
      fitView
      fitViewOptions={{ padding: 0.12, maxZoom: 1.1, minZoom: 0.7 }}
      minZoom={0.15}
      nodesConnectable={false}
      onNodeClick={(_, n) => onSelect(byId.get(Number(n.id)) ?? null)}
      onPaneClick={() => onSelect(null)}
    />
  );
}

/** SQL text rendered token by token. The selected node's source span is
 *  highlighted; clicking a token selects the innermost node that covers it. */
export function SourceView({
  sql,
  offset,
  tokens,
  span,
  onPick,
}: {
  sql: string;
  offset: number;
  tokens: Token[];
  span?: { begin: number; end: number };
  onPick?: (absolutePos: number) => void;
}) {
  const parts: JSX.Element[] = [];
  let pos = 0;
  const inSpan = (b: number, e: number) => !!span && b >= span.begin - offset && e <= span.end - offset;
  tokens.forEach((t, i) => {
    const b = t.span.begin - offset;
    const e = t.span.end - offset;
    if (b < pos || b > sql.length) return;
    if (b > pos) parts.push(<span key={`g${i}`} className={inSpan(pos, b) ? 'bg-[#2E5C86]/70' : ''}>{sql.slice(pos, b)}</span>);
    parts.push(
      <span
        key={`t${i}`}
        onClick={() => onPick?.(t.span.begin)}
        className={`cursor-pointer rounded-sm ${inSpan(b, e) ? 'bg-[#2E5C86] text-chalk' : 'hover:text-chalk'}`}
        title={`${t.type}: ${t.lexeme}`}
      >
        {sql.slice(b, e)}
      </span>,
    );
    pos = e;
  });
  if (pos < sql.length) parts.push(<span key="tail">{sql.slice(pos)}</span>);
  return <pre className="code whitespace-pre-wrap break-words leading-6 text-haze p-3 bg-well rounded border border-rule overflow-auto">{parts}</pre>;
}

/** Innermost AST node whose span contains `pos`. */
export function nodeAt(root: AstNode, pos: number): AstNode | null {
  if (pos < root.span.begin || pos >= root.span.end) return null;
  for (const c of root.children) {
    const hit = nodeAt(c, pos);
    if (hit) return hit;
  }
  return root;
}
