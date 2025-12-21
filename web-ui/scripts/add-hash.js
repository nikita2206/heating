import { createHash } from 'crypto';
import { readFileSync, writeFileSync } from 'fs';
import { join } from 'path';

const distDir = 'dist';

console.log('Adding content hashes to asset filenames in HTML...');

// Calculate hash of a file
function getFileHash(filepath) {
  const content = readFileSync(filepath);
  return createHash('md5').update(content).digest('hex').substring(0, 8);
}

// Get hashes for assets
const jsPath = join(distDir, 'assets/index.js');
const cssPath = join(distDir, 'assets/index.css');

const jsHash = getFileHash(jsPath);
const cssHash = getFileHash(cssPath);

console.log(`  JS hash:  ${jsHash}`);
console.log(`  CSS hash: ${cssHash}`);

// Read and update HTML
const htmlPath = join(distDir, 'index.html');
let html = readFileSync(htmlPath, 'utf8');

// Replace asset references with hashed versions
// Note: files stay as index.js/index.css on disk, only HTML references change
html = html.replace(
  /src="\/assets\/index\.js"/g,
  `src="/assets/index_${jsHash}.js"`
);

html = html.replace(
  /href="\/assets\/index\.css"/g,
  `href="/assets/index_${cssHash}.css"`
);

writeFileSync(htmlPath, html);

console.log('Done! HTML now references hashed filenames.');

