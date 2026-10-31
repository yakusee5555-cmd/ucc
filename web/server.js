'use strict';

/*
 * UCC Web IDE backend — Node.js + Express + WebSocket.
 *
 * Serves the static frontend, exposes POST /api/compile-run for one-shot
 * compile+run with optional compiler-inspection payloads, and a WebSocket
 * endpoint /ws for interactive sessions (streaming stdout/stderr, live stdin).
 *
 * Security posture: the compiler is always spawned via child_process.spawn
 * with an argv array — no shell is used anywhere. Sources are written to
 * randomly-named temp files under os.tmpdir() and unlinked after each run.
 */

const express = require('express');
const http = require('http');
const path = require('path');
const os = require('os');
const fs = require('fs');
const crypto = require('crypto');
const { spawn } = require('child_process');
const WebSocket = require('ws');

const PORT = process.env.PORT || 3000;
const UCC_BIN = process.env.UCC_BIN || path.join(__dirname, '..', 'build', 'ucc');

const RUN_TIMEOUT_MS = 5000;          // hard wall-clock cap per HTTP run
const WS_IDLE_TIMEOUT_MS = 30000;     // idle kill for interactive sessions
const MAX_CONCURRENT = 8;             // 429 beyond this
const MAX_SOURCE_BYTES = 128 * 1024;
const MAX_STDIN_BYTES = 64 * 1024;
const OUTPUT_CAP = 1024 * 1024;       // 1 MB stdout/stderr cap each
const INSPECT_CAP = 200 * 1024;       // 200 KB per inspect dump
const TRUNC_MARKER = '[output truncated]';

let activeRuns = 0;

const app = express();
app.disable('x-powered-by');
app.use(express.json({ limit: '256kb' }));

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// Byte-capped string accumulator for child output.
function makeCapped(cap, marker) {
  const chunks = [];
  let size = 0;
  let truncated = false;
  return {
    push(s) {
      if (truncated || !s) return;
      const n = Buffer.byteLength(s, 'utf8');
      if (size + n > cap) { truncated = true; return; }
      chunks.push(s);
      size += n;
    },
    text() {
      const t = chunks.join('');
      return truncated ? (t ? t + '\n' : '') + marker : t;
    },
  };
}

async function writeTempSource(source) {
  const name = 'ucc-' + crypto.randomBytes(8).toString('hex') + '.c';
  const file = path.join(os.tmpdir(), name);
  await fs.promises.writeFile(file, source, 'utf8');
  return file;
}

function unlinkQuiet(p) {
  fs.promises.unlink(p).catch(() => {});
}

// Best-effort peak RSS of the child via /proc/<pid>/status (VmHWM).
// Returns null where /proc is unavailable (non-Linux).
function trackPeakRss(child) {
  const pid = child.pid;
  if (!pid) return null;
  let peak = 0;
  let done = false;
  const poll = () => {
    if (done) return;
    try {
      const st = fs.readFileSync('/proc/' + pid + '/status', 'utf8');
      const m = st.match(/^VmHWM:\s+(\d+)\s+kB$/m);
      if (m) {
        const b = parseInt(m[1], 10) * 1024;
        if (b > peak) peak = b;
      }
    } catch (e) { /* process gone or /proc unavailable */ }
  };
  const timer = setInterval(poll, 50);
  poll();
  return {
    stop() {
      done = true;
      clearInterval(timer);
      poll();
      return peak > 0 ? peak : null;
    },
  };
}

function clampSteps(v) {
  const n = Number(v);
  if (!Number.isFinite(n)) return { error: 'maxSteps must be a number' };
  return { value: String(Math.min(100000000, Math.max(1, Math.floor(n)))) };
}

// ---------------------------------------------------------------------------
// one-shot compile+run used by POST /api/compile-run
// ---------------------------------------------------------------------------

function execUcc(args, stdinText) {
  return new Promise((resolve) => {
    const t0 = Date.now();
    const out = makeCapped(OUTPUT_CAP, TRUNC_MARKER);
    const err = makeCapped(OUTPUT_CAP, TRUNC_MARKER);
    let child;
    try {
      // argv array — never a shell string.
      child = spawn(UCC_BIN, args, { stdio: ['pipe', 'pipe', 'pipe'] });
    } catch (e) {
      return resolve({
        ok: false, stdout: '', stderr: 'failed to launch compiler: ' + e.message,
        exitCode: null, timeMs: 0, peakMemoryBytes: null, diagnostics: [],
      });
    }
    const rss = trackPeakRss(child);
    let finished = false;
    const finish = (extra) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      const peakMemoryBytes = rss ? rss.stop() : null;
      resolve(Object.assign({
        stdout: out.text(),
        stderr: err.text(),
        timeMs: Date.now() - t0,
        peakMemoryBytes,
        diagnostics: [],
      }, extra));
    };
    const timer = setTimeout(() => {
      try { child.kill('SIGKILL'); } catch (e) {}
      const soFar = err.text();
      finish({
        ok: false,
        exitCode: null,
        stderr: (soFar ? soFar + '\n' : '') + 'execution timed out (5s)',
      });
    }, RUN_TIMEOUT_MS);

    child.on('error', (e) => {
      finish({ ok: false, exitCode: null, stderr: 'failed to launch compiler: ' + e.message });
    });
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (d) => out.push(d));
    child.stderr.on('data', (d) => err.push(d));
    if (stdinText) { try { child.stdin.write(stdinText); } catch (e) {} }
    try { child.stdin.end(); } catch (e) {}
    child.on('close', (code) => {
      const exitCode = code === null ? null : code;
      finish({ ok: exitCode === 0, exitCode });
    });
  });
}

