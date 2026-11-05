const decoder = new TextDecoder('utf-8', {fatal: true});
const MAX_TOTAL = 512 * 1024 * 1024;

export function crc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function readZip(buffer) {
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  let end = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) {
    if (view.getUint32(i, true) === 0x06054b50 && i + 22 + view.getUint16(i + 20, true) === bytes.length) { end = i; break; }
  }
  if (end < 0 || view.getUint16(end + 4, true) || view.getUint16(end + 6, true)) throw Error('Kein unterstütztes ZIP-Paket.');
  const count = view.getUint16(end + 10, true);
  const centralEnd = view.getUint32(end + 16, true) + view.getUint32(end + 12, true);
  if (!count || count > 5000 || count !== view.getUint16(end + 8, true) || centralEnd !== end) throw Error('Ungültiges oder zu großes ZIP-Verzeichnis.');
  let position = view.getUint32(end + 16, true), total = 0;
  const entries = [], names = new Set();
  for (let i = 0; i < count; i++) {
    if (position + 46 > centralEnd || view.getUint32(position, true) !== 0x02014b50) throw Error('Beschädigtes ZIP-Verzeichnis.');
    const flags = view.getUint16(position + 8, true), method = view.getUint16(position + 10, true);
    const checksum = view.getUint32(position + 16, true), compressed = view.getUint32(position + 20, true), size = view.getUint32(position + 24, true);
    const nameSize = view.getUint16(position + 28, true), extra = view.getUint16(position + 30, true), comment = view.getUint16(position + 32, true);
    const attributes = view.getUint32(position + 38, true), offset = view.getUint32(position + 42, true);
    if (position + 46 + nameSize + extra + comment > centralEnd) throw Error('Ungültiger ZIP-Eintrag.');
    const name = decoder.decode(bytes.subarray(position + 46, position + 46 + nameSize));
    position += 46 + nameSize + extra + comment;
    if (flags & 1 || ![0, 8].includes(method) || (attributes >>> 16 & 0xf000) === 0xa000 || view.getUint16(position - 46 - nameSize - extra - comment + 34, true)) throw Error('Verschlüsselte, verlinkte oder verteilte ZIP-Einträge sind nicht erlaubt.');
    const parts = name.replace(/\/$/, '').split('/');
    if (!name || name.includes('\\') || name.includes(':') || /[\x00-\x1f]/.test(name) || parts.some(part => !part || part === '.' || part === '..' || /[. ]$/.test(part))) throw Error('Unsicherer Dateipfad im Paket.');
    const key = name.toLowerCase();
    if (names.has(key)) throw Error('Doppelte Dateipfade im Paket.');
    names.add(key);
    if (name.endsWith('/')) {
      if (!['.kobo/', '.adds/', '.adds/kobalt/', '.adds/kobalt/assets/'].includes(name) && !name.startsWith('.adds/kobalt/assets/')) throw Error('Unerwarteter Paketordner.');
      continue;
    }
    if (name !== '.kobo/Kobo.tgz' && !name.startsWith('.adds/kobalt/assets/')) throw Error('Das Paket enthält Dateien außerhalb des Qt6-Installationsbereichs.');
    total += size;
    if (total > MAX_TOTAL || compressed > MAX_TOTAL || offset + 30 > view.getUint32(end + 16, true)) throw Error('Paket ist zu groß oder beschädigt.');
    if (view.getUint32(offset, true) !== 0x04034b50 || view.getUint16(offset + 8, true) !== method || view.getUint16(offset + 6, true) !== flags) throw Error('ZIP-Dateikopf stimmt nicht überein.');
    const localNameLength = view.getUint16(offset + 26, true);
    if (decoder.decode(bytes.subarray(offset + 30, offset + 30 + localNameLength)) !== name) throw Error('ZIP-Dateiname stimmt nicht überein.');
    const start = offset + 30 + localNameLength + view.getUint16(offset + 28, true);
    if (start + compressed > view.getUint32(end + 16, true)) throw Error('ZIP-Datei ist abgeschnitten.');
    entries.push({name, method, size, checksum, compressed: bytes.subarray(start, start + compressed)});
  }
  if (position !== centralEnd || !entries.some(entry => entry.name === '.kobo/Kobo.tgz') || !entries.some(entry => entry.name === '.adds/kobalt/assets/manifest.json')) throw Error('Qt6-Installer oder Runtime-Manifest fehlt.');
  return entries;
}

export async function extract(entry) {
  let result;
  if (entry.method === 0) result = entry.compressed;
  else {
    const reader = new Blob([entry.compressed]).stream().pipeThrough(new DecompressionStream('deflate-raw')).getReader();
    const chunks = []; let size = 0;
    try {
      for (;;) {
        const chunk = await reader.read();
        if (chunk.done) break;
        size += chunk.value.length;
        if (size > entry.size) throw Error('Entpackte Datei überschreitet die angegebene Größe.');
        chunks.push(chunk.value);
      }
    } catch (error) { await reader.cancel().catch(() => {}); throw error; }
    result = new Uint8Array(size); let offset = 0;
    for (const chunk of chunks) { result.set(chunk, offset); offset += chunk.length; }
  }
  if (result.length !== entry.size || crc32(result) !== entry.checksum) throw Error('ZIP-Prüfsumme oder Dateigröße stimmt nicht.');
  return result;
}

export async function sha256(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), byte => byte.toString(16).padStart(2, '0')).join('');
}

export function firmwareFromVersion(text) {
  // Kobo's comma-separated version file records firmware as a dotted token.
  const firmware = text.split(/[\s,]+/).find(part => /^[56]\.\d+\.\d+(?:\.\d+)?$/.test(part));
  if (!firmware) throw Error('Keine unterstützte Firmware 5.x oder 6.x erkannt.');
  return firmware;
}
