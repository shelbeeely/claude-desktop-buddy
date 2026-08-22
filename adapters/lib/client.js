'use strict';

const net = require('net');
const { defaultSocketPath } = require('./daemon');

/**
 * Thin client hook scripts use to talk to the daemon (daemon.js) over its
 * local socket, instead of opening the device's serial port themselves —
 * see daemon.js's header comment for why. Each call opens a short-lived
 * socket connection, sends one request, reads one response line, and
 * disconnects; that's cheap and safe to do per hook invocation, unlike
 * repeatedly opening the real serial port.
 */
function request(req, { socketPath = defaultSocketPath(), timeoutMs } = {}) {
  const effectiveTimeout = timeoutMs || (req.timeoutMs || 3000) + 2000;
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(socketPath);
    let buf = '';
    const timer = setTimeout(() => {
      socket.destroy();
      reject(new Error(`adapter daemon at ${socketPath} did not respond within ${effectiveTimeout}ms`));
    }, effectiveTimeout);

    socket.on('connect', () => socket.write(`${JSON.stringify(req)}\n`));
    socket.on('error', (err) => {
      clearTimeout(timer);
      reject(new Error(`can't reach adapter daemon at ${socketPath} (is it running? see ../README.md): ${err.message}`));
    });
    socket.on('data', (chunk) => {
      buf += chunk.toString('utf8');
      const idx = buf.indexOf('\n');
      if (idx < 0) return;
      clearTimeout(timer);
      socket.end();
      try {
        resolve(JSON.parse(buf.slice(0, idx)));
      } catch (err) {
        reject(err);
      }
    });
  });
}

/** Fire-and-forget-ish: still resolves once the daemon has written the line. */
function sendHeartbeat(snapshot, opts) {
  return request({ type: 'heartbeat', snapshot }, opts);
}

/** Resolves to 'approve' or 'deny'; rejects if the daemon is unreachable or times out. */
function requestApproval({ id, tool, hint, snapshot, timeoutMs = 60_000 }, opts) {
  return request(
    { type: 'approval', id, tool, hint, snapshot, timeoutMs },
    { timeoutMs: timeoutMs + 2000, ...opts },
  ).then((res) => {
    if (!res.ok) throw new Error(res.error || 'approval request failed');
    return res.decision;
  });
}

module.exports = { sendHeartbeat, requestApproval, request };
