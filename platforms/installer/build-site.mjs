import {readFile, mkdir, copyFile, writeFile, stat, rename, rm, cp} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const options = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
  if (!['--package', '--output'].includes(process.argv[i]) || !process.argv[i+1]) throw Error('Usage: node build-site.mjs --package Qt6.zip --output directory');
  options.set(process.argv[i], process.argv[i+1]);
}
if (!options.has('--package') || !options.has('--output')) throw Error('Both --package and --output are required.');
const packagePath = resolve(options.get('--package'));
const output = resolve(options.get('--output'));
if (output === root || output === '/' || packagePath.startsWith(output + '/')) throw Error('Unsafe site output directory.');
const bytes = await readFile(packagePath);
const hash = createHash('sha256').update(bytes).digest('hex');
const expected = (await readFile(packagePath + '.sha256','utf8')).trim().split(/\s+/)[0];
if (hash !== expected) throw Error('Qt6 package does not match its checksum sidecar.');
const temporary = output + '.staging-' + process.pid;
await mkdir(temporary,{recursive:true});
try {
  for (const file of ['index.html','app.js','zip.js','style.css','locale.js']) await copyFile(join(root,'platforms/installer',file),join(temporary,file));
  await cp(join(root,'assets/translations'),join(temporary,'locales'),{recursive:true});
  await copyFile(join(root,'assets/roreader-icon.png'),join(temporary,'logo.png'));
  await copyFile(packagePath,join(temporary,'package.zip'));
  await writeFile(join(temporary,'package.sha256'),hash+'  package.zip\n');
  await writeFile(join(temporary,'.nojekyll'),'');
  await writeFile(join(temporary,'site-manifest.json'),JSON.stringify({generator:'roreader-pages',package_sha256:hash,package_bytes:bytes.length},null,2)+'\n');
  try {
    const previous=JSON.parse(await readFile(join(output,'site-manifest.json'),'utf8'));
    if(previous.generator!=='roreader-pages')throw Error('Refusing to replace an unknown directory.');
    await rm(output,{recursive:true});
  } catch(error) { if(error.code!=='ENOENT')throw error; }
  await mkdir(dirname(output),{recursive:true});
  await rename(temporary,output);
} catch(error) {await rm(temporary,{recursive:true,force:true});throw error;}
console.log(`ROREADER Pages ready: ${output} (${(await stat(join(output,'package.zip'))).size} package bytes)`);
