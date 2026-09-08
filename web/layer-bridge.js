// Lua tables provide native type/identity semantics; every field access still
// targets the corresponding canonical JS object. No snapshots replace live nodes.
import {luaLiteralValue} from './lua-value.js'
const SNAPSHOT_FIELDS=['id','x','y','w','h','visible','opacity','alpha','blend_mode',
  'z','scaleX','scaleY','rotation','clipX','clipY','clipW','clipH','layer_type','tag']
export async function installLayerBridge(lua, core) {
  const keys = new WeakMap()
  let sequence = 0
  const host = {
    object: value => value !== null && typeof value === 'object',
    key: value => {
      if (!keys.has(value)) keys.set(value, ++sequence)
      return keys.get(value)
    },
    read: (value, key) => {
      if (Array.isArray(value) && typeof key === 'number') return value[key - 1]
      const name = core.isLayerNode(value) && key === 'tex' ? 'texture'
        : core.isLayerNode(value) && key === 'layer_type' ? 'layerType' : key
      return Object.hasOwn(value, name) ? value[name] : undefined
    },
    write: (value, key, next) => {
      if (core.isLayerNode(value)) return core.setLayerProperty(value, key, next)
      if (core.isLayerChildren(value)) throw new Error('Use layer APIs to change children')
      if (key === '__proto__' || key === 'constructor' || key === 'prototype') throw new Error('Invalid layer field')
      value[Array.isArray(value) && typeof key === 'number' ? key - 1 : key] = next
      return true
    },
    size: value => Array.isArray(value) ? value.length : 0,
    names: value => Array.isArray(value) ? Object.keys(value).map(key => Number(key) + 1) : Object.keys(value),
    root: () => core.getRoot(),
    get: name => core.getLayer(name) ?? undefined,
    all: () => [core.getRoot(), ...core.layers.values()],
    ensure: (name, z) => core.ensureLayer(String(name), { z: z ?? undefined, tag: String(name), layer_type: 4 }),
    add: (parent, options) => core.addLayer(parent, { layer_type: 4, ...options }),
    bg: name => core.ensureLayer(String(name), { z: 0, layer_type: 1 }),
    fg: name => core.ensureLayer(String(name), { z: 1, layer_type: 4 }),
    image: (node, id) => core.setLayerImage(node, id),
    visible: (node, value) => core.setLayerVisible(node, value),
    opacity: (node, value) => core.setLayerOpacity(node, value),
    z: (node, value) => core.setLayerZ(node, value),
    move: (node, x, y) => core.moveLayer(node, x, y),
    remove: node => core.removeLayer(node),
    clear: () => core.clearLayers(),
    install: records => core.installPreparedLayers(records),
    snapshot: () => core.renderList(),
    captureSnapshot: () => {
      const layers=Object.create(null)
      for (const node of core.layers.values()) {
        const values=Object.create(null)
        for (const key of SNAPSHOT_FIELDS) values[key]=node[key==='layer_type'?'layerType':key]
        layers[node.id]=values
      }
      return luaLiteralValue({layers})
    },
    captureRestoreTree: scalarFields => {
      const fields = ['id','name','tag','visible','blend_mode','tex','rt', ...scalarFields.split(',')]
      const seen = new Set()
      const walk = (node, depth) => {
        if (depth > 64 || seen.size >= 256 || seen.has(node)) throw new Error('Layer tree exceeds restore limits')
        seen.add(node)
        const values = Object.create(null)
        for (const key of fields) {
          const value = host.read(node, key)
          if (value == null) continue
          if (!['string','number','boolean'].includes(typeof value)) throw new Error('Non-scalar layer field: ' + key)
          values[key] = value
        }
        // These fields are refusal conditions, never restorable payloads.
        // Preserve Lua truthiness (0 is true) and avoid serializing opaque data.
        if (node.userdata != null) values.userdata = true
        for (const key of ['quake','shake','fade']) {
          const effect = node[key]
          if (effect == null || effect === false) continue
          if (typeof effect !== 'object') throw new Error('Invalid layer effect: ' + key)
          if (effect.active != null && effect.active !== false) values[key] = {active:true}
        }
        values.children = (node.children ?? []).map(child => walk(child, depth + 1))
        return values
      }
      return luaLiteralValue(walk(core.getRoot(), 0))
    },
  }
  lua.global.set('__CAESURA_LAYER_HOST', host)
  try {
    await lua.doString(`
      local host=__CAESURA_LAYER_HOST
      local cache=setmetatable({}, {__mode='v'})
      local raw=setmetatable({}, {__mode='k'})
      local function unwrap(value) return raw[value] or value end
      local wrap
      wrap=function(value)
        if not host.object(value) then return value end
        local key=host.key(value)
        if cache[key] then return cache[key] end
        local proxy={}
        raw[proxy]=value; cache[key]=proxy
        return setmetatable(proxy, {
          __index=function(_,field) return wrap(host.read(value,field)) end,
          __newindex=function(_,field,next) host.write(value,field,unwrap(next)) end,
          __len=function() return host.size(value) end,
          __pairs=function()
            local fields=host.names(value)
            local index=0
            return function()
              index=index+1
              local field=host.read(fields,index)
              if field~=nil then return field,proxy[field] end
            end,proxy,nil
          end,
          __metatable=false,
        })
      end
      local L={Type={LAYER_BASE=1,LAYER_LAYER0=2,LAYER_LAYER1=3,LAYER_FORE=4,
        LAYER_UI=5,LAYER_MESSAGE=6,LAYER_EFFECT=7,LAYER_NORMAL=0,LAYER_BG=1,LAYER_FG=4}}
      function L.get_root() return wrap(host.root()) end
      function L.get(name) return wrap(host.get(name)) end
      L.get_layer=L.get
      function L.forEach(callback)
        local nodes=wrap(host.all())
        for _,node in ipairs(nodes) do callback(node.id,node) end
      end
      function L.find(predicate)
        local result
        L.forEach(function(_,node)
          if result then return end
          if type(predicate)=='function' and predicate(node)
            or type(predicate)=='string' and (node.tag==predicate or node.name==predicate) then result=node end
        end)
        return result
      end
      function L.ensure(_,name,z)
        local node=L.get(name) or L.find(name)
        if node then
          if z~=nil then host.z(unwrap(node),z) end
          return node
        end
        return wrap(host.ensure(name,z))
      end
      function L.add_layer(parent,options) return wrap(host.add(unwrap(parent),options or {})) end
      function L.bg(name) return wrap(host.bg(name or 'bg')) end
      function L.fg(name) return wrap(host.fg(name or 'fg')) end
      function L.set_layer_image(node,id,ix,iy,iw,ih)
        if not node then return end
        host.image(unwrap(node),id)
        node.imgX=ix; node.imgY=iy; node.imgW=iw; node.imgH=ih
        if iw~=nil and ih~=nil then node.w=iw; node.h=ih end
      end
      function L.set_layer_visible(node,value) if node then host.visible(unwrap(node),value) end end
      function L.set_layer_opacity(node,value) if node then host.opacity(unwrap(node),value) end end
      function L.set_z(node,value) if node then host.z(unwrap(node),value) end end
      function L.move_layer(node,x,y) if node then host.move(unwrap(node),x,y) end end
      function L.set_layer_blend(node,value) if node then node.blend_mode=value end end
      function L.mark_dirty(node) if node then node.dirty=true end end
      function L.remove_layer(node) return host.remove(unwrap(node)) end
      L.remove=L.remove_layer
      function L.clear_for_restore() return host.clear() end
      L.init=L.clear_for_restore
      function L.count() return host.size(host.all()) end
      function L.install_prepared(nodes) return host.install(nodes) end
      -- Existing rollback contract: value transforms of surviving nodes.
      -- Resource/graph replacement belongs to the save restoration API.
      local snapshot_fields={'id','x','y','w','h','visible','opacity','alpha',
        'blend_mode','z','scaleX','scaleY','rotation','clipX','clipY','clipW','clipH','layer_type','tag'}
      local snapshot_source,snapshot_constructor
      function L.capture_snapshot()
        -- Copy once inside JS and transfer one value string. Per-field proxy
        -- reads dominated snapshot CPU in the real Wasmoon measurement.
        local source=host.captureSnapshot()
        if source~=snapshot_source then
          local constructor=assert(load('return '..source,'@layers.snapshot','t',{}))
          snapshot_source,snapshot_constructor=source,constructor
        end
        -- Cache code only: every capture still owns distinct value tables.
        return snapshot_constructor()
      end
      local restore_source,restore_constructor
      function L.capture_restore_tree(fields)
        local source=host.captureRestoreTree(table.concat(fields,','))
        if source~=restore_source then
          local constructor=assert(load('return '..source,'@layers.restore-tree','t',{}))
          restore_source,restore_constructor=source,constructor
        end
        return restore_constructor()
      end
      function L.restore_snapshot(snapshot)
        if not snapshot or not snapshot.layers then return end
        for _,node in ipairs(wrap(host.all())) do
          local values=snapshot.layers[node.id]
          if values then
            for _,key in ipairs(snapshot_fields) do
              if key~='id' and values[key]~=nil then node[key]=values[key] end
            end
            L.mark_dirty(node)
          end
        end
      end
      function L.snapshot()
        local out={}
        for i,node in ipairs(wrap(host.snapshot())) do
          out[i]={id=node.id,name=node.name,tag=node.tag,x=node.x,y=node.y,w=node.w,h=node.h,
            visible=node.visible,opacity=node.opacity,z=node.z,texture=node.texture}
        end
        return out
      end
      function L.get_world_pos(node)
        local x,y=0,0
        while node do x=x+(node.x or 0); y=y+(node.y or 0); node=node.parent end
        return x,y
      end
      function L.get_world_rect(node)
        local x,y=L.get_world_pos(node)
        return {x=x,y=y,w=node.w or 0,h=node.h or 0}
      end
      function L.set_position(name,x,y,scale,unit)
        local node=L.get(name)
        if not node then return end
        if unit=='px' then x=x/1280; y=y/720 end
        node.pos_x=x; node.pos_y=y; node.scale=scale or 1
        L.mark_dirty(node)
      end
      function L.set_options(name,options)
        local node=L.get(name)
        if not node then return end
        if options.opacity~=nil then L.set_layer_opacity(node,math.floor(math.max(0,math.min(1,options.opacity))*255)) end
        if options.visible~=nil then L.set_layer_visible(node,options.visible) end
        if options.blend~=nil then L.set_layer_blend(node,options.blend) end
        L.mark_dirty(node)
      end
      function L.fade_to(node,target,duration)
        if not node then return end
        local finish=math.max(0,math.min(255,tonumber(target) or 255))
        duration=tonumber(duration) or 0
        local start,elapsed=node.opacity or 255,0
        while elapsed<duration do
          local delta=16
          if coroutine.isyieldable() then local resumed=coroutine.yield(); delta=tonumber(resumed) or 16 end
          elapsed=elapsed+math.max(delta,0)
          L.set_layer_opacity(node,start+(finish-start)*math.min(1,elapsed/duration))
        end
        L.set_layer_opacity(node,finish)
      end
      _G.layers=L
      package.loaded.layers=L
    `)
  } finally {
    lua.global.set('__CAESURA_LAYER_HOST', undefined)
  }
}
