// A per-connection public Undici Dispatcher for the already-owned CDP endpoint.
// The builtin WebSocket still validates Sec-WebSocket-Accept and parses frames.
// https://github.com/nodejs/undici/blob/v7.25.0/docs/docs/api/WebSocket.md
// https://github.com/nodejs/undici/blob/v7.25.0/docs/docs/api/Dispatcher.md
import { request } from 'node:http'

function endpointFor(value) {
  const url = new URL(value)
  if (url.protocol !== 'ws:' || url.hostname !== '127.0.0.1' || !url.port
      || url.username || url.password || url.search || url.hash
      || !/^\/devtools\/browser\/[A-Za-z0-9_-]+$/.test(url.pathname)) {
    throw new Error('CDP WebSocket must use the exact explicit loopback browser endpoint')
  }
  return url
}

function headerPairs(headers) {
  if (Array.isArray(headers)) {
    if (headers.every(value => Array.isArray(value) && value.length === 2)) return headers
    if (headers.length % 2) throw new Error('Invalid dispatcher header list')
    return Array.from({ length: headers.length / 2 }, (_, i) => [headers[2 * i], headers[2 * i + 1]])
  }
  if (headers && typeof headers[Symbol.iterator] === 'function') return [...headers]
  return Object.entries(headers || {})
}

// This deliberately implements only one HTTP/1.1 WebSocket upgrade, not a pool,
// proxy, redirector, general fetch dispatcher, or WebSocket frame implementation.
class CdpUpgradeDispatcher {
  constructor(endpoint) { this.endpoint = endpoint; this.used = false }

  dispatch(options, handler) {
    let req, socket, terminal = false, upgraded = false
    const fail = error => {
      if (terminal) return
      terminal = true
      req?.destroy(); socket?.destroy()
      handler.onError(error)
    }
    try {
      const endpoint = this.endpoint
      if (this.used || String(options.origin) !== `http://${endpoint.host}`
          || options.path !== endpoint.pathname || options.method !== 'GET'
          || String(options.upgrade).toLowerCase() !== 'websocket' || options.body != null) {
        throw new Error('Unexpected CDP upgrade dispatch; no redirect or endpoint substitution allowed')
      }
      this.used = true
      if (typeof handler.onConnect !== 'function' || typeof handler.onUpgrade !== 'function'
          || typeof handler.onError !== 'function') throw new Error('Unsupported public WebSocket dispatcher handler')
      const headers = Object.create(null)
      for (const [name, value] of headerPairs(options.headers)) {
        const lower = String(name).toLowerCase()
        // The builtin advertises deflate before dispatch. Do not transmit it:
        // CDP's full response can exceed its bundled decompressor flush bound.
        if (lower === 'sec-websocket-extensions') continue
        if (lower === 'host' && String(value) !== endpoint.host) throw new Error('Unexpected CDP Host header')
        headers[lower] = value
      }
      headers.connection = 'Upgrade'; headers.upgrade = 'websocket'
      req = request({ protocol: 'http:', hostname: endpoint.hostname, port: endpoint.port,
        path: endpoint.pathname, method: 'GET', headers, agent: false })
      req.once('error', error => { if (!upgraded) fail(error) })
      req.once('response', response => {
        response.destroy()
        fail(new Error(`CDP WebSocket upgrade returned HTTP ${response.statusCode}; redirects are not followed`))
      })
      req.once('upgrade', (response, connection, head) => {
        socket = connection
        try {
          if (response.statusCode !== 101 || response.headers['sec-websocket-extensions'] !== undefined) {
            throw new Error('CDP upgrade must be HTTP 101 without any negotiated extensions')
          }
          // Do not validate/replace the key or accept value here. The builtin
          // verifies them, including negative controls for a forged accept.
          if (head.length) socket.unshift(head)
          const accepted = handler.onUpgrade(response.statusCode,
            response.rawHeaders.map(value => Buffer.from(value, 'latin1')), socket)
          if (accepted === false) throw new Error('Builtin WebSocket rejected the CDP upgrade')
          upgraded = true
        } catch (error) { fail(error) }
      })
      handler.onConnect(error => fail(error || new Error('CDP upgrade aborted')), null)
      if (!terminal) req.end()
      return true
    } catch (error) {
      if (typeof handler.onError !== 'function') throw error
      fail(error)
      return false
    }
  }
}

export function createCdpWebSocket(url) {
  const endpoint = endpointFor(url)
  return new WebSocket(endpoint.href, { dispatcher: new CdpUpgradeDispatcher(endpoint) })
}
