import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { ExecEvent } from '../types/api';

/** What a node looks like at a given point of the recorded execution. */
export interface NodeReplayState {
  phase: 'waiting' | 'open' | 'producing' | 'done' | 'closed';
  rows: number;
  note?: string; // e.g. "hash table built from 1,000 rows"
}

export type ReplayMap = Map<number, NodeReplayState>;

/**
 * Steps through execution events that the engine recorded on its real
 * execution path. This is a replay after the fact, not live streaming.
 */
export function useReplay(events: ExecEvent[] | undefined) {
  const list = useMemo(() => events ?? [], [events]);
  const [cursor, setCursor] = useState(-1); // index of last applied event
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(40); // events per second
  const timer = useRef<number | null>(null);

  // Reset whenever a new execution arrives.
  useEffect(() => {
    setCursor(-1);
    setPlaying(false);
  }, [list]);

  useEffect(() => {
    if (!playing) return;
    if (cursor >= list.length - 1) {
      setPlaying(false);
      return;
    }
    timer.current = window.setTimeout(() => setCursor((c) => c + 1), 1000 / speed);
    return () => {
      if (timer.current) window.clearTimeout(timer.current);
    };
  }, [playing, cursor, list.length, speed]);

  const state: ReplayMap = useMemo(() => {
    const m: ReplayMap = new Map();
    for (let i = 0; i <= cursor && i < list.length; i++) {
      const e = list[i];
      const prev = m.get(e.nodeId) ?? { phase: 'waiting' as const, rows: 0 };
      switch (e.type) {
        case 'open':
          m.set(e.nodeId, { ...prev, phase: 'open' });
          break;
        case 'row':
          m.set(e.nodeId, { ...prev, phase: 'producing', rows: e.rows });
          break;
        case 'done':
          m.set(e.nodeId, { ...prev, phase: 'done', rows: e.rows });
          break;
        case 'close':
          m.set(e.nodeId, { ...prev, phase: 'closed', rows: e.rows });
          break;
        case 'build_done':
          m.set(e.nodeId, { ...prev, note: `buffered ${e.rows.toLocaleString()} input rows` });
          break;
        case 'sort_done':
          m.set(e.nodeId, { ...prev, note: `sorted ${e.rows.toLocaleString()} rows` });
          break;
        case 'aggregate_done':
          m.set(e.nodeId, { ...prev, note: `${e.rows.toLocaleString()} groups ready` });
          break;
      }
    }
    return m;
  }, [cursor, list]);

  const start = useCallback(() => {
    setCursor(-1);
    setPlaying(true);
  }, []);
  const pause = useCallback(() => setPlaying(false), []);
  const resume = useCallback(() => setPlaying(true), []);
  const step = useCallback(() => {
    setPlaying(false);
    setCursor((c) => Math.min(c + 1, list.length - 1));
  }, [list.length]);
  const reset = useCallback(() => {
    setPlaying(false);
    setCursor(-1);
  }, []);
  const seek = useCallback((i: number) => {
    setPlaying(false);
    setCursor(Math.max(-1, Math.min(i, list.length - 1)));
  }, [list.length]);

  return {
    active: cursor >= 0,
    cursor,
    total: list.length,
    current: cursor >= 0 ? list[cursor] : null,
    playing,
    speed,
    setSpeed,
    state,
    start,
    pause,
    resume,
    step,
    reset,
    seek,
  };
}
