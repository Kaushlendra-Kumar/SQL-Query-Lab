import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// By default the UI runs the engine in the browser (WebAssembly, built by
// scripts/build-wasm.sh into public/wasm). With VITE_BACKEND=server it talks to
// the C++ server instead; in development that runs on :8080 and Vite proxies /api to it.
export default defineConfig({
  // Relative asset paths, so the build works from any sub-path (e.g. GitHub Pages).
  base: "./",
  plugins: [react()],
  worker: { format: "es" },
  server: {
    port: 5173,
    proxy: { '/api': { target: process.env.QUERYLAB_API ?? 'http://127.0.0.1:8080', changeOrigin: false } },
  },
  build: {
    chunkSizeWarningLimit: 4000,
  },
  test: {
    environment: 'jsdom',
  },
} as any);
