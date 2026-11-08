import {fileURLToPath} from 'node:url';
import {zip} from './support/installer_zip.mjs';
import {installDeviceMock} from './support/installer_device_mock.mjs';
const {chromium} = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
import {createServer} from 'node:http';
import {readFile, mkdir} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const root = fileURLToPath(new URL('..', import.meta.url)).replace(/\/$/, '');
const archive = Buffer.from(zip([['.adds/kobalt/assets/manifest.json', '{}'], ['.kobo/Kobo.tgz', 'test system archive']], 8));
const checksum = createHash('sha256').update(archive).digest('hex');
const requests = [];
const routes = {'/':['platforms/installer/index.html','text/html'], '/locale.js':['platforms/installer/locale.js','text/javascript'], '/app.js':['platforms/installer/app.js','text/javascript'], '/zip.js':['platforms/installer/zip.js','text/javascript'], '/style.css':['platforms/installer/style.css','text/css'], '/logo.png':['assets/roreader-icon.png','image/png']};
const server = createServer(async (request, response) => {
  requests.push(request.url);
  const pathname = new URL(request.url,'http://localhost').pathname;
  const path = pathname.startsWith('/roreader/') ? pathname.slice('/roreader'.length) : pathname;
  if (path === '/package.zip') { response.end(archive); return; }
  if (path === '/package.sha256') { response.end(checksum + '  ROREADER-qt6.zip'); return; }
  const spec = /^\/locales\/[a-z]+\.json$/.test(path) ? ['assets/translations/'+path.split('/').at(-1),'application/json'] : routes[path];
  if (!spec) { response.writeHead(404); response.end(); return; }
  response.setHeader('Content-Type', spec[1]); response.end(await readFile(root + '/' + spec[0]));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const browser = await chromium.launch({executablePath:process.env.CHROMIUM_PATH || undefined, headless:true, args:['--no-sandbox']});
try {
  const page = await browser.newPage({viewport:{width:1120,height:1500},locale:'de-DE'});
  const errors=[]; page.on('pageerror', error=>errors.push(error.message));
  await page.addInitScript(installDeviceMock);
  const url=`http://127.0.0.1:${server.address().port}/`;
  async function prepare() {
    await page.goto(url);
    await page.getByText('Beiliegendes ROREADER-Paket ·').waitFor();
    await page.locator('#device').click();
    await page.locator('#backup').check();
  }
  await prepare();
  assert.equal(await page.locator('#install').textContent(),'ROREADER aktualisieren');
  await page.locator('#device-status').filter({hasText:'Installation erkannt'}).waitFor();
  await page.evaluate(()=>{
    for (const path of ['books/example.epub','.adds/kobalt/profile/settings.json','ROREADER/notizen.txt'])
      window.deviceTest.files[path]=new TextEncoder().encode('keep me');
  });
  await mkdir(root+'/build/screenshots/installer',{recursive:true});
  await page.screenshot({path:root+'/build/screenshots/installer/chrome-installer.png',fullPage:true});
  await page.locator('#install').click();
  await page.locator('#result').filter({hasText:'vollständig kopiert und geprüft'}).waitFor();
  assert.deepEqual(await page.evaluate(()=>window.deviceTest.writes),['.adds/kobalt/assets/manifest.json','.kobo/Kobo.tgz']);
  assert.equal(await page.evaluate(()=>new TextDecoder().decode(window.deviceTest.files['.adds/kobalt/assets/manifest.json'])),'{}');
  for (const path of ['books/example.epub','.adds/kobalt/profile/settings.json','ROREADER/notizen.txt'])
    assert.equal(await page.evaluate(path=>new TextDecoder().decode(window.deviceTest.files[path]),path),'keep me');
  console.log('PASS: Installed ROREADER detected and replaced; books, profile and documents preserved');
  await page.goto(url);
  await page.getByText('Beiliegendes ROREADER-Paket ·').waitFor();
  await page.evaluate(()=>{delete window.deviceTest.files['.adds/kobalt/assets/manifest.json'];});
  await page.locator('#device').click();
  assert.equal(await page.locator('#install').textContent(),'ROREADER auf den Reader kopieren');
  await page.locator('#backup').check();
  await page.locator('#install').click();
  await page.locator('#result').filter({hasText:'vollständig kopiert und geprüft'}).waitFor();
  console.log('PASS: Chrome flow selects device root, auto-verifies package, writes assets before activation');
  await prepare();
  await page.evaluate(()=>{window.deviceTest.files['.kobo/Kobo.tgz']=new TextEncoder().encode('existing update');});
  await page.locator('#install').click();
  await page.locator('#result').filter({hasText:'liegt bereits'}).waitFor();
  assert.equal(await page.evaluate(()=>new TextDecoder().decode(window.deviceTest.files['.kobo/Kobo.tgz'])),'existing update');
  assert.deepEqual(await page.evaluate(()=>window.deviceTest.writes),[]);
  console.log('PASS: Existing update preserved and no writes performed');
  await prepare();
  await page.evaluate(()=>{window.deviceTest.badReadback=true;});
  await page.locator('#install').click();
  await page.locator('#result').filter({hasText:'Schreibprüfung fehlgeschlagen'}).waitFor();
  assert.equal(await page.evaluate(()=>'.kobo/Kobo.tgz' in window.deviceTest.files),false);
  assert.equal(await page.evaluate(()=>new TextDecoder().decode(window.deviceTest.files['.adds/kobalt/assets/manifest.json'])),'old assets');
  console.log('PASS: Readback failure removes attempted activation and restores old assets');
  await page.goto(url);
  await page.evaluate(()=>{window.deviceTest.files['.kobo/version']=new TextEncoder().encode('SERIAL,4.46.23456,DEVICE');});
  await page.locator('#device').click();
  await page.locator('#device-status').filter({hasText:'Keine unterstützte Firmware'}).waitFor();
  assert.equal(await page.locator('#install').isDisabled(),true);
  assert.deepEqual(errors,[]);
  console.log('PASS: Unsupported firmware is rejected; no browser script errors');
  requests.length = 0;
  await page.goto(url + 'roreader/');
  await page.getByText('Beiliegendes ROREADER-Paket ·').waitFor();
  assert.ok(requests.includes('/roreader/package.zip'));
  assert.ok(requests.includes('/roreader/package.sha256'));
  assert.ok(requests.includes('/roreader/logo.png'));
  assert.ok(!requests.includes('/package.zip'));
  await page.locator('#device').click();
  await page.locator('#backup').check();
  await page.locator('#install').click();
  await page.locator('#result').filter({hasText:'vollständig kopiert und geprüft'}).waitFor();
  assert.deepEqual(errors,[]);
  console.log('PASS: Project Pages subdirectory uses relative assets and installs correctly');
  const supported = JSON.parse(await readFile(root+'/assets/translations/languages.json','utf8'));
  for (const code of Object.keys(supported)) {
    await page.goto(url+'?lang='+code);
    await page.locator('#language option').nth(Object.keys(supported).length-1).waitFor({state:'attached'});
    assert.equal(await page.locator('html').getAttribute('lang'),code);
    const catalog = JSON.parse(await readFile(root+'/assets/translations/'+code+'.json','utf8')).web;
    assert.equal(await page.locator('#device').textContent(),catalog['Reader-Laufwerk auswählen']);
    await page.locator('#device').click();
    await page.locator('#install').filter({hasText:catalog['ROREADER aktualisieren']}).waitFor();
    assert.equal(await page.locator('#install').textContent(),catalog['ROREADER aktualisieren']);
    if (['ku','uk','fr'].includes(code)) await page.screenshot({path:root+'/build/screenshots/installer/language-'+code+'.png',fullPage:true});
  }
  await page.locator('#language').selectOption('fr');
  await page.waitForURL('**lang=fr');
  await page.locator('#language option[value=fr]').waitFor({state:'attached'});
  await page.locator('#language').selectOption('ku');
  await page.waitForURL('**lang=ku');
  await page.locator('#language option[value=ku]').waitFor({state:'attached'});
  assert.equal(await page.locator('#language').inputValue(),'ku');
  await page.goto(url);
  await page.locator('#language option[value=ku]').waitFor({state:'attached'});
  assert.equal(await page.locator('#language').inputValue(),'ku');
  assert.equal(await page.locator('html').getAttribute('lang'),'ku');
  assert.deepEqual(errors,[]);
  console.log('PASS: Every language renders its catalog, including Kurmancî, and the picker switches language');

} finally { await browser.close(); await new Promise(resolve=>server.close(resolve)); }
