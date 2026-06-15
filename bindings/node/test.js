// Test Hermes Node.js bindings.
// Run: node bindings/node/test.js

const path = require('path');
const fs = require('fs');

// Set library path
const libPath = path.join(__dirname, '..', '..', 'build',
  process.platform === 'darwin' ? 'libkanbudb_shared.dylib' : 'libkanbudb_shared.so');
process.env.KANBUDB_LIB = libPath;

const { Condition } = require('./index');
const kanbudb = require('./index');

function cleanup(dbpath) {
  try { fs.unlinkSync(dbpath + '.wal'); } catch(e) {}
  try { fs.unlinkSync(dbpath + '.lsm'); } catch(e) {}
  try { fs.unlinkSync(dbpath + '.system'); } catch(e) {}
}

function testLifecycle() {
  const p = '/tmp/kb_test_lifecycle';
  cleanup(p);
  const db = kanbudb.open(p);
  db.close();
  cleanup(p);
  console.log('  PASS: lifecycle');
}

function testCRUD() {
  const p = '/tmp/kb_test_crud';
  cleanup(p);
  const db = kanbudb.open(p);
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
  cleanup(p);
  console.log('  PASS: crud');
}

function testFTS() {
  const p = '/tmp/kb_test_fts';
  cleanup(p);
  const db = kanbudb.open(p);
  db.createTable('docs', { id: 'string', body: 'string' }, 'id');
  db.ftsCreateIndex('docs', 'body');
  db.put('docs', 'd1', Buffer.from('hello world database'));

  const rs = db.ftsSearch('docs', 'body', 'hello');
  if (!rs) throw new Error('expected result set');
  rs.close();
  db.close();
  cleanup(p);
  console.log('  PASS: fts');
}

function testMultiAnd() {
  const p = '/tmp/kb_test_ma';
  cleanup(p);
  const db = kanbudb.open(p);
  db.createTable('users', { id: 'string', name: 'string', age: 'int32' }, 'id');
  db.putRow('users', 'u1', { id: 'u1', name: 'alice', age: 25 });
  db.putRow('users', 'u2', { id: 'u2', name: 'bob', age: 17 });
  db.putRow('users', 'u3', { id: 'u3', name: 'charlie', age: 30 });
  db.putRow('users', 'u4', { id: 'u4', name: 'diana', age: 22 });

  const c1 = new Condition('age', '>', '18');
  const c2 = new Condition('age', '<', '28');
  const rs = db.query('users').where(Condition.and(c1, c2)).exec();
  let count = 0;
  for (const _ of rs) count++;
  rs.close();
  if (count !== 2) throw new Error(`expected 2, got ${count}`);
  db.close();
  cleanup(p);
  console.log('  PASS: multi_and');
}

function testMultiOr() {
  const p = '/tmp/kb_test_mo';
  cleanup(p);
  const db = kanbudb.open(p);
  db.createTable('users', { id: 'string', name: 'string', age: 'int32' }, 'id');
  db.putRow('users', 'u1', { id: 'u1', name: 'alice', age: 25 });
  db.putRow('users', 'u2', { id: 'u2', name: 'bob', age: 12 });

  const c1 = new Condition('age', '<', '15');
  const c2 = new Condition('age', '>', '28');
  const rs = db.query('users').where(Condition.or(c1, c2)).exec();
  let count = 0;
  for (const _ of rs) count++;
  rs.close();
  if (count !== 1) throw new Error(`expected 1, got ${count}`);
  db.close();
  cleanup(p);
  console.log('  PASS: multi_or');
}

function testNot() {
  const p = '/tmp/kb_test_not';
  cleanup(p);
  const db = kanbudb.open(p);
  db.createTable('users', { id: 'string', name: 'string', age: 'int32' }, 'id');
  db.putRow('users', 'u1', { id: 'u1', name: 'alice', age: 25 });
  db.putRow('users', 'u2', { id: 'u2', name: 'bob', age: 17 });

  const c = new Condition('age', '>', '20');
  const rs = db.query('users').where(Condition.not(c)).exec();
  let count = 0;
  for (const _ of rs) count++;
  rs.close();
  if (count !== 1) throw new Error(`expected 1, got ${count}`);
  db.close();
  cleanup(p);
  console.log('  PASS: not');
}

function testNested() {
  const p = '/tmp/kb_test_nest';
  cleanup(p);
  const db = kanbudb.open(p);
  db.createTable('users', { id: 'string', name: 'string', age: 'int32' }, 'id');
  db.putRow('users', 'u1', { id: 'u1', name: 'alice', age: 25 });
  db.putRow('users', 'u2', { id: 'u2', name: 'bob', age: 17 });
  db.putRow('users', 'u3', { id: 'u3', name: 'charlie', age: 30 });

  const left = Condition.and(new Condition('age', '>', '20'), new Condition('age', '<', '28'));
  const right = new Condition('name', '=', 'bob');
  const rs = db.query('users').where(Condition.or(left, right)).exec();
  let count = 0;
  for (const _ of rs) count++;
  rs.close();
  if (count !== 2) throw new Error(`expected 2, got ${count}`);
  db.close();
  cleanup(p);
  console.log('  PASS: nested');
}

testLifecycle();
testCRUD();
testFTS();
testMultiAnd();
testMultiOr();
testNot();
testNested();
console.log('\nAll Node.js tests passed!');
