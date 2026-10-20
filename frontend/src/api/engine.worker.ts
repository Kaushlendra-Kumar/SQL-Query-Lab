/// <reference lib="webworker" />
// Runs the WebAssembly engine off the main thread so long queries never freeze
// the UI. The page sends { init: moduleUrl } once, then { id, method, path, body }
// requests; each reply is { id, status, body } (body is the JSON text the HTTP
// server would have sent) or { id, error } if the engine failed to load.

interface QueryLabModule {
  handle(method: string, path: string, body: string): string;
}

let engine: Promise<QueryLabModule> | null = null;

self.onmessage = async (e: MessageEvent) => {
  const msg = e.data;
  if (msg.init) {
    engine = import(/* @vite-ignore */ msg.init).then((m) => m.default() as Promise<QueryLabModule>);
    return;
  }
  try {
    const mod = await engine!;
    const out = mod.handle(msg.method, msg.path, msg.body ?? '');
    const nl = out.indexOf('\n');
    self.postMessage({ id: msg.id, status: Number(out.slice(0, nl)), body: out.slice(nl + 1) });
  } catch (err) {
    self.postMessage({ id: msg.id, error: (err as Error).message ?? String(err) });
  }
};
