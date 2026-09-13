// @vitest-environment jsdom
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage, entryKey } from './test-support/main-entry-page.js'

let cleanup
beforeAll(async () => { cleanup = await bootEntryPage({savedSlot:63}) })
afterAll(() => cleanup?.())

it('a fresh real page lists and loads the previous player saved slot without a boot rejection', async () => {
  await vi.waitFor(() => expect(document.getElementById('status').textContent).toMatch(/^parked: WAIT:/))
  const saved = localStorage.getItem('caesura.save.63')
  expect(JSON.parse(saved).scene).toBe(entryKey)
  expect(document.getElementById('saves-count').textContent).toBe('1')
  expect(document.getElementById('saves').textContent).toContain('scene ' + entryKey)
  expect(window.__caesuraErrors).toEqual([])
  const load = [...document.querySelectorAll('#saves button')].find(button => button.textContent === 'Load')
  expect(load).toBeDefined()
  load.click()
  await vi.waitFor(() => expect(document.getElementById('log').textContent).toMatch(/load result: (?:WAIT:|DONE)/))
  expect(localStorage.getItem('caesura.save.63')).toBe(saved)
  expect(window.__caesuraErrors).toEqual([])
})
