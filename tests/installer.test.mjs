import test from 'node:test';
import assert from 'node:assert/strict';
import {zip} from './support/installer_zip.mjs';
import {readZip, extract, firmwareFromVersion} from '../platforms/installer/zip.js';


const valid = [['.adds/kobalt/assets/manifest.json', '{}'], ['.kobo/Kobo.tgz', 'system archive']];
test('stored and deflated Qt6 ZIP contents are verified', async () => {
  for (const method of [0, 8]) {
    const entries = readZip(zip(valid, method));
    assert.equal(entries.length, 2);
    assert.equal(new TextDecoder().decode(await extract(entries[1])), 'system archive');
  }
});
test('corrupt bytes are rejected before a device is written', async () => {
  const entries = readZip(zip(valid));
  entries[1].compressed[0] ^= 1;
  await assert.rejects(extract(entries[1]), /Prüfsumme/);
});
test('traversal, unsupported packages and case collisions are rejected', () => {
  for (const path of ['../escape', '/absolute', '.adds/kobalt/assets/../../book.epub', '.adds/kobalt/assets/x\\y', '.kobo/KoboRoot.tgz', 'book.epub']) {
    assert.throws(() => readZip(zip([...valid, [path, 'bad']])));
  }
  assert.throws(() => readZip(zip([...valid, ['.ADDS/KOBALT/ASSETS/MANIFEST.JSON', '{}']])));
  assert.throws(() => readZip(zip([valid[1]])), /Manifest/);
  assert.throws(() => readZip(new ArrayBuffer(0)));
});
test('local header disagreement and truncated data are rejected', () => {
  const archive = zip(valid);
  new Uint8Array(archive)[30] ^= 1;
  assert.throws(() => readZip(archive), /Dateiname/);
  const full = zip(valid);
  assert.throws(() => readZip(full.slice(0, -1)));
});
test('only firmware families 5 and 6 are accepted', () => {
  assert.equal(firmwareFromVersion('SERIAL,6.0.2,other'), '6.0.2');
  assert.equal(firmwareFromVersion('SERIAL,5.18.12345,other'), '5.18.12345');
  assert.throws(() => firmwareFromVersion('SERIAL,4.46.23456,other'));
  assert.throws(() => firmwareFromVersion('unknown'));
});