// Re-run with --json-diagnostics and normalize to
// [{file, line, column, severity, message}].
function jsonDiagnostics(file, stepsArg) {
  return new Promise((resolve) => {
    const args = [];
    if (stepsArg) args.push('--max-steps', stepsArg);
    args.push('--json-diagnostics', file);
    const out = makeCapped(INSPECT_CAP, TRUNC_MARKER);
    let child;
    try {
      child = spawn(UCC_BIN, args, { stdio: ['pipe', 'pipe', 'pipe'] });
    } catch (e) { return resolve([]); }
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (d) => out.push(d));
    try { child.stdin.end(); } catch (e) {}
    const timer = setTimeout(() => { try { child.kill('SIGKILL'); } catch (e) {} }, RUN_TIMEOUT_MS);
    child.on('error', () => { clearTimeout(timer); resolve([]); });
    child.on('close', () => {
      clearTimeout(timer);
      try {
        const arr = JSON.parse(out.text());
        if (!Array.isArray(arr)) return resolve([]);
        resolve(arr.map((d) => ({
          file: d.file != null ? String(d.file) : '',
          line: Number(d.line) || 0,
          column: Number(d.column) || 0,
          severity: d.severity ? String(d.severity) : 'error',
          message: d.message ? String(d.message) : '',
        })));
      } catch (e) { resolve([]); }
    });
  });
}

// Run a --dump-* flag, return its stdout truncated at INSPECT_CAP.
function dumpUcc(flag, file, stepsArg) {
  return new Promise((resolve) => {
    const args = [];
    if (stepsArg) args.push('--max-steps', stepsArg);
    args.push(flag, file);
    const out = makeCapped(INSPECT_CAP, TRUNC_MARKER);
    let child;
    try {
      child = spawn(UCC_BIN, args, { stdio: ['pipe', 'pipe', 'pipe'] });
    } catch (e) { return resolve(''); }
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (d) => out.push(d));
    try { child.stdin.end(); } catch (e) {}
    const timer = setTimeout(() => { try { child.kill('SIGKILL'); } catch (e) {} }, RUN_TIMEOUT_MS);
    child.on('error', () => { clearTimeout(timer); resolve(''); });
    child.on('close', () => { clearTimeout(timer); resolve(out.text()); });
  });
}

// ---------------------------------------------------------------------------
// static frontend (explicit routes — server internals are never served)
// ---------------------------------------------------------------------------

app.get('/', (req, res) => res.sendFile(path.join(__dirname, 'index.html')));
app.get('/app.js', (req, res) => res.sendFile(path.join(__dirname, 'app.js')));
app.get('/styles.css', (req, res) => res.sendFile(path.join(__dirname, 'styles.css')));

app.get('/api/health', (req, res) => {
  res.json({ ok: true, ucc: fs.existsSync(UCC_BIN), uccBin: UCC_BIN });
});

// ---------------------------------------------------------------------------
// POST /api/compile-run
// ---------------------------------------------------------------------------

app.post('/api/compile-run', async (req, res) => {
  const body = req.body || {};
  const source = body.source;
  const stdinRaw = body.stdin;
  const inspect = body.inspect === true;

  if (typeof source !== 'string' || source.length === 0) {
    return res.status(400).json({ ok: false, error: 'source is required' });
  }
  if (Buffer.byteLength(source, 'utf8') > MAX_SOURCE_BYTES) {
    return res.status(400).json({ ok: false, error: 'source exceeds 128KB limit' });
  }
  const stdinText = stdinRaw === undefined || stdinRaw === null ? '' : String(stdinRaw);
  if (Buffer.byteLength(stdinText, 'utf8') > MAX_STDIN_BYTES) {
    return res.status(400).json({ ok: false, error: 'stdin exceeds 64KB limit' });
  }
  let stepsArg = null;
  if (body.maxSteps !== undefined && body.maxSteps !== null) {
    const c = clampSteps(body.maxSteps);
    if (c.error) return res.status(400).json({ ok: false, error: c.error });
    stepsArg = c.value;
  }

  if (activeRuns >= MAX_CONCURRENT) {
    return res.status(429).json({ ok: false, error: 'server busy: too many concurrent executions' });
  }
  activeRuns++;
  let file = null;
  try {
    file = await writeTempSource(source);
    const args = [];
    if (stepsArg) args.push('--max-steps', stepsArg);
    args.push(file);
    const result = await execUcc(args, stdinText);
    if (result.exitCode === 1) {
      result.diagnostics = await jsonDiagnostics(file, stepsArg);
    }
    if (inspect && result.exitCode === 0) {
      result.tokens = await dumpUcc('--dump-tokens', file, stepsArg);
      result.ast = await dumpUcc('--dump-ast', file, stepsArg);
      result.bytecode = await dumpUcc('--dump-bytecode', file, stepsArg);
    }
    res.json(result);
  } catch (err) {
    res.status(500).json({ ok: false, error: String((err && err.message) || err) });
  } finally {
    activeRuns--;
    if (file) unlinkQuiet(file);
  }
});

