import {mkdtemp, writeFile, readFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import assert from 'node:assert/strict';
import {zip} from './support/installer_zip.mjs';
const directory=await mkdtemp(join(tmpdir(),'roreader-pages-'));
try {
  const archive=Buffer.from(zip([['.adds/kobalt/assets/manifest.json','{}'],['.kobo/Kobo.tgz','fixture']],8));
  const file=join(directory,'ROREADER-qt6.zip');const output=join(directory,'site');
  const hash=createHash('sha256').update(archive).digest('hex');
  await writeFile(file,archive);await writeFile(file+'.sha256',hash+'  ROREADER-qt6.zip\n');
  const build=()=>spawnSync(process.execPath,['platforms/installer/build-site.mjs','--package',file,'--output',output],{encoding:'utf8'});
  let result=build();assert.equal(result.status,0,result.stderr);
  assert.equal((await readFile(join(output,'package.zip'))).compare(archive),0);
  assert.equal((await readFile(join(output,'package.sha256'),'utf8')).trim().split(/\s+/)[0],hash);
  assert.equal(JSON.parse(await readFile(join(output,'site-manifest.json'),'utf8')).package_sha256,hash);
  assert.ok((await readFile(join(output,'index.html'),'utf8')).includes('src="logo.png"'));
  assert.ok((await readFile(join(output,'locale.js'),'utf8')).includes('export function t'));
  assert.equal(JSON.parse(await readFile(join(output,'locales/languages.json'),'utf8')).ku,'Kurmancî');
  assert.ok(JSON.parse(await readFile(join(output,'locales/ku.json'),'utf8')).web['Sprache']);
  result=build();assert.equal(result.status,0,result.stderr);
  await writeFile(file+'.sha256','0'.repeat(64));result=build();assert.notEqual(result.status,0);assert.match(result.stderr,/checksum/);
  assert.equal((await readFile(join(output,'package.zip'))).compare(archive),0);
  console.log('PASS: Static Pages build verifies the device package, supports repeat builds and preserves output on checksum failure');
} finally {await rm(directory,{recursive:true,force:true});}
