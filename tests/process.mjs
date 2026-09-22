import assert from 'node:assert/strict';
import {test} from 'node:test';
import path from 'node:path';
import os from 'node:os';
import {startServer} from '../tools/process.mjs';
import {directory, accounts, cleanup} from '../tools/demo-environment.mjs';
import fs from 'node:fs';

test('failed spawn preserves ENOENT and resolves cleanup', async () => {
  await assert.rejects(startServer(path.join(os.tmpdir(), 'meridian-missing-' + process.pid, 'server'), 'unused.journal', 'unused.conf'),
    error => error.code === 'ENOENT' && !error.message.includes('did not exit'));
});
test('demo restart never overwrites existing account configuration', () => {
  const dir = directory();
  try {
    const file = accounts(dir);
    const changed = fs.readFileSync(file, 'utf8').replace('1'.repeat(32), 'a'.repeat(32));
    fs.writeFileSync(file, changed);
    assert.equal(accounts(dir), file);
    assert.equal(fs.readFileSync(file, 'utf8'), changed);
  } finally { cleanup(dir); }
});
