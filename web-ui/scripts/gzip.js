import { gzipSync } from 'zlib';
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'fs';
import { join, dirname } from 'path';

const distDir = 'dist';
const files = ['index.html', 'assets/index.js', 'assets/index.css'];

console.log('Gzipping build output...');

files.forEach(file => {
  const srcPath = join(distDir, file);
  const destPath = srcPath + '.gz';

  if (!existsSync(srcPath)) {
    console.warn(`Warning: ${srcPath} not found, skipping`);
    return;
  }

  const content = readFileSync(srcPath);
  const compressed = gzipSync(content, { level: 9 });

  writeFileSync(destPath, compressed);

  const ratio = ((1 - compressed.length / content.length) * 100).toFixed(1);
  console.log(`  ${file}: ${content.length} -> ${compressed.length} bytes (${ratio}% reduction)`);
});

console.log('Done!');
