// @vitest-environment jsdom
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage, entryKey } from './test-support/main-entry-page.js'
let cleanup
beforeAll(async () => { cleanup = await bootEntryPage() })
afterAll(() => cleanup?.())
it('the packaged entry drives the real default scene and its selected UI control', async () => {
  await vi.waitFor(() => expect(document.getElementById('status').textContent).toMatch(/^parked: WAIT:/))
  expect(document.getElementById('stage').textContent).toContain('U21_SELECTED_ENTRY')
  expect(document.getElementById('stage').textContent).not.toContain('U21_LEGACY_FIRST')
  expect(document.getElementById('scene').value).toBe(entryKey)
  expect(window.__caesuraErrors).toEqual([])
})
