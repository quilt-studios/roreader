import {readZip, extract, sha256, firmwareFromVersion} from './zip.js';
const $ = id => document.getElementById(id);
let device, firmware, entries, verifiedHash, busy = false;
function message(id, text, error = false) { $(id).textContent = text; $(id).classList.toggle('error', error); }
function update() { $('install').disabled = busy || !device || !entries || !/^[a-fA-F0-9]{64}$/.test($('checksum').value.trim()) || !$('backup').checked; }
async function setPackage(buffer, label) {
  if (buffer.byteLength > 512 * 1024 * 1024) throw Error('Maximale Paketgröße: 512 MiB.');
  const parsed = readZip(buffer);
  const hash = await sha256(buffer);
  for (const entry of parsed) { await extract(entry); }
  verifiedHash = hash;
  entries = parsed;
  message('package-status', `${label} · ${parsed.length} Dateien · geprüft und bereit.`);
}
async function loadBundledPackage() {
  try {
    const checksum = await fetch('/package.sha256');
    if (!checksum.ok) { $('manual-package').open = true; return; }
    const hash = (await checksum.text()).trim().split(/\s+/)[0];
    if (!/^[a-fA-F0-9]{64}$/.test(hash)) throw Error('Ungültige Prüfsummendatei.');
    message('package-status', 'Beiliegendes ROREADER-Paket wird geprüft …');
    const packageResponse = await fetch('/package.zip');
    if (!packageResponse.ok) throw Error('Beiliegendes ROREADER-Paket fehlt.');
    const buffer = await packageResponse.arrayBuffer();
    if (await sha256(buffer) !== hash.toLowerCase()) throw Error('Beiliegendes Paket stimmt nicht mit seiner SHA-256-Prüfsumme überein.');
    if ($('package').files.length) return;
    $('checksum').value = hash;
    await setPackage(buffer, 'Beiliegendes ROREADER-Paket');
  } catch (error) { entries = null; $('manual-package').open = true; message('package-status', error.message, true); }
  update();
}
if (!window.showDirectoryPicker || !window.DecompressionStream) {
  message('browser', 'Bitte diese Seite in einer aktuellen Version von Microsoft Edge oder Google Chrome öffnen.', true);
  $('device').disabled = true;
}
$('device').onclick = async () => {
  device = null; update();
  try {
    const selected = await showDirectoryPicker({mode: 'readwrite'});
    const kobo = await selected.getDirectoryHandle('.kobo');
    const version = await (await kobo.getFileHandle('version')).getFile();
    if (version.size > 16384) throw Error('Ungültige Versionsdatei.');
    firmware = firmwareFromVersion(await version.text());
    device = selected;
    message('device-status', `${selected.name} · Firmware ${firmware}. Modellkompatibilität ist damit noch nicht bestätigt.`);
  } catch (error) { message('device-status', error.name === 'AbortError' ? 'Auswahl abgebrochen.' : error.message, true); }
  update();
};
$('package').onchange = async () => {
  entries = null; verifiedHash = null; update();
  try {
    const file = $('package').files[0];
    if (!file) return;
    if (file.size > 512 * 1024 * 1024) throw Error('Maximale Paketgröße: 512 MiB.');
    await setPackage(await file.arrayBuffer(), file.name);
  } catch (error) { message('package-status', error.message, true); }
  update();
};
$('checksum').oninput = update;
$('backup').onchange = update;
async function writeFile(path, bytes, undo) {
  const parts = path.split('/');
  let directory = device;
  for (const name of parts.slice(0, -1)) directory = await directory.getDirectoryHandle(name, {create: true});
  const name = parts.at(-1);
  let previous = null;
  try {
    const original = await directory.getFileHandle(name);
    const originalFile = await original.getFile();
    const retained = undo.reduce((total, change) => total + (change.previous?.byteLength || 0), 0);
    if (originalFile.size + retained > 512 * 1024 * 1024) throw Error('Vorhandene Dateien sind zu groß für eine sichere Rücknahme.');
    previous = await originalFile.arrayBuffer();
  } catch (error) { if (error.name !== 'NotFoundError') throw error; }
  const file = await directory.getFileHandle(name, {create: true});
  undo.push({directory, name, previous, file});
  const writer = await file.createWritable();
  try { await writer.write(bytes); await writer.close(); }
  catch (error) { await writer.abort().catch(() => {}); throw error; }
  const actual = await (await file.getFile()).arrayBuffer();
  if (await sha256(actual) !== await sha256(bytes)) throw Error(`Schreibprüfung fehlgeschlagen: ${path}`);
}
$('install').onclick = async () => {
  if ($('install').disabled) return;
  if ($('checksum').value.trim().toLowerCase() !== verifiedHash) { message('result', 'SHA-256 stimmt nicht mit der erwarteten Prüfsumme überein. Es wurde nichts geschrieben.', true); return; }
  busy = true; update();
  for (const id of ['device', 'package', 'checksum', 'backup']) $(id).disabled = true;
  const progress = $('progress'); progress.hidden = false; progress.max = entries.length;
  const undo = [];
  try {
    const kobo = await device.getDirectoryHandle('.kobo');
    const current = firmwareFromVersion(await (await (await kobo.getFileHandle('version')).getFile()).text());
    if (current !== firmware) throw Error('Firmware oder ausgewähltes Gerät hat sich geändert. Bitte neu auswählen.');
    // Do not overwrite an already staged firmware update or another installer.
    for (const name of ['Kobo.tgz', 'KoboRoot.tgz']) {
      try { await kobo.getFileHandle(name); throw Error(`${name} liegt bereits auf dem Gerät. Vorhandenes Update zuerst abschließen.`); }
      catch (error) { if (error.name !== 'NotFoundError') throw error; }
    }
    const ordered = [...entries].sort((a, b) => Number(a.name.startsWith('.kobo/')) - Number(b.name.startsWith('.kobo/')));
    for (let i = 0; i < ordered.length; i++) {
      const entry = ordered[i];
      message('result', `Kopiere ${entry.name}`);
      await writeFile(entry.name, await extract(entry), undo);
      progress.value = i + 1;
    }
    message('result', 'Paket vollständig kopiert und geprüft. Jetzt sicher auswerfen, USB trennen und am Reader warten. Die erfolgreiche Geräteinstallation ist damit noch nicht geprüft.');
  } catch (error) {
    let rollbackError = false;
    // Restore overwritten assets and remove only files created by this attempt.
    for (const change of undo.reverse()) {
      try {
        if (change.previous === null) await change.directory.removeEntry(change.name);
        else {
          const writer = await change.file.createWritable();
          await writer.write(change.previous); await writer.close();
        }
      } catch { rollbackError = true; }
    }
    message('result', `Installation nicht abgeschlossen: ${error.message}${rollbackError ? ' Rücknahme unvollständig: Gerät erneut verbinden und Dateien prüfen.' : undo.length ? ' Kopierte Dateien wurden zurückgenommen.' : ' Es wurde nichts geschrieben.'}`, true);
  } finally {
    busy = false;
    for (const id of ['device', 'package', 'checksum', 'backup']) $(id).disabled = false;
    update();
  }
};
window.addEventListener('beforeunload', event => { if (busy) { event.preventDefault(); event.returnValue = ''; } });
loadBundledPackage();
