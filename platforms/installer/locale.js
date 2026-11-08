const supported = await (await fetch(new URL('locales/languages.json', import.meta.url))).json();
function normalize(value) {
  const code = String(value || '').toLowerCase().replace('_', '-').split('-')[0];
  return ({nb:'no',nn:'no',kmr:'ku'})[code] || code;
}
let saved;
try { saved = localStorage.getItem('roreader-language'); } catch {}
const requested = normalize(new URL(location.href).searchParams.get('lang') || saved || navigator.language);
export const language = supported[requested] ? requested : 'en';
const load = async code => {
  const response = await fetch(new URL(`locales/${code}.json`, import.meta.url));
  if (!response.ok) throw Error(`Language catalog unavailable: ${code}`);
  return (await response.json()).web;
};
const english = await load('en');
const catalog = language === 'en' ? english : await load(language).catch(() => english);
export function t(source, ...args) {
  const text = catalog[source] || english[source] || source;
  return text.replace(/%([1-9][0-9]*)/g, (slot, index) => index <= args.length ? String(args[index-1]) : slot);
}
export function tr(parts, ...args) {
  return t(parts.reduce((result, part, index) => result + (index ? `%${index}` : '') + part, ''), ...args);
}
document.documentElement.lang = language;
const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
const nodes = [];
while (walker.nextNode()) {
  const node = walker.currentNode;
  if (!['SCRIPT','STYLE','CODE'].includes(node.parentElement?.tagName) && node.textContent.trim()) nodes.push(node);
}
for (const node of nodes) {
  const source = node.textContent.trim();
  node.textContent = node.textContent.replace(source, t(source));
}
for (const element of document.querySelectorAll('[aria-label],[placeholder],[title]'))
  for (const name of ['aria-label','placeholder','title'])
    if (element.hasAttribute(name)) element.setAttribute(name, t(element.getAttribute(name)));
const selector = document.getElementById('language');
for (const [code,name] of Object.entries(supported)) selector.add(new Option(name,code));
selector.value = language;
selector.setAttribute('aria-label',t('Sprache'));
selector.onchange = () => {
  try { localStorage.setItem('roreader-language',selector.value); } catch {}
  const url = new URL(location.href);
  url.searchParams.set('lang',selector.value);
  location.replace(url);
};
