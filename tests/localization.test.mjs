import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const base = new URL('../assets/translations/',import.meta.url);
const read = async name => JSON.parse(await readFile(new URL(name+'.json',base),'utf8'));
const languages = await read('languages');
const source = await read('source');
assert.equal(languages.ku,'Kurmancî');
const slots = value => [...value.matchAll(/%[1-9][0-9]*/g)].map(match=>match[0]).sort();
const canonical = Object.fromEntries([...String(await readFile(new URL('../assets/en.strings',import.meta.url),'utf8')).matchAll(/^"([a-z0-9_.]+)"\s*=\s*("(?:[^"\\]|\\.)*");/gm)].map(match=>[match[1],JSON.parse(match[2])]));
assert.deepEqual(source.reader,canonical,'English reader source stays canonical');
for (const code of Object.keys(languages)) {
  assert.match(code,/^[a-z]{2}$/);
  const catalog = await read(code);
  for (const [group,entries] of Object.entries(source)) {
    assert.deepEqual(Object.keys(catalog[group]).sort(),Object.keys(entries).sort(),`${code}/${group}: all source keys`);
    for (const [key,original] of Object.entries(entries)) {
      const translated = catalog[group][key];
      assert.equal(typeof translated,'string');
      assert.ok(translated.trim(),`${code}/${group}/${key}: nonempty`);
      assert.deepEqual(slots(translated),slots(original),`${code}/${group}/${key}: format slots`);
      assert.ok(!/ZXQPH\d+ZXQ/i.test(translated),`${code}/${group}/${key}: no generator tokens`);
    }
  }
}
console.log('PASS: Every reader, tools and website catalog is complete and preserves format slots, including Kurmancî');
