// Frontend tests run against a real response captured from the C++ engine
// (fixture_query.json), so they check that the UI renders actual plans.
import { describe, expect, it, beforeAll } from 'vitest';
import { act, render, screen } from '@testing-library/react';
import { renderHook } from '@testing-library/react';
import fixture from './fixture_query.json';
import type { ScriptResult, PlanNode, StatementResult } from '../types/api';
import { PlanGraph } from '../visualizations/PlanGraph';
import { layoutTree, flatten } from '../visualizations/treeLayout';
import { cellText, fmtEst, misestimate, familyOf } from '../visualizations/format';
import { useReplay } from '../hooks/useReplay';
import { OperatorInspector } from '../components/OperatorInspector';
import { ExplanationPanel } from '../components/Insights';

const result = (fixture as unknown as ScriptResult).results[0] as StatementResult;
const plan = result.physicalPlan as PlanNode;

beforeAll(() => {
  // React Flow measures nodes with ResizeObserver / DOMMatrix, absent in jsdom.
  class RO {
    observe() {}
    unobserve() {}
    disconnect() {}
  }
  (globalThis as unknown as { ResizeObserver: unknown }).ResizeObserver = RO;
  (globalThis as unknown as { DOMMatrixReadOnly: unknown }).DOMMatrixReadOnly = class {
    m22 = 1;
  };
});

describe('engine fixture', () => {
  it('is a successful executed query with a physical plan', () => {
    expect(result.success).toBe(true);
    expect(plan.kind).toBe('physical');
    expect(plan.actual?.executed).toBe(true);
  });
});

describe('tree layout', () => {
  it('places parents above children and centres them', () => {
    const placed = layoutTree(plan, 200, 80);
    expect(placed).toHaveLength(flatten(plan).length);
    const byNode = new Map(placed.map((p) => [p.node, p]));
    for (const p of placed) {
      for (const c of p.node.children) expect(byNode.get(c)!.y).toBeGreaterThan(p.y);
      if (p.node.children.length > 1) {
        const xs = p.node.children.map((c) => byNode.get(c)!.x);
        expect(p.x).toBeCloseTo((Math.min(...xs) + Math.max(...xs)) / 2);
      }
    }
  });
});

describe('formatting', () => {
  it('marks estimates and renders values', () => {
    expect(fmtEst(1234.4)).toBe('~1,234');
    expect(cellText(null)).toBe('NULL');
    expect(cellText(0.1 + 0.2)).toBe('0.3');
    expect(cellText(true)).toBe('TRUE');
    expect(misestimate(10, 40)).toBe(4);
    expect(familyOf('Hash Join')).toBe('join');
    expect(familyOf('Index Scan')).toBe('scan');
    expect(familyOf('Hash Aggregate')).toBe('reshape');
  });
});

describe('PlanGraph', () => {
  it('renders every operator of the real plan with actual row counts', () => {
    render(
      <div style={{ width: 1000, height: 800 }}>
        <PlanGraph plan={plan} showActual />
      </div>,
    );
    for (const n of flatten(plan)) {
      expect(screen.getAllByText(n.op).length).toBeGreaterThan(0);
    }
    const scan = flatten(plan).find((n) => n.op === 'Seq Scan')!;
    expect(screen.getAllByText(`${scan.actual!.rowsOut!.toLocaleString('en-US')} rows`).length).toBeGreaterThan(0);
  });

  it('shows estimates, not actuals, in estimated mode', () => {
    render(
      <div style={{ width: 1000, height: 800 }}>
        <PlanGraph plan={plan} showActual={false} />
      </div>,
    );
    expect(screen.getAllByText(/^est ~/).length).toBeGreaterThan(0);
    expect(screen.queryAllByText(/^cost /).length).toBe(flatten(plan).length);
  });
});

describe('OperatorInspector', () => {
  it('separates estimated from measured values', () => {
    const join = flatten(plan).find((n) => n.op === 'Hash Join')!;
    render(<OperatorInspector node={join} root={plan} steps={result.optimizer ?? []} showActual />);
    expect(screen.getByText('Estimated vs actual')).toBeTruthy();
    expect(screen.getByText('Measured work')).toBeTruthy();
    expect(screen.getByText(/JoinStrategy/)).toBeTruthy();
  });
});

describe('ExplanationPanel', () => {
  it('labels observed facts separately from general guidance', () => {
    render(<ExplanationPanel explanation={result.explanation!} />);
    expect(screen.getByText('Observed in this run')).toBeTruthy();
    expect(screen.getByText(result.explanation!.summary)).toBeTruthy();
  });
});

describe('useReplay', () => {
  it('applies recorded events in order and resets', () => {
    const events = result.events!.events;
    const { result: r } = renderHook(() => useReplay(events));
    expect(r.current.active).toBe(false);
    act(() => r.current.step());
    expect(r.current.cursor).toBe(0);
    expect(r.current.state.get(events[0].nodeId)?.phase).toBe('open');
    act(() => r.current.seek(events.length - 1));
    const rowEvents = events.filter((e) => e.type === 'row');
    const last = rowEvents[rowEvents.length - 1];
    expect(r.current.state.get(last.nodeId)!.rows).toBeGreaterThanOrEqual(last.rows);
    act(() => r.current.reset());
    expect(r.current.state.size).toBe(0);
  });
});
