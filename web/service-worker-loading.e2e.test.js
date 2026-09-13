// @vitest-environment jsdom
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage } from './test-support/main-entry-page.js'

let cleanup
const register = vi.fn().mockResolvedValue({})
const previous = Object.getOwnPropertyDescriptor(navigator, 'serviceWorker')
beforeAll(async () => {
  Object.defineProperty(navigator, 'serviceWorker', {configurable:true, value:{register}})
  vi.spyOn(document, 'readyState', 'get').mockReturnValue('loading')
  cleanup = await bootEntryPage()
})
afterAll(() => {
  cleanup?.()
  vi.restoreAllMocks()
  if (previous) Object.defineProperty(navigator, 'serviceWorker', previous)
  else delete navigator.serviceWorker
})
it('the actual player waits for an outstanding load event and registers only once', async () => {
  expect(register).not.toHaveBeenCalled()
  window.dispatchEvent(new Event('load'))
  await vi.waitFor(() => expect(register).toHaveBeenCalledTimes(1))
  expect(register).toHaveBeenCalledWith('./sw.js')
  window.dispatchEvent(new Event('load'))
  expect(register).toHaveBeenCalledTimes(1)
  expect(window.__caesuraErrors).toEqual([])
})
