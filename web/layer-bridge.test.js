// @vitest-environment node
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { Lua } from 'wasmoon'
import { AdapterCore } from './adapter-core.js'
import { installLayerBridge } from './layer-bridge.js'

const here = dirname(fileURLToPath(import.meta.url))
let factory, lua, core

beforeAll(async () => {
  factory = await Lua.load({ wasmFile: join(here, 'node_modules/wasmoon/dist/glue.wasm') })
})
beforeEach(async () => {
  lua = factory.createState()
  core = new AdapterCore()
  await installLayerBridge(lua, core)
})
afterEach(() => lua?.global.close())

async function loadLayerState() {
  for (const name of ['kag.wait_state', 'kag.save_state', 'capability_catalog', 'capability_json',
    'target_capabilities', 'capability_runtime', 'capability_backend', 'backend', 'rtt', 'kag.layer_state']) {
    lua.global.set('__module_source', readFileSync(join(here, '../scripts', name.replaceAll('.', '/') + '.lua'), 'utf8'))
    await lua.doString(`package.preload[${JSON.stringify(name)}]=assert(load(__module_source)); __module_source=nil`)
  }
}

describe('live Lua Layers facade (real Wasmoon)', () => {
  it('bulk capture preserves the complete restore tree and matches the shared scalar validator', async () => {
    await loadLayerState()
    const result = await lua.doString(`
      local L=require('layers')
      local state=require('kag.layer_state')
      local bulk=assert(L.capture_restore_tree,'bulk restore capture unavailable')
      Restore={describe_texture=function() return {kind='color',r=1,g=2,b=3,a=255} end}
      local parent=L.add_layer(nil,{id='group',name='Group',x=17,y=23,z=2,visible=false})
      local n=L.add_layer(parent,{id='child',name='Child',tag='portrait',z=3})
      n.tex=73; n.rt=4; n.w=80; n.h=40; n.opacity=0; n.alpha=0
      n.originX=7; n.originY=8; n.pos_x=0.2; n.pos_y=0.3
      n.imgX=1; n.imgY=2; n.imgW=30; n.imgH=20; n.scale=0.5; n.rotation=11
      local first=state.capture()
      local second=state.capture()
      assert(first~=second and first.nodes~=second.nodes and first.nodes[3]~=second.nodes[3])
      L.capture_restore_tree=nil
      local scalar=state.capture()
      L.capture_restore_tree=bulk
      n.x=99; n.opacity=255; n.parent=L.get_root()
      assert(first.nodes[3].parent=='group' and first.nodes[3].opacity==0)
      local changed=state.capture()
      assert(changed.nodes[3].x==99 and changed.nodes[3].parent=='_root')
      return {bulk=first,scalar=scalar}
    `)
    expect(result.bulk).toEqual(result.scalar)
    expect(result.bulk.nodes[2]).toMatchObject({parent:'group',opacity:0,alpha:0,originX:7,originY:8,
      pos_x:0.2,pos_y:0.3,imgX:1,imgY:2,imgW:30,imgH:20,source:{kind:'color',r:1,g:2,b:3,a:255}})
  })

  it('bulk and scalar capture both reject unsupported payloads, effects, values and cyclic trees', async () => {
    await loadLayerState()
    expect(await lua.doString(`
      local L=require('layers')
      local state=require('kag.layer_state')
      local bulk=assert(L.capture_restore_tree,'bulk restore capture unavailable')
      local n=L.ensure({},'actor',1)
      local function refused()
        local fast=pcall(state.capture)
        L.capture_restore_tree=nil
        local scalar=pcall(state.capture)
        L.capture_restore_tree=bulk
        assert(not fast and not scalar,'capture refusal differs')
      end
      n.userdata=false; refused(); n.userdata=nil
      n.quake.active=true; refused(); n.quake.active=false
      n.rt=17; refused(); n.rt=nil
      n.w=-1; refused(); n.w=0
      n.x=0/0; refused(); n.x=0
      assert(state.capture().nodes[2].id==n.id)
      return L.get('actor')==n
    `)).toBe(true)
    core.getRoot().children.push(core.getRoot())
    expect(await lua.doString(`
      local L=require('layers'); local state=require('kag.layer_state')
      local fast=pcall(state.capture)
      L.capture_restore_tree=nil
      local scalar=pcall(state.capture)
      return not fast and not scalar
    `)).toBe(true)
  })

  it('captures isolated rollback transforms and restores surviving layer identities', async () => {
    expect(await lua.doString(`
      local L=require('layers')
      local n=L.ensure({},'actor',2)
      n.x=17; n.opacity=0
      local snapshot=L.capture_snapshot()
      local second=L.capture_snapshot()
      assert(second~=snapshot and second.layers~=snapshot.layers and second.layers[n.id]~=snapshot.layers[n.id])
      n.x=99; n.opacity=255
      assert(snapshot.layers[n.id].x==17 and snapshot.layers[n.id].opacity==0)
      L.restore_snapshot(snapshot)
      return L.get('actor')==n and n.x==17 and n.opacity==0
    `)).toBe(true)
  })
  it('installs real Lua tables with a stable root and nil for a missing node', async () => {
    expect(await lua.doString(`
      local L = require('layers')
      local root = L.get_root()
      assert(type(L)=='table' and L==_G.layers)
      assert(type(root)=='table' and root==L.get_root() and root.id=='_root')
      assert(type(root.children)=='table' and #root.children==0 and root.parent==nil)
      assert(L.get('missing')==nil and L.find('missing')==nil)
      assert(L.Type.LAYER_BASE==1 and L.Type.LAYER_MESSAGE==6)
      return L.count()
    `)).toBe(1)
  })

  it('keeps direct Lua scalar and nested writes visible in the canonical JS node', async () => {
    await lua.doString(`
      local L=require('layers')
      n=L.ensure({},'actor',2)
      assert(type(n)=='table' and type(n.id)=='string')
      n.x=43; n.y=17; n.opacity=0; n.visible=false
      n.quake.active=true; n.quake.offset_x=7
      n.tex=51; n.layer_type=4; n.name='renamed'
      assert(L.get('renamed')==n and n.parent==L.get_root())
      assert(type(n.quake)=='table' and n.texture==51)
    `)
    const node = core.getLayer('renamed')
    expect(node).toMatchObject({ x: 43, y: 17, opacity: 0, visible: false, texture: 51, layerType: 4 })
    expect(node.quake).toMatchObject({ active: true, offset_x: 7 })
    node.x = 86
    expect(await lua.doString('return n.x')).toBe(86)
  })

  it('retains parent identity and stable sibling z order across direct writes and reparenting', async () => {
    expect(await lua.doString(`
      local L=require('layers')
      p=L.add_layer(L.get_root(),{id='parent',name='parent',x=10,y=20,z=1})
      a=L.add_layer(p,{id='a',name='a',x=2,y=3,z=5})
      b=L.add_layer(p,{id='b',name='b',z=2})
      c=L.add_layer(p,{id='c',name='c',z=2})
      assert(a.parent==p and p.children[1]==b and p.children[2]==c)
      a.z=0
      assert(p.children[1]==a and p.children[2]==b and p.children[3]==c)
      a.parent=L.get_root()
      assert(a.parent==L.get_root() and #p.children==2)
      assert(not pcall(function() p.parent=b end))
      assert(p.parent==L.get_root() and b.parent==p)
      return #L.get_root().children
    `)).toBe(2)
  })

  it('preserves ensure/get/find/mutation APIs and filters hidden parent subtrees for rendering', async () => {
    await lua.doString(`
      local L=require('layers')
      local p=L.ensure({},'panel',4)
      L.move_layer(p,10,20)
      local n=L.add_layer(p,{id='image',name='image',tag='portrait',x=3,y=4,z=2})
      L.set_layer_image(n,73)
      assert(L.ensure({},'panel',6)==p and p.z==6)
      assert(L.find('portrait')==n and L.find(function(v) return v.id=='image' end)==n)
      L.set_layer_opacity(n,128); L.set_layer_blend(n,'add')
      local x,y=L.get_world_pos(n); assert(x==13 and y==24)
      assert(L.get_layer('image')==n and n.tex==73)
    `)
    expect(core.renderList()).toEqual([expect.objectContaining({ name: 'image', x: 13, y: 24, opacity: 128, texture: 73 })])
    await lua.doString("layers.get('panel').visible=false")
    expect(core.renderList()).toEqual([])
  })

  it('clears actual nodes without claiming ownership of texture resources', async () => {
    const texture = core.loadTexture('fixture-only.png')
    lua.global.set('textureId', texture)
    await lua.doString(`
      old=layers.ensure({},'old',1)
      layers.set_layer_image(old,textureId)
      oldRoot=layers.get_root()
      assert(layers.clear_for_restore())
      assert(layers.count()==1 and #layers.get_root().children==0)
      assert(oldRoot~=layers.get_root() and layers.get('old')==nil)
    `)
    expect(core.layers.size).toBe(0)
    expect(core.renderList()).toEqual([])
    expect(core.textures.has(texture)).toBe(true)
    expect(core.events.some(event => event.kind === 'texture.destroy')).toBe(false)
  })

  it('synchronously installs caller-materialized IDs and isolates retired Lua/JS references', async () => {
    const old = core.ensureLayer('same', { x: 5 })
    const texture = core.loadTexture('already-materialized-by-caller.png')
    lua.global.set('textureId', texture)
    await lua.doString("old=layers.get('same'); oldRoot=layers.get_root()")
    expect(lua.doStringSync(`
      return layers.install_prepared({
        {id='_root',source=false},
        {id='same',parent='_root',name='same',x=22,y=33,w=64,h=48,z=2,
         visible=true,opacity=128,layer_type=4,image={id=textureId}},
        {id='child',parent='same',name='child',x=3,y=4,z=1,source=false}
      })
    `)).toBe(true)
    const current = core.getLayer('same')
    expect(current).not.toBe(old)
    await lua.doString(`
      local n=layers.get('same')
      assert(n~=old and layers.get_root()~=oldRoot)
      assert(n.tex==textureId and n.rt==nil and n.layer_type==4)
      assert(n.children[1].parent==n)
      old.x=999; layers.move_layer(old,888,777); layers.set_layer_image(old,100)
      assert(n.x==22 and n.y==33 and n.tex==textureId)
    `)
    core.moveLayer(old, 700, 800)
    expect(current).toMatchObject({ x: 22, y: 33, texture })
  })

  it('rejects an incomplete install without publishing a partial replacement', async () => {
    const original = core.ensureLayer('original', { x: 12 })
    await expect(lua.doString(`
      layers.install_prepared({{id='_root'}, {id='a',parent='missing',name='a'}})
    `)).rejects.toThrow()
    expect(core.getLayer('original')).toBe(original)
    expect(core.getLayer('a')).toBeNull()
    await expect(lua.doString(`
      layers.install_prepared({{id='_root'}, {id='_root',parent='_root'}})
    `)).rejects.toThrow()
    expect(core.getLayer('original')).toBe(original)
  })

  it('feeds the unchanged shared LayerState capture with actual live Lua graph fields', async () => {
    for (const name of ['kag.wait_state', 'kag.save_state', 'kag.layer_state']) {
      lua.global.set('__moduleSource', readFileSync(join(here, '../scripts', name.replaceAll('.', '/') + '.lua'), 'utf8'))
      lua.global.set('__moduleName', name)
      await lua.doString(`
        local source,name=__moduleSource,__moduleName
        package.preload[name]=function() return assert(load(source,'@'..name,'t',_ENV))() end
      `)
    }
    const snapshot = await lua.doString(`
      package.loaded.backend={}
      package.loaded.rtt={} -- capture is pure: no RTT or resource preparation is invoked.
      local p=layers.add_layer(layers.get_root(),{id='group',name='group',x=12,y=34,z=1})
      local n=layers.add_layer(p,{id='child',name='child',w=80,h=50,z=2})
      n.opacity=0; n.visible=false
      return require('kag.layer_state').capture()
    `)
    expect(snapshot).toMatchObject({ version: 1, nodes: [
      { id: '_root', source: false },
      { id: 'group', parent: '_root', x: 12, y: 34 },
      { id: 'child', parent: 'group', opacity: 0, visible: false, w: 80, h: 50 },
    ] })
  })

  it('keeps two explicitly different identities even when their display names match', async () => {
    expect(await lua.doString(`
      local L=require('layers')
      local a=L.add_layer(nil,{id='first',name='shared'})
      local b=L.add_layer(nil,{id='second',name='shared'})
      assert(a~=b and a.id=='first' and b.id=='second')
      b.x=29
      assert(a.x==0 and b.x==29)
      return #L.get_root().children
    `)).toBe(2)
    expect(core.layers.size).toBe(2)
  })

  it('ensures by existing tag and retains foreground/background type contracts', async () => {
    expect(await lua.doString(`
      local L=require('layers')
      local node=L.add_layer(nil,{id='actor-id',name='actor',tag='portrait'})
      assert(L.ensure({},'portrait',7)==node and node.z==7 and node.name=='actor')
      assert(node.layer_type==L.Type.LAYER_FORE)
      assert(L.bg().layer_type==L.Type.LAYER_BASE and L.fg().layer_type==L.Type.LAYER_FORE)
      return L.count()
    `)).toBe(4)
  })

  it('keeps zero opacity, image rectangles, position/options and frame-driven fades observable', async () => {
    expect(await lua.doString(`
      local L=require('layers')
      local node=L.ensure({},'panel',3)
      L.set_layer_image(node,78,0,0,80,40)
      L.set_options('panel',{opacity=0,visible=false,blend='add'})
      assert(node.opacity==0 and node.visible==false and node.blend_mode=='add')
      L.set_layer_visible(node,true)
      L.set_position('panel',128,72,2,'px')
      local rect=L.get_world_rect(node)
      assert(type(rect)=='table' and rect.x==0 and rect.y==0 and rect.w==80 and rect.h==40)
      assert(node.pos_x==0.1 and node.pos_y==0.1)
      L.set_layer_opacity(node,128)
      L.fade_to(node,0,32)
      local snapshot=L.snapshot()
      assert(type(snapshot)=='table' and type(snapshot[1])=='table')
      assert(snapshot[1].texture==78 and snapshot[1].opacity==0)
      L.remove_layer(node)
      assert(L.get('panel')==nil and #L.snapshot()==0)
      return L.count()
    `)).toBe(1)
  })

  it('allocates unused anonymous identities after restoring existing layer names', async () => {
    expect(await lua.doString(`
      local L=require('layers')
      assert(L.install_prepared({
        {id='_root'},
        {id='layer1',parent='_root',name='layer1',x=17},
        {id='layer3',parent='_root',name='layer3',x=31}
      }))
      local restored=L.get_layer('layer1')
      local a=L.add_layer(nil,{})
      local b=L.add_layer(nil,{})
      assert(type(a.id)=='string' and a.id~=b.id)
      assert(a.id~='layer1' and a.id~='layer3' and b.id~='layer1' and b.id~='layer3')
      assert(L.get_layer('layer1')==restored and restored.x==17)
      assert(L.get_layer('layer3').x==31)
      return #L.get_root().children
    `)).toBe(4)
    expect(core.layers.size).toBe(4)
  })

  it('returns the native named rectangle table to a real Lua hit-region caller', async () => {
    expect(await lua.doString(`
      local L=require('layers')
      local parent=L.add_layer(nil,{id='parent',x=10,y=20})
      local node=L.add_layer(parent,{id='button',x=3,y=4,w=80,h=40})
      parent.pos_x=400; parent.pos_y=500
      node.pos_x=600; node.pos_y=700
      local x,y=L.get_world_pos(node)
      assert(x==13 and y==24)
      local function contains(px,py)
        local rect=L.get_world_rect(node)
        assert(type(rect)=='table')
        return px>=rect.x and py>=rect.y and px<rect.x+rect.w and py<rect.y+rect.h
      end
      assert(contains(13,24) and contains(92,63))
      assert(not contains(12,24) and not contains(93,64))
      return L.get_world_rect(node)
    `)).toEqual({ x: 13, y: 24, w: 80, h: 40 })
  })
})
