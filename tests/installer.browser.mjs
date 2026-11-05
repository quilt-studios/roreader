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
const routes = {'/':['platforms/installer/index.html','text/html'], '/app.js':['platforms/installer/app.js','text/javascript'], '/zip.js':['platforms/installer/zip.js','text/javascript'], '/style.css':['platforms/installer/style.css','text/css'], '/logo.png':['assets/roreader-icon.png','image/png']};
const server = createServer(async (request, response) => {
  if (request.url === '/package.zip') { response.end(archive); return; }
  if (request.url === '/package.sha256') { response.end(checksum + '  ROREADER-qt6.zip'); return; }
  const spec = routes[request.url];
  if (!spec) { response.writeHead(404); response.end(); return; }
  response.setHeader('Content-Type', spec[1]); response.end(await readFile(root + '/' + spec[0]));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const browser = await chromium.launch({executablePath:process.env.CHROMIUM_PATH || undefined, headless:true, args:['--no-sandbox']});
try {
  const page = await browser.newPage({viewport:{width:1120,height:1500}});
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
  await mkdir(root+'/build/screenshots/installer',{recursive:true});
  await page.screenshot({path:root+'/build/screenshots/installer/chrome-installer.png',fullPage:true});
  await page.locator('#install').click();
  await page.locator('#result').filter({hasText:'vollständig kopiert und geprüft'}).waitFor();
  assert.deepEqual(await page.evaluate(()=>window.deviceTest.writes),['.adds/kobalt/assets/manifest.json','.kobo/Kobo.tgz']);
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
} finally { await browser.close(); await new Promise(resolve=>server.close(resolve)); }
