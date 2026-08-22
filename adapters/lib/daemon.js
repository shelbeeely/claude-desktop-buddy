#!/usr/bin/env node
'use strict';

const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { HardwareBuddyLink } = require('./link');

/**
 * Persistent bridge process: owns the one serial connection to the device
 * and relays requests from short-lived adapter hook scripts over a local
 * socket.
 *
 * Why this exists instead of each hook opening the serial port directly:
 * Copilot CLI and Codex CLI hooks run as one short-lived process per event
 * (a preToolUse hook fires, does its thing, and exits). Many Arduino-core
 * boards — including the ESP32-C3/S3 targets this repo builds for —
 * toggle the target's reset line on DTR when a serial connection opens
 * (the same signal esptool/Arduino IDE use to enter the bootloader), so
 * opening and closing the port on every single tool call would risk
 * resetting the device instead of just talking to it. One daemon holds
 * the port open for the life of an adapter session; hook scripts are
 * cheap, disposable clients of its local socket (see client.js).
 */
function defaultSocketPath() {
  return process.platform === 'win32'
    ? '\\\\.\\pipe\\hardware-buddy-adapter'
    : path.join(os.tmpdir(), 'hardware-buddy-adapter.sock');
}

function start({ path: serialPath, baudRate, socketPath = defaultSocketPath() } = {}) {
  if (!serialPath) throw new Error('start() requires { path } — the device serial port');
  const link = new HardwareBuddyLink({ path: serialPath, baudRate });

  if (process.platform !== 'win32' && fs.existsSync(socketPath)) fs.unlinkSync(socketPath);

  async function handleRequest(socket, line) {
    let req;
    try {
      req = JSON.parse(line);
    } catch {
      return;
    }
    try {
      if (req.type === 'heartbeat') {
        link.sendHeartbeat(req.snapshot);
        socket.write(`${JSON.stringify({ ok: true })}\n`);
      } else if (req.type === 'approval') {
        const decision = await link.requestApproval(
          { id: req.id, tool: req.tool, hint: req.hint },
          req.snapshot || {},
          req.timeoutMs,
        );
        socket.write(`${JSON.stringify({ ok: true, decision })}\n`);
      } else {
        socket.write(`${JSON.stringify({ ok: false, error: `unknown request type "${req.type}"` })}\n`);
      }
    } catch (err) {
      socket.write(`${JSON.stringify({ ok: false, error: err.message })}\n`);
    }
  }

  const server = net.createServer((socket) => {
    let buf = '';
    socket.on('data', (chunk) => {
      buf += chunk.toString('utf8');
      let idx;
      while ((idx = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, idx);
        buf = buf.slice(idx + 1);
        if (line.trim()) handleRequest(socket, line);
      }
    });
    socket.on('error', () => {}); // a hook client that disconnects early is not our problem
  });

  server.listen(socketPath, () => {
    console.error(`[hardware-buddy-adapter] bridging ${serialPath} <-> ${socketPath}`);
  });

  const shutdown = () => {
    server.close();
    link.close().finally(() => process.exit(0));
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);

  return { server, link, socketPath };
}

if (require.main === module) {
  const args = process.argv.slice(2);
  const opts = {};
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--port') opts.path = args[++i];
    else if (args[i] === '--baud') opts.baudRate = Number(args[++i]);
    else if (args[i] === '--socket') opts.socketPath = args[++i];
  }
  if (!opts.path) {
    console.error('usage: node daemon.js --port <serial-port> [--baud 115200] [--socket <path>]');
    console.error('       node ../lib/daemon.js --port $(node ../lib/list-ports.js)');
    process.exit(1);
  }
  start(opts);
}

module.exports = { start, defaultSocketPath };
