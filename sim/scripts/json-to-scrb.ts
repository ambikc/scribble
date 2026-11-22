// Turn a level saved by the hosted simulator (.json, the JSON twin) into the device's .scrb file.
// Usage:  npx tsx scripts/json-to-scrb.ts my-level.json [out.scrb]
import { readFileSync, writeFileSync } from 'node:fs';
import { levelFromJSON, encodeLevel } from '../src/level';

const [inp, out] = process.argv.slice(2);
if (!inp) { console.error('usage: npx tsx scripts/json-to-scrb.ts level.json [out.scrb]'); process.exit(1); }
const bytes = encodeLevel(levelFromJSON(readFileSync(inp, 'utf8')), true);
const dest = out ?? inp.replace(/\.json$/i, '') + '.scrb';
writeFileSync(dest, bytes);
console.log(`wrote ${dest} (${bytes.length} bytes)`);
