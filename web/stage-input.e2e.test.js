// @vitest-environment jsdom
// Real main/Wasmoon/choice/DOM. Only layout bounds are supplied by the DOM host.
import { afterAll, beforeAll, expect, it, vi } from 'vitest'
import { bootEntryPage } from './test-support/main-entry-page.js'
let cleanup
const menu = `[select]
[sel target="*a" text="U21_OPTION_A"]
[sel target="*b" text="U21_OPTION_B"]
[endselect]
[end]
*a
[ch text="U21_ROUTE_A"]
[ch text="U21_SECOND_PAGE"]
[end]
*b
[ch text="U21_ROUTE_B"]
[ch text="U21_SECOND_PAGE"]
[end]`
const stage = () => document.getElementById('stage')
const status = () => document.getElementById('status').textContent
const rect = {left:60, top:40, width:640, height:360, right:700, bottom:400}
const waitText = text => vi.waitFor(() => expect(stage().textContent).toContain(text))
function clickLogical(x,y) {
  stage().dispatchEvent(new MouseEvent('click', {bubbles:true,button:0,
    clientX:rect.left+x/2,clientY:rect.top+y/2}))
}
beforeAll(async () => {
  cleanup = await bootEntryPage({entry:'menu.ks',sceneSources:{'menu.ks':menu}})
  Object.defineProperty(stage(),'clientWidth',{configurable:true,value:1280})
  Object.defineProperty(stage(),'clientHeight',{configurable:true,value:720})
  stage().getBoundingClientRect = () => rect
  await waitText('U21_OPTION_B')
})
afterAll(() => cleanup?.())
it('a scaled stage click selects the actual second choice and only one page', async () => {
  // A click inside the surface but away from a choice must not pick the default.
  clickLogical(100,30)
  await new Promise(resolve => setTimeout(resolve,40))
  expect(stage().textContent).toContain('U21_OPTION_B')
  expect(status()).not.toContain('ERR:')
  const option = [...stage().querySelectorAll('.caesura-message span')].find(node=>node.textContent==='2. U21_OPTION_B')
  expect(option).toBeTruthy()
  const x = parseFloat(option.style.left)+10
  const y = parseFloat(option.style.top)+parseFloat(option.style.fontSize)/2
  const before = document.getElementById('log').textContent.split('\n').filter(line=>line.startsWith('advance:')).length
  clickLogical(x,y)
  await waitText('U21_ROUTE_B')
  expect(stage().textContent).not.toContain('U21_ROUTE_A')
  expect(stage().textContent).not.toContain('U21_SECOND_PAGE')
  expect(document.getElementById('log').textContent.split('\n').filter(line=>line.startsWith('advance:'))).toHaveLength(before+1)
  clickLogical(100,600)
  await waitText('U21_SECOND_PAGE')
  expect(window.__caesuraErrors).toEqual([])
})
