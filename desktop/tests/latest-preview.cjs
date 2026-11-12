const {_electron:electron}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs/promises'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const app=await electron.launch({executablePath:process.env.ELECTRON_EXECUTABLE,args:[]});
 const errors=[];
 try{
  const page=await app.firstWindow();page.on('pageerror',e=>errors.push(e.message));
  await page.waitForLoadState('domcontentloaded');
  await page.evaluate(()=>localStorage.setItem('orbvi-preview-mode','latest'));
  await page.reload({waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>window.orbviVideoStats?.every(s=>s.mode==='latest'&&s.width===1280&&s.fps>30),undefined,{timeout:30000});
  const baseline=await page.evaluate(async()=>await(await fetch('/api/status')).json());
  const samples=[];
  for(let i=0;i<30;i++){
   await page.waitForTimeout(1000);samples.push(await page.evaluate(()=>structuredClone(window.orbviVideoStats)));
   if(i%10===9)console.log(JSON.stringify(samples.at(-1)));
  }
  await page.click('#pause');await page.waitForTimeout(300);
  const before=await page.evaluate(()=>window.orbviVideoStats.map(s=>s.frames));
  await page.waitForTimeout(3000);
  assert.deepEqual(await page.evaluate(()=>window.orbviVideoStats.map(s=>s.frames)),before);
  await page.click('#pause');
  await page.waitForFunction(before=>window.orbviVideoStats.every((s,i)=>s.frames>before[i]+30),before);
  const recoveryStart=await page.evaluate(()=>{const n=videoStats.map(s=>s.frames);livePreview.controller.abort();return n});
  await page.waitForFunction(before=>window.orbviVideoStats.every((s,i)=>s.reconnects>0&&s.frames>before[i]+30),recoveryStart,{timeout:15000});
  const exportPath=path.join(__dirname,'../../reports/latest-export.json');
  await app.evaluate(({dialog},file)=>{dialog.showSaveDialog=async()=>({canceled:false,filePath:file})},exportPath);
  await page.click('#export');await page.waitForFunction(()=>!document.querySelector('#export').disabled);
  assert(JSON.parse(await fs.readFile(exportPath,'utf8')).viewer.display.every(s=>s.mode==='latest'));
  await Promise.all([page.waitForEvent('domcontentloaded'),page.selectOption('#preview-mode','buffered')]);
  await page.waitForFunction(()=>[...document.querySelectorAll('video')].every(v=>v.videoWidth===1280&&!v.paused),undefined,{timeout:30000});
  await Promise.all([page.waitForEvent('domcontentloaded'),page.selectOption('#preview-mode','latest')]);
  await page.waitForFunction(()=>window.orbviVideoStats?.every(s=>s.mode==='latest'&&s.frames>30),undefined,{timeout:15000});
  await page.screenshot({path:path.join(__dirname,'../../reports/latest-preview.png'),fullPage:true});
  // Reload closes the old stream and decoders, then starts a fresh latest-frame stream.
  await Promise.all([page.waitForEvent('domcontentloaded'),page.click('#reconnect',{noWaitAfter:true})]);
  await page.waitForFunction(()=>window.orbviVideoStats?.every(s=>s.mode==='latest'&&s.frames>60),undefined,{timeout:30000});
  const status=await page.evaluate(async()=>await(await fetch('/api/status')).json());
  assert(status.diagnostics.level<2);assert.equal(errors.length,0,errors.join('\n'));
  for(const key of ['sequence_gaps','invalid','transport_drops','transport_invalid','image_callback_drops','imu_callback_drops','imu_pairing_errors'])
   assert.equal(status.diagnostics.values[key],baseline.diagnostics.values[key],key);
  assert(samples.every(row=>row.every(s=>s.decode_queue<=1&&s.buffer_ms===0&&s.reconnects===0)));
  const report={passed:true,samples,baseline,status,errors};
  await fs.writeFile(path.join(__dirname,'../../reports/latest-preview.json'),JSON.stringify(report,null,2));
  console.log('PASS latest-frame decode, bounded queue, pause/resume, stream recovery, native export, mode switching, reconnect and capture health');
 }finally{await app.close()}
})().catch(e=>{console.error(e);process.exitCode=1});
