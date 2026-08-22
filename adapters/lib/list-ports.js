#!/usr/bin/env node
'use strict';

// Prints candidate serial ports, one per line, to help pick --port for
// daemon.js or a direct HardwareBuddyLink. Prints just the path by
// default; --verbose adds manufacturer/serial number when the OS reports
// them, which is the easiest way to tell an ESP32 board apart from other
// serial devices on a busy machine.
const { HardwareBuddyLink } = require('./link');

async function main() {
  const verbose = process.argv.includes('--verbose');
  const ports = await HardwareBuddyLink.listCandidates();
  if (ports.length === 0) {
    console.error('no serial ports found');
    process.exit(1);
  }
  for (const p of ports) {
    if (verbose) {
      console.log(`${p.path}\t${p.manufacturer || ''}\t${p.serialNumber || ''}`);
    } else {
      console.log(p.path);
    }
  }
}

main();
