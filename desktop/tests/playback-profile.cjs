const {_electron:electron}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs/promises');
const path=require('node:path');
const http=require('node:http');
(async()=>{
 const sockets=new Set(),jitter=Number(process.env.JITTER_MS||0);
 const proxy=http.createServer((req,res)=>{
  const upstream=http.get('http://127.0.0.1:18765'+req.url,incoming=>{
   res.writeHead(incoming.statusCode,incoming.headers);incoming.pipe(res);
   let timer,restart;
   if(jitter&&req.url.startsWith('/video/'))timer=setInterval(()=>{
    incoming.pause();restart=setTimeout(()=>incoming.resume(),jitter);
   },3000);
   res.on('close',()=>{clearInterval(timer);clearTimeout(restart);upstream.destroy()});
   incoming.on('error',()=>res.destroy());
  });
  upstream.on('error',()=>res.destroy());
 });
 proxy.on('connection',s=>{sockets.add(s);s.on('close',()=>sockets.delete(s))});
 await new Promise(resolve=>proxy.listen(0,'127.0.0.1',resolve));
 const app=await electron.launch({executablePath:process.env.ELECTRON_EXECUTABLE,args:[],
  env:{...process.env,ORBVI_DASHBOARD_URL:`http://127.0.0.1:${proxy.address().port}`}});
 try{
 const page=await app.firstWindow();await page.waitForLoadState('domcontentloaded');await page.evaluate(()=>localStorage.setItem('orbvi-preview-mode','buffered'));await page.reload({waitUntil:'domcontentloaded'});
 await page.waitForFunction(()=>window.orbviVideoStats?.every(x=>x.width>0),undefined,{timeout:45000});
 await page.waitForTimeout(5000);
 const start=await page.evaluate(async()=>await(await fetch('/api/status')).json());
 await page.evaluate(()=>{
  window.profile={started:performance.now(),streams:[]};
  for(let i=0;i<4;i++){
   const video=document.querySelector('#video'+i),p={frames:0,gaps100:0,gaps250:0,gaps500:0,maxGap:0,maxSilence:0,lastFrameAt:performance.now(),seeks:0,waits:0,headroom:[],events:[]};
   profile.streams.push(p);let last=performance.now();
   function frame(now){const gap=now-last;last=now;p.lastFrameAt=performance.now();p.frames++;p.maxGap=Math.max(p.maxGap,gap);for(const n of [100,250,500])if(gap>n)p['gaps'+n]++;video.requestVideoFrameCallback(frame)}
   video.requestVideoFrameCallback(frame);
   for(const event of ['seeking','waiting','stalled','playing','seeked'])video.addEventListener(event,()=>{if(event==='seeking')p.seeks++;if(event==='waiting')p.waits++;p.events.push({event,t:performance.now()-profile.started,current:video.currentTime,headroom:video.buffered.length?video.buffered.end(video.buffered.length-1)-video.currentTime:null})});
   setInterval(()=>{p.maxSilence=Math.max(p.maxSilence,performance.now()-p.lastFrameAt);if(video.buffered.length)p.headroom.push(video.buffered.end(video.buffered.length-1)-video.currentTime)},200);
  }
 });
 for(let i=0;i<Number(process.env.PROFILE_SECONDS||60)/10;i++){
  await page.waitForTimeout(10000);
  console.log(JSON.stringify(await page.evaluate(()=>profile.streams.map(p=>({frames:p.frames,seeks:p.seeks,waits:p.waits,gaps250:p.gaps250,maxGap:p.maxGap})))));
 }
 const result=await page.evaluate(async()=>({profile,display:window.orbviVideoStats,end:await(await fetch('/api/status')).json()}));
 result.start=start;result.jitter_ms=jitter;result.capture_healthy=result.end.diagnostics?.level===0&&result.end.cameras.every(c=>c.active)&&result.end.imu.active;await fs.writeFile(process.env.PROFILE_OUTPUT||path.join(__dirname,'../../reports/playback-profile.json'),JSON.stringify(result,null,2));
 if(!result.capture_healthy)throw Error('Capture became unhealthy; inspect the saved profile diagnostics');
 }finally{await app.close();for(const socket of sockets)socket.destroy();await new Promise(resolve=>proxy.close(resolve))}
})().catch(error=>{console.error(error);process.exitCode=1});