// ---------------------------------------------------------------------------
// WebSocket /ws — interactive sessions
// ---------------------------------------------------------------------------

const server = http.createServer(app);
const wss = new WebSocket.Server({ server, path: '/ws' });

wss.on('connection', (ws) => {
  let child = null;
  let file = null;
  let rss = null;
  let t0 = 0;
  let idleTimer = null;
  let outSize = 0;

  const send = (obj) => {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
  };
  const clearIdle = () => { if (idleTimer) { clearTimeout(idleTimer); idleTimer = null; } };
  const killChild = () => {
    if (child) { try { child.kill('SIGKILL'); } catch (e) {} }
  };
  const cleanup = () => {
    clearIdle();
    killChild();
    if (file) unlinkQuiet(file);
    file = null;
    child = null;
    rss = null;
  };
  const armIdle = () => {
    clearIdle();
    idleTimer = setTimeout(() => {
      send({ type: 'stderr', data: '\n[session idle for 30s — terminated]\n' });
      killChild();
    }, WS_IDLE_TIMEOUT_MS);
  };

  ws.on('message', async (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch (e) {
      send({ type: 'error', message: 'invalid JSON message' });
      return;
    }

    if (msg.type === 'start') {
      if (child) {
        send({ type: 'error', message: 'a session is already running on this socket' });
        return;
      }
      const source = msg.source;
      if (typeof source !== 'string' || source.length === 0) {
        send({ type: 'error', message: 'source is required' });
        return;
      }
      if (Buffer.byteLength(source, 'utf8') > MAX_SOURCE_BYTES) {
        send({ type: 'error', message: 'source exceeds 128KB limit' });
        return;
      }
      let stepsArg = null;
      if (msg.maxSteps !== undefined && msg.maxSteps !== null) {
        const c = clampSteps(msg.maxSteps);
        if (c.error) { send({ type: 'error', message: c.error }); return; }
        stepsArg = c.value;
      }
      try {
        file = await writeTempSource(source);
      } catch (e) {
        send({ type: 'error', message: 'failed to write temp file' });
        return;
      }
      const args = [];
      if (stepsArg) args.push('--max-steps', stepsArg);
      args.push(file);
      try {
        child = spawn(UCC_BIN, args, { stdio: ['pipe', 'pipe', 'pipe'] });
      } catch (e) {
        send({ type: 'error', message: 'failed to launch compiler: ' + e.message });
        cleanup();
        return;
      }
      t0 = Date.now();
      rss = trackPeakRss(child);
      outSize = 0;
      armIdle();
      send({ type: 'started' });

      const pushStream = (kind, d) => {
        if (outSize > OUTPUT_CAP) return;
        outSize += Buffer.byteLength(d, 'utf8');
        if (outSize > OUTPUT_CAP) {
          send({ type: kind, data: '\n' + TRUNC_MARKER + '\n' });
          return;
        }
        send({ type: kind, data: d });
      };
      child.stdout.setEncoding('utf8');
      child.stderr.setEncoding('utf8');
      child.stdout.on('data', (d) => { armIdle(); pushStream('stdout', d); });
      child.stderr.on('data', (d) => { armIdle(); pushStream('stderr', d); });
      child.on('error', (e) => {
        send({ type: 'error', message: 'compiler process error: ' + e.message });
        cleanup();
      });
      child.on('close', async (code) => {
        const timeMs = Date.now() - t0;
        const peak = rss ? rss.stop() : null;
        const exitCode = code === null ? null : code;
        if (exitCode === 1 && file) {
          const diags = await jsonDiagnostics(file, stepsArg);
          send({ type: 'diagnostic', diagnostics: diags });
        }
        send({ type: 'exit', code: exitCode, timeMs, peakMemoryBytes: peak });
        cleanup();
      });
    } else if (msg.type === 'stdin') {
      if (!child) {
        send({ type: 'error', message: 'no running session' });
        return;
      }
      armIdle();
      try {
        child.stdin.write(String(msg.data === undefined || msg.data === null ? '' : msg.data));
      } catch (e) {
        send({ type: 'error', message: 'stdin write failed' });
      }
    } else if (msg.type === 'kill') {
      send({ type: 'stderr', data: '\n[terminated by user]\n' });
      killChild();
    } else {
      send({ type: 'error', message: 'unknown message type: ' + String(msg.type) });
    }
  });

  ws.on('close', cleanup);
  ws.on('error', () => {});
});

server.listen(PORT, () => {
  console.log('UCC Web IDE listening on port ' + PORT + ' (ucc: ' + UCC_BIN + ')');
});
