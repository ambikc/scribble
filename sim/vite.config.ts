import { defineConfig } from 'vite';

// Relative base so the built simulator also works from a sub-folder (e.g. GitHub Pages).
export default defineConfig({
  base: './',
  server: { host: true },
  test: { environment: 'node', include: ['test/**/*.test.ts'] },
} as never);
