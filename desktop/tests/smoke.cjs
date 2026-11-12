// Run against a live local dashboard; only this app's proxy is interrupted.
// PLAYWRIGHT_MODULE may point to a separately installed Playwright package.
const {_electron: electron} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const http = require('node:http');
const fs = require('node:fs/promises');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '../..');
const out = path.join(root, 'reports');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
(async () => {
 await fs.mkdir(out, {recursive: true});
 let offline = false;
 const connections = new Set();
 const proxy = http.createServer((request, response) => {
  if (offline) {response.writeHead(503);response.end();return;}
  const upstream = http.get('http://127.0.0.1:18765' + request.url, incoming => {
   response.writeHead(incoming.statusCode, incoming.headers);incoming.pipe(response);
   incoming.on('error', () => response.destroy());
  });
  upstream.on('error', () => {if (!response.headersSent) response.writeHead(503);response.end();});
  response.on('close', () => upstream.destroy());
 });
 proxy.on('connection', socket => {connections.add(socket);socket.on('close', () => connections.delete(socket));});
 await new Promise(resolve => proxy.listen(0, '127.0.0.1', resolve));
 let app;
 const errors=[];
 try {
  app = await electron.launch({
   executablePath: process.env.ELECTRON_EXECUTABLE || require('../node_modules/electron'),
   args: process.env.ELECTRON_EXECUTABLE ? [] : [path.join(root, 'desktop')],
   env: {...process.env, ORBVI_DASHBOARD_URL: `http://127.0.0.1:${proxy.address().port}`}
  });
  const page=await app.firstWindow();page.on('pageerror', error => errors.push(error.message));
  await page.waitForFunction(() => document.querySelector('#online')?.textContent==='4', undefined, {timeout: 30000});
  await page.waitForFunction(() => window.orbviVideoStats?.every(v=>v.width===1280&&v.height===1088&&v.fps>30), undefined, {timeout: 45000});
  await delay(12000);
  const capture=()=>page.evaluate(async()=>({status:await(await fetch('/api/status')).json(),
   display:window.orbviVideoStats,connection:document.querySelector('#connection').textContent,
   security:{node:typeof require,desktop:typeof window.orbviDesktop?.exportStatus}}));
  const live=await capture();
  assert.equal(live.status.diagnostics.level,0);
  assert(live.status.cameras.every(c=>c.active&&c.rate>58&&c.rate<62));
  assert(live.status.imu.rate>980&&live.status.imu.rate<1030);
  assert.equal(live.security.node,'undefined');assert.equal(live.security.desktop,'function');
  await page.screenshot({path:path.join(out,'electron-live.png'),fullPage:true});
  await page.click('#pause');await delay(15000);
  const paused=await page.evaluate(()=>[...document.querySelectorAll('video')].map(v=>({paused:v.paused,
   span:v.buffered.length?v.buffered.end(v.buffered.length-1)-v.buffered.start(0):0})));
  assert(paused.every(v=>v.paused&&v.span<14));
  await page.click('#pause');await delay(4000);
  assert(await page.evaluate(()=>[...document.querySelectorAll('video')].every(v=>!v.paused&&v.readyState>=2)));
  const exported=path.join(out,'electron-export.json');
  await app.evaluate(({dialog}, file)=>{dialog.showSaveDialog=async()=>({canceled:false,filePath:file})}, exported);
  await page.click('#export');
  await page.waitForFunction(()=>!document.querySelector('#export').disabled);
  const saved=JSON.parse(await fs.readFile(exported,'utf8'));assert.equal(saved.viewer.display.length,4);
  offline=true;for(const socket of connections)socket.destroy();
  await page.waitForFunction(()=>document.querySelector('#connection').textContent.includes('未连接'));
  assert.equal(await page.locator('#diagnostic-state').innerText(),'诊断不可用');
  await page.screenshot({path:path.join(out,'electron-offline.png'),fullPage:true});
  offline=false;
  await page.waitForFunction(()=>document.querySelector('#connection').textContent.includes('采集正常'),undefined,{timeout:30000});
  await page.waitForFunction(()=>[...document.querySelectorAll('video')].every(v=>v.readyState>=2&&!v.paused),undefined,{timeout:30000});
  await Promise.all([page.waitForEvent('domcontentloaded'),page.click('#reconnect', {noWaitAfter:true})]);
  await page.waitForFunction(()=>window.orbviVideoStats?.every(v=>v.width===1280&&v.fps>30),undefined,{timeout:45000});
  const recovery=await capture();assert.equal(errors.length,0,errors.join('\n'));
  const report={passed:true,live,paused,recovery,export_checked:true,offline_recovery_checked:true,errors};
  await fs.writeFile(path.join(out,'electron-smoke.json'),JSON.stringify(report,null,2));
  console.log(JSON.stringify({passed:true,cameras:live.status.cameras.map(c=>c.rate),imu:live.status.imu.rate,
   display:live.display,diagnostics:live.status.diagnostics,sync:live.status.sync}));
 } finally {
  if(app)await app.close();for(const socket of connections)socket.destroy();await new Promise(resolve=>proxy.close(resolve));
 }
})().catch(error=>{console.error(error);process.exitCode=1});
