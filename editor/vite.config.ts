import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// The engine's HTTP editor server is on 127.0.0.1:9876 and only allows
// localhost/127.0.0.1 CORS origins. In dev, Vite runs on :5173 -- proxy
// /api to the engine so the browser sees a same-origin request (no CORS
// preflight, no token header leaks). In production build the front-end
// is served from the same origin as the engine (or behind the same
// proxy), so this matches both modes.
export default defineConfig({
  // Keep packaged static assets resolvable after moving the dist directory.
  base: './',
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:9876',   // Windows: localhost resolves ::1 first (EditorServer binds IPv4 only)
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: 'dist',
    // M1-J: release packages must not ship sourcemaps (.map files ~16MB for
    // the main bundle alone); set CAESURA_EDITOR_SOURCEMAP=1 to opt back in.
    sourcemap: process.env.CAESURA_EDITOR_SOURCEMAP === '1',
  },
})
