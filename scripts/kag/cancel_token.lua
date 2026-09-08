-- ===========================================================================
--  Caesura (AmeKAG) — cancel_token.lua
--  Spec [10.2.33]: Two-phase cancel model.
--
--  Phase 1: mark_cancelled() — sets the cancelled flag (idempotent,
--           guards against re-entrant cancellation).
--  Phase 2: execute_callbacks() — runs all registered callbacks in
--           reverse order, pcall-wrapped (no yield, no new coroutine).
--
--  Usage:
--    local ct = CancelToken.new()
--    ct:register(function() backend.close(handle) end)
--    ...
--    ct:mark_cancelled()       -- Phase 1
--    ct:execute_callbacks()    -- Phase 2 (after coroutine.close)
--
--  Convenience: ct:cancel() = mark_cancelled() + execute_callbacks()
--
--  Lua 5.4: __close metamethod calls cancel() automatically when the
--           token goes out of scope in a <close> variable declaration.
-- ===========================================================================

local CancelToken = {}
CancelToken.__index = CancelToken

-- ===========================================================================
--  CancelToken.new() → CancelToken instance
-- ===========================================================================
function CancelToken.new()
    return setmetatable({
        cancelled         = false,
        cancellation_lock = false,
        callbacks         = {},
    }, CancelToken)
end

-- Lua 5.4 __close support: local ct <close> = CancelToken.new()
function CancelToken:__close()
    self:cancel()
end

-- ===========================================================================
--  CancelToken:register(fn)
--  Register a cleanup callback.  If the token is already cancelled,
--  the callback is executed immediately.  Callbacks MUST NOT yield
--  or start new coroutines — they are for releasing main-thread
--  resources only (per arch invariant 1.3).
-- ===========================================================================
function CancelToken:register(fn)
    if type(fn) ~= "function" then return end
    if self.cancelled then
        pcall(fn)
        return
    end
    table.insert(self.callbacks, fn)
end

-- ===========================================================================
--  CancelToken:mark_cancelled()
--  Phase 1: Mark the token as cancelled.  Idempotent; the lock keeps
--  repeated calls from changing the cancellation state.
-- ===========================================================================
function CancelToken:mark_cancelled()
    if self.cancellation_lock then return end
    self.cancellation_lock = true
    self.cancelled = true
end

-- ===========================================================================
--  CancelToken:execute_callbacks()
--  Phase 2: Run all registered callbacks in reverse registration order,
--  each wrapped in pcall. Detach the list first so re-entrant cancellation
--  cannot execute the same callbacks again.
--  Should be called after coroutine.close() in the scheduler.
-- ===========================================================================
function CancelToken:execute_callbacks()
    local callbacks = self.callbacks
    self.callbacks = {}
    for i = #callbacks, 1, -1 do
        pcall(callbacks[i])
    end
end

-- ===========================================================================
--  CancelToken:cancel()
--  Convenience: mark_cancelled() then execute_callbacks().
--  Also exposed as the __close metamethod for Lua 5.4 <close> support.
-- ===========================================================================
function CancelToken:cancel()
    self:mark_cancelled()
    -- coroutine.close() is the scheduler's responsibility, not ours.
    self:execute_callbacks()
end

-- Finish a token without firing cancellation callbacks on the success path.
function CancelToken:complete()
    if self.cancelled then
        self:execute_callbacks()
    else
        self.callbacks = {}
    end
end

return CancelToken
