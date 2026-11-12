const fs = require('node:fs');
const path = require('node:path');
const target = path.join(__dirname, 'ui');
fs.mkdirSync(target, {recursive: true});
for (const name of ['dashboard.html', 'dashboard.css', 'dashboard.js', 'preview.js']) {
  fs.copyFileSync(path.join(__dirname, '..', 'tools', name), path.join(target, name));
}
