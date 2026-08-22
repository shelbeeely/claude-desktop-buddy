'use strict';

// Both Copilot CLI and Codex CLI hooks pass their event payload as JSON on
// stdin — see each adapter's README for the exact shape. This just reads
// and parses it; empty stdin resolves to {} rather than throwing, since a
// hook invoked with no payload shouldn't crash the tool it's hooked into.
function readStdinJson() {
  return new Promise((resolve, reject) => {
    let data = '';
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', (chunk) => {
      data += chunk;
    });
    process.stdin.on('end', () => {
      if (!data.trim()) return resolve({});
      try {
        resolve(JSON.parse(data));
      } catch (err) {
        reject(err);
      }
    });
    process.stdin.on('error', reject);
  });
}

module.exports = { readStdinJson };
