// @vitest-environment jsdom
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage } from './test-support/main-entry-page.js'

let cleanup
const register = vi.fn().mockResolvedValue({})
const previous = Object.getOwnPropertyDescriptor(navigator, 'serviceWorker')
beforeAll(async () => {
  Object.defineProperty(navigator, 'serviceWorker', {configurable:true, value:{register}})
  vi.spyOn(document, 'readyState', 'get').mockReturnValue('complete')
  cleanup = await bootEntryPage()
})
afterAll(() => {
  cleanup?.()
  vi.restoreAllMocks()
  if (previous) Object.defineProperty(navigator, 'serviceWorker', previous)
  else delete navigator.serviceWorker
})
it('the actual player registers its classic package worker when async boot finishes after window load', async () => {
  await vi.waitFor(() => expect(register).toHaveBeenCalledTimes(1))
  expect(register).toHaveBeenCalledWith('./sw.js')
  window.dispatchEvent(new Event('load'))
  expect(register).toHaveBeenCalledTimes(1)
  expect(window.__caesuraErrors).toEqual([])
})
