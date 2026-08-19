// Mesure EN SITUATION REELLE : le solveur dans un vrai navigateur, isole
// cross-origin, avec les Web Workers que cela debloque.
//
//   node mesure.mjs [url] [timeout_s]
//
// Ne suppose rien : on lit ce que le binaire imprime — compteurs exacts
// (tirages, etats) et temps de mur — exactement comme pour le bras natif.

import { chromium } from 'playwright-core';
import { writeFileSync } from 'node:fs';

const url = process.argv[2] || 'http://127.0.0.1:8765/index.html';
const limite = Number(process.argv[3] || 300) * 1000;
const threads = process.argv[4] ? Number(process.argv[4]) : null;
const exe = process.env.LOCALAPPDATA +
  '\\ms-playwright\\chromium-1208\\chrome-win64\\chrome.exe';

const nav = await chromium.launch({
  executablePath: exe,
  args: ['--enable-features=SharedArrayBuffer'],
});
const page = await nav.newPage();
page.on('pageerror', e => console.error('ERREUR PAGE :', e.message));
page.on('console', m => { if (m.type() === 'error') console.error('console:', m.text()); });

const t0 = Date.now();
await page.goto(url, { waitUntil: 'load' });

const env = await page.evaluate(() => ({
  isole: self.crossOriginIsolated,
  sab: typeof SharedArrayBuffer !== 'undefined',
  coeurs: navigator.hardwareConcurrency,
  ua: navigator.userAgent.match(/Chrome\/[\d.]+/)?.[0],
}));
console.log('--- navigateur ---');
console.log(`  ${env.ua}`);
console.log(`  crossOriginIsolated : ${env.isole}`);
console.log(`  SharedArrayBuffer   : ${env.sab}`);
console.log(`  hardwareConcurrency : ${env.coeurs}`);
if (!env.isole) { console.error('PAS ISOLE : les threads ne demarreront pas'); }

if (threads) await page.selectOption('#th', String(threads));
await page.click('#go');
console.log('--- lance, attente ---');

try {
  await page.waitForFunction(() => window.__r2v !== undefined, null,
                             { timeout: limite, polling: 1000 });
} catch (e) {
  const d = await page.evaluate(() => window.__dump ? window.__dump() : '(rien)');
  writeFileSync('mesure_navigateur.log', d);
  console.error('--- PAS DE FIN ; ce que la page avait imprime ---');
  console.error(d.split(String.fromCharCode(10)).slice(-25).join(String.fromCharCode(10)));
  await nav.close();
  process.exit(1);
}
const r = await page.evaluate(() => window.__r2v);
console.log(`--- termine : code ${r.code}, ${r.threads} workers, ${r.seconds.toFixed(1)} s ` +
            `(dont ${(r.load / 1000).toFixed(2)} s de chargement) ---`);
writeFileSync('mesure_navigateur.log', r.texte);

for (const m of r.texte.split('\n')) {
  if (/^\s*(NRPA|glouton\+nouveaute|total :|workers|arene\s+:|engage|zone servie)/.test(m) ||
      /IDENTIQUE|DIVERGENCE|MSG_RETRY|hardwareConcurrency|crossOriginIsolated/.test(m))
    console.log(m);
}
console.log(`\n(rapport complet : mesure_navigateur.log, ${r.texte.length} octets)`);
console.log(`temps total du script : ${((Date.now() - t0) / 1000).toFixed(1)} s`);
await nav.close();
