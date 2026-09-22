import {spawn} from 'node:child_process';
import {EventEmitter} from 'node:events';

// Process supervision only: never creates accounts or changes configuration.
export async function startServer(executable, journal, accounts, extra = []) {
  const env = Object.fromEntries(Object.entries(process.env).map(([key, value]) => [process.platform === 'win32' ? key.toUpperCase() : key, value]));
  const child = spawn(executable, ['--journal', journal, '--accounts', accounts, '--port', '0', ...extra],
    {env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']});
  const events = new EventEmitter();
  const messages = [];
  let buffer = '', stderr = '', ended = false, spawnError;
  // 'close' follows both exit and a failed spawn; failed spawns have no 'exit'.
  const closed = new Promise(resolve => child.once('close', (code, signal) => {
    ended = true;
    resolve({code, signal});
    events.emit('closed');
  }));
  child.on('error', error => { spawnError = error; stderr = (stderr + error.message).slice(-16384); });
  child.stdout.on('data', bytes => {
    buffer += bytes.toString();
    for (;;) {
      const end = buffer.indexOf('\n');
      if (end < 0) break;
      const line = buffer.slice(0, end).trim();
      buffer = buffer.slice(end + 1);
      try {
        const item = JSON.parse(line);
        messages.push(item);
        if (messages.length > 64) messages.shift();
        events.emit('message', item);
      } catch { /* Human-readable child output is not a lifecycle message. */ }
    }
    if (buffer.length > 65536) { stderr = 'oversized child status line'; child.kill(); }
  });
  child.stderr.on('data', bytes => { stderr = (stderr + bytes).slice(-16384); });
  const waitFor = (type, timeout = 15000) => new Promise((resolve, reject) => {
    const existing = messages.find(message => message.type === type);
    if (existing) { resolve(existing); return; }
    const finish = (error, value) => {
      clearTimeout(timer);
      events.off('message', onMessage);
      events.off('closed', onClose);
      if (error) reject(error); else resolve(value);
    };
    const onMessage = item => { if (item.type === type) finish(null, item); };
    const onClose = () => finish(spawnError ?? Error('server exited before ' + type + ': ' + stderr));
    const timer = setTimeout(() => finish(Error('server timeout waiting for ' + type + ': ' + stderr)), timeout);
    events.on('message', onMessage);
    events.on('closed', onClose);
    if (ended) onClose();
  });
  const stop = async (signal = 'SIGKILL') => {
    if (!ended && child.pid && child.exitCode === null && child.signalCode === null) child.kill(signal);
    let timer;
    try {
      const result = await Promise.race([closed, new Promise((_, reject) => {
        timer = setTimeout(() => reject(Error('server did not exit: ' + stderr)), 5000);
      })]);
      if (signal === 'SIGTERM' && result.code !== 0) throw Error('graceful server exit failed: ' + stderr);
      return result;
    } finally { clearTimeout(timer); }
  };
  try {
    const ready = await waitFor('ready');
    return {child, ready, port: ready.port, waitFor, stop, closed, get stderr() { return stderr; }};
  } catch (error) {
    try { await stop(); } catch (cleanupError) { throw new AggregateError([error, cleanupError], error.message); }
    throw error;
  }
}
