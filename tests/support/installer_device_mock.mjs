export function installDeviceMock() {
    const encoder = new TextEncoder();
    window.deviceTest = {files:{'.kobo/version':encoder.encode('SERIAL,6.0.2,DEVICE'),'.adds/kobalt/assets/manifest.json':encoder.encode('old assets')},writes:[],removed:[],badReadback:false};
    const notFound = () => new DOMException('Not found','NotFoundError');
    function directory(prefix='') {
      return {
        name: prefix ? prefix.split('/').filter(Boolean).at(-1) : 'KOBOeReader',
        async getDirectoryHandle(name, options={}) {
          const path = prefix+name+'/';
          if (!options.create && !Object.keys(window.deviceTest.files).some(file=>file.startsWith(path))) throw notFound();
          return directory(path);
        },
        async getFileHandle(name, options={}) {
          const path=prefix+name;
          if (!(path in window.deviceTest.files)) {
            if (!options.create) throw notFound();
            window.deviceTest.files[path]=new Uint8Array();
          }
          return {
            async getFile() {
              const bytes=window.deviceTest.badReadback && path==='.kobo/Kobo.tgz' ? encoder.encode('corrupt') : window.deviceTest.files[path];
              return new File([bytes],name);
            },
            async createWritable() { let bytes;
              return {async write(value) { bytes=new Uint8Array(value); },async close() { window.deviceTest.files[path]=bytes; window.deviceTest.writes.push(path); },async abort(){}};
            }
          };
        },
        async removeEntry(name) {const path=prefix+name; if (!(path in window.deviceTest.files)) throw notFound();delete window.deviceTest.files[path];window.deviceTest.removed.push(path);}
      };
    }
    window.showDirectoryPicker=async()=>directory();
}
