// @vitest-environment jsdom
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage } from './test-support/main-entry-page.js'
let cleanup
beforeAll(async () => { cleanup = await bootEntryPage({legacy:true}) })
afterAll(() => cleanup?.())
it('a legacy bundle without entry still opens its original first scene', async () => {
  await vi.waitFor(() => expect(document.getElementById('status').textContent).toMatch(/^parked: WAIT:/))
  expect(document.getElementById('stage').textContent).toContain('U21_LEGACY_FIRST')
  expect(window.__caesuraErrors).toEqual([])
})
