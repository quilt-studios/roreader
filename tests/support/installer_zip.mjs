import {deflateRawSync} from 'node:zlib';
import {crc32} from '../../platforms/installer/zip.js';

export function zip(files, method = 0) {
  const locals = [], central = []; let offset = 0;
  for (const [path, content] of files) {
    const name = Buffer.from(path), bytes = Buffer.from(content);
    const compressed = method === 8 ? deflateRawSync(bytes) : bytes;
    const header = Buffer.alloc(30);
    header.writeUInt32LE(0x04034b50); header.writeUInt16LE(method, 8);
    header.writeUInt32LE(crc32(bytes), 14); header.writeUInt32LE(compressed.length, 18);
    header.writeUInt32LE(bytes.length, 22); header.writeUInt16LE(name.length, 26);
    const entry = Buffer.alloc(46);
    entry.writeUInt32LE(0x02014b50); entry.writeUInt16LE(method, 10);
    entry.writeUInt32LE(crc32(bytes), 16); entry.writeUInt32LE(compressed.length, 20);
    entry.writeUInt32LE(bytes.length, 24); entry.writeUInt16LE(name.length, 28);
    entry.writeUInt32LE(offset, 42);
    locals.push(header, name, compressed); central.push(entry, name);
    offset += header.length + name.length + compressed.length;
  }
  const directory = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(files.length, 8); end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16);
  const all = Buffer.concat([...locals, directory, end]);
  return all.buffer.slice(all.byteOffset, all.byteOffset + all.length);
}
