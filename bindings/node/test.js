// Test Hermes Node.js bindings.
// Run: node bindings/node/test.js

const path = require('path');
const fs = require('fs');

// Set library path
const libPath = path.join(__dirname, '..', '..', 'build',
  process.platform === 'darwin' ? 'libkanbudb_shared.dylib' : 'libkanbudb_shared.so');
process.env.KANBUDB_LIB = libPath;

const kanbudb = require('./index');

const DB_PATH = '/tmp/kanbudb_test_node';

function cleanup() {
  try { fs.unlinkSync(DB_PATH + '.wal'); } catch(e) {}
  try { fs.unlinkSync(DB_PATH + '.lsm'); } catch(e) {}
}

function testLifecycle() {
  const db = kanbudb.open(DB_PATH);
  db.close();
  console.log('  PASS: lifecycle');
}

function testCRUD() {
  const db = kanbudb.open(DB_PATH);
  db.createTable('t1', { id: 'string', val: 'string' }, 'id');
  db.put('t1', 'k1', Buffer.from('hello'));

  const val = db.get('t1', 'k1');
  if (!val || val.toString() !== 'hello') throw new Error(`bad get: ${val}`);

  const missing = db.get('t1', 'nope');
  if (missing !== null) throw new Error('expected null');

  db.delete('t1', 'k1');
  const after = db.get('t1', 'k1');
  if (after !== null) throw new Error('expected null after delete');

  db.close();
  console.log('  PASS: crud');
}

function testFTS() {
  const db = kanbudb.open(DB_PATH);
  db.createTable('docs', { id: 'string', body: 'string' }, 'id');
  db.ftsCreateIndex('docs', 'body');
  db.put('docs', 'd1', Buffer.from('hello world database'));

  const rs = db.ftsSearch('docs', 'body', 'hello');
  if (!rs) throw new Error('expected result set');
  rs.close();
  db.close();
  console.log('  PASS: fts');
}

cleanup();
testLifecycle();
testCRUD();
testFTS();
cleanup();
console.log('\nAll Node.js tests passed!');
