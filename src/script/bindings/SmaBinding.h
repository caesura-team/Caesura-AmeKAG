#pragma once

struct lua_State;

namespace Caesura {

// Lua `sma` global (SMA Battle 4d S3 driver surface): exposes the
// IMeshRenderer pipeline to scripts/kag/sma.lua.
//   sma.create_mesh(verts, indices) -> handle (or 0)
//   sma.update_mesh(handle, poses)          -- poses: {{rot, scale, ox, oy}, ...}
//   sma.draw_mesh(handle, view, textureManagerId, x, y, scale, opacity)
//   sma.destroy_mesh(handle)
//   sma.count() -> number
void registerSmaBinding(lua_State* L);

} // namespace Caesura
