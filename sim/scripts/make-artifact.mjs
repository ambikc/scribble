// Turn the production build (dist/) into ONE self-contained page for hosting as a claude.ai Artifact:
//   dist/artifact/page.html        title + styles + markup + the whole app inlined (no doctype/html/head/body: the host wraps it)
//   dist/artifact/standalone.html  the same page inside a normal HTML document, for testing it with any static server
//   dist/artifact/scribble-run-level-template-a4.pdf   the printable template the page links to
// Usage:  npm run build && node scripts/make-artifact.mjs
import { readFileSync, writeFileSync, mkdirSync, readdirSync, copyFileSync } from 'node:fs';

const dist = new URL('../dist/', import.meta.url).pathname, out = dist + 'artifact/';
mkdirSync(out, { recursive: true });
const html = readFileSync(dist + 'index.html', 'utf8');
const jsFile = readdirSync(dist + 'assets').find((f) => /^index-.*\.js$/.test(f));
if (!jsFile) throw new Error('no built index-*.js; run npm run build first');
const js = readFileSync(dist + 'assets/' + jsFile, 'utf8');
if (/\bimport\s*[("{]|import\.meta/.test(js.replace(/`[^`]*`|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'/g, ''))) console.warn('warning: the bundle may contain import statements; check it runs inline');

const pick = (re) => [...html.matchAll(re)].map((m) => m[0]);
const title = pick(/<title>[\s\S]*?<\/title>/g)[0];
const links = pick(/<link\b[^>]*\bhref="https:\/\/fonts\.[^>]*>/g);
const styles = pick(/<style>[\s\S]*?<\/style>/g);
const body = html.match(/<body>([\s\S]*)<\/body>/)[1].replace(/<script\b[^>]*><\/script>/g, '').trim();
const page = [title, ...links, ...styles, body, `<script type="module">\n${js.replace(/<\/script/gi, '<\\/script')}\n</script>`].join('\n');
writeFileSync(out + 'page.html', page);
writeFileSync(out + 'standalone.html', `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"></head><body>${page}</body></html>`);
copyFileSync(dist + 'scribble-run-level-template-a4.pdf', out + 'scribble-run-level-template-a4.pdf');
console.log(`page.html ${(page.length / 1024).toFixed(0)} KB`);
