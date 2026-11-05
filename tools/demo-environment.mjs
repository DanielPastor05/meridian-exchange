import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {startServer} from './process.mjs';

// Public fixtures for isolated demonstrations/tests only, never live provisioning.
export const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
export function directory() { return fs.mkdtempSync(path.join(os.tmpdir(), 'meridian-')); }
export function cleanup(dir) {
  const absolute = fs.realpathSync(dir);
  const prefix = fs.realpathSync(os.tmpdir()) + path.sep + 'meridian-';
  if (!absolute.startsWith(prefix)) throw Error('unexpected test directory');
  for (const name of fs.readdirSync(absolute)) fs.unlinkSync(path.join(absolute, name));
  fs.rmdirSync(absolute);
}
export function accounts(dir) {
  const file = path.join(dir, 'accounts.conf');
  if (!fs.existsSync(file)) {
    const lines = [1, 2, 3].map(id => id + ' ' + String(id).repeat(32) + ' 1000000000 100000 10000 1000000000 1000000');
    fs.writeFileSync(file, lines.join('\n') + '\n', {flag: 'wx', mode: 0o600});
  }
  return file;
}
export const start = (executable, dir, extra = []) => startServer(executable, path.join(dir, 'server.journal'), accounts(dir), extra);
