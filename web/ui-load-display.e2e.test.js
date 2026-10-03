// @vitest-environment jsdom
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage, entryKey } from './test-support/main-entry-page.js'

const source = '[ch text="SAVED_VISIBLE_PAGE"]\n[p]\n[ch text="NEXT_AFTER_INPUT"]\n[end]'
const text = () => document.getElementById('stage').textContent
const status = () => document.getElementById('status').textContent
const log = () => document.getElementById('log').textContent
let cleanup
beforeAll(async () => {
  cleanup = await bootEntryPage({ savedSlot: 63, sceneSources: { [entryKey]: source } })
})
afterAll(() => cleanup?.())

async function uiLoad() {
  const count = log().split('load result:').length
  const button = [...document.querySelectorAll('#saves button')].find(node => node.textContent === 'Load')
  expect(button).toBeDefined()
  button.click()
  await vi.waitFor(() => expect(log().split('load result:').length).toBe(count + 1))
}

it('cold UI Load restores the saved page without synthesizing an advance', async () => {
  await vi.waitFor(() => expect(status()).toMatch(/^parked: WAIT:/))
  const original = localStorage.getItem('caesura.save.63')
  await uiLoad()
  console.log('UI_LOAD_COLD_OBSERVATION', JSON.stringify({ status: status(), text: text() }))
  expect(text()).toContain('SAVED_VISIBLE_PAGE')
  expect(text()).not.toContain('NEXT_AFTER_INPUT')
  expect(status()).toMatch(/^load: WAIT:/)
  expect(localStorage.getItem('caesura.save.63')).toBe(original)
  expect(window.__caesuraErrors).toEqual([])
})

it('hot UI Save/Load keeps the page until an actual Advance reaches the next page', async () => {
  document.getElementById('run').click()
  await vi.waitFor(() => expect(status()).toMatch(/^parked: WAIT:/))
  document.getElementById('save-slot').value = '63'
  const writes = log().split('saved current position to slot 63').length
  document.getElementById('save-now').click()
  await vi.waitFor(() => expect(log().split('saved current position to slot 63').length).toBe(writes + 1))
  const original = localStorage.getItem('caesura.save.63')
  await uiLoad()
  console.log('UI_LOAD_HOT_OBSERVATION', JSON.stringify({ status: status(), text: text() }))
  expect(text()).toContain('SAVED_VISIBLE_PAGE')
  expect(text()).not.toContain('NEXT_AFTER_INPUT')
  expect(status()).toMatch(/^load: WAIT:/)
  expect(localStorage.getItem('caesura.save.63')).toBe(original)
  // [ch] and the explicit following [p] are two distinct input waits.
  // The first real input must not silently consume the page-break wait.
  const advances = log().split('advance:').length
  document.getElementById('advance').click()
  await vi.waitFor(() => expect(log().split('advance:').length).toBe(advances + 1))
  console.log('UI_LOAD_FIRST_ADVANCE', JSON.stringify({ status: status(), text: text() }))
  expect(status()).toMatch(/^advance: WAIT:/)
  expect(text()).toContain('SAVED_VISIBLE_PAGE')
  document.getElementById('advance').click()
  await vi.waitFor(() => expect(text()).toContain('NEXT_AFTER_INPUT'))
  console.log('UI_LOAD_SECOND_ADVANCE', JSON.stringify({ status: status(), text: text() }))
  expect(text()).not.toContain('SAVED_VISIBLE_PAGE')
  expect(window.__caesuraErrors).toEqual([])
})
