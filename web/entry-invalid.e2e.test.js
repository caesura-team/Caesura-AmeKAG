// @vitest-environment jsdom
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage } from './test-support/main-entry-page.js'
let cleanup
beforeAll(async () => { cleanup = await bootEntryPage({entry:'missing.ks'}) })
afterAll(() => cleanup?.())
it('a broken declared entry is a visible boot failure instead of playing another scene', async () => {
  await vi.waitFor(() => expect(window.__caesuraErrors.length).toBeGreaterThan(0))
  expect(document.getElementById('status').textContent).toMatch(/^FAILED:/)
  expect(document.getElementById('stage').textContent).not.toMatch(/U21_SELECTED_ENTRY|U21_LEGACY_FIRST/)
  expect(document.getElementById('log').textContent).toContain('entry')
})
