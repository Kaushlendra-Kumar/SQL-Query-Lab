import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// During development the C++ server runs on :8080 and Vite proxies /api to it.
export default defineConfig({
  plugins: [react()],
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
