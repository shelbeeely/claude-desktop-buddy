'use strict';

const { SerialPort } = require('serialport');

// Default USB serial rate — must match src/main.cpp's Serial.begin(115200).
const DEFAULT_BAUD_RATE = 115200;

/**
 * A connection to a Hardware Buddy device — or any device implementing
 * ../../REFERENCE.md's wire protocol — over USB serial.
 *
 * REFERENCE.md documents the protocol over BLE, but the firmware parses
 * USB serial through the exact same code path (src/data.h's _applyJson(),
 * fed by both _usbLine and _btLine). Serial is what every adapter in this
 * directory uses instead of BLE: no OS Bluetooth stack, no pairing/bonding
 * dance, and it works the instant the device is plugged in over USB —
 * which is the common case for a script running next to a CLI tool on the
 * same machine. Nothing here is Claude-specific; this is the same
 * newline-delimited-JSON contract any maker device on REFERENCE.md speaks.
 */
class HardwareBuddyLink {
  constructor({ path, baudRate = DEFAULT_BAUD_RATE } = {}) {
    if (!path) throw new Error('HardwareBuddyLink requires { path } — see listCandidates()');
    this.port = new SerialPort({ path, baudRate });
    this._buf = '';
    // cmd -> queue of {resolve, timer} waiting on the next matching ack,
    // in send order (the device acks in the order it receives commands).
    this._ackWaiters = new Map();
    // prompt id -> {resolve, timer} waiting on a {"cmd":"permission",...}.
    this._permissionWaiters = new Map();
    this.port.on('data', (chunk) => this._onData(chunk));
  }

  /** Lists serial ports so a setup script/CLI flag can pick the right one. */
  static listCandidates() {
    return SerialPort.list();
  }

  _onData(chunk) {
    this._buf += chunk.toString('utf8');
    let idx;
    while ((idx = this._buf.indexOf('\n')) >= 0) {
      const line = this._buf.slice(0, idx).replace(/\r$/, '');
      this._buf = this._buf.slice(idx + 1);
      if (line) this._onLine(line);
    }
  }

  _onLine(line) {
    if (line[0] !== '{') return;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      return;
    }

    // The device sends this in reply to a heartbeat carrying a `prompt` —
    // see REFERENCE.md "Permission decisions".
    if (msg.cmd === 'permission' && typeof msg.id === 'string') {
      const waiter = this._permissionWaiters.get(msg.id);
      if (waiter) {
        clearTimeout(waiter.timer);
        this._permissionWaiters.delete(msg.id);
        waiter.resolve(msg.decision === 'once' ? 'approve' : 'deny');
      }
      return;
    }

    if (typeof msg.ack === 'string') {
      const q = this._ackWaiters.get(msg.ack);
      if (q && q.length) {
        const { resolve, timer } = q.shift();
        clearTimeout(timer);
        resolve(msg);
      }
    }
  }

  _writeLine(obj) {
    this.port.write(JSON.stringify(obj) + '\n');
  }

  /**
   * Sends a heartbeat snapshot (see lib/heartbeat.js for the builder).
   * Fire-and-forget — heartbeats have no ack in the protocol.
   */
  sendHeartbeat(snapshot) {
    this._writeLine(snapshot);
  }

  /** Sends `{"cmd": ...}` and resolves with the matching `{"ack": ...}`. */
  sendCommand(command, timeoutMs = 3000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const q = this._ackWaiters.get(command.cmd);
        if (q) {
          const i = q.findIndex((w) => w.timer === timer);
          if (i >= 0) q.splice(i, 1);
        }
        reject(new Error(`no ack for "${command.cmd}" within ${timeoutMs}ms`));
      }, timeoutMs);
      const q = this._ackWaiters.get(command.cmd) || [];
      q.push({ resolve, timer });
      this._ackWaiters.set(command.cmd, q);
      this._writeLine(command);
    });
  }

  sendOwnerName(name) {
    return this.sendCommand({ cmd: 'owner', name });
  }

  sendDeviceName(name) {
    return this.sendCommand({ cmd: 'name', name });
  }

  /** One-shot time sync — see REFERENCE.md "One-shot on connect". */
  sendTimeSync(date = new Date()) {
    this._writeLine({ time: [Math.floor(date.getTime() / 1000), -date.getTimezoneOffset() * 60] });
  }

  /**
   * Sends a heartbeat carrying a `prompt` and waits for the device's
   * decision. Resolves to 'approve' or 'deny'; rejects on timeout, which
   * every adapter here treats as "fall back to the tool's own normal
   * prompt" rather than blocking a tool call on a device that may not be
   * connected at all — see each adapter's pre-tool-use hook.
   */
  requestApproval({ id, tool, hint }, snapshot = {}, timeoutMs = 60_000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this._permissionWaiters.delete(id);
        reject(new Error(`no decision from device within ${timeoutMs}ms`));
      }, timeoutMs);
      this._permissionWaiters.set(id, { resolve, timer });
      this._writeLine({ ...snapshot, prompt: { id, tool, hint } });
    });
  }

  close() {
    return new Promise((resolve) => this.port.close(resolve));
  }
}

module.exports = { HardwareBuddyLink, DEFAULT_BAUD_RATE };
