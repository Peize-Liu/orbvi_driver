
const $=id=>document.getElementById(id);let paused=false,latest=null,busy=false,connected=false;
for(let i=0;i<4;i++){$('cameras').insertAdjacentHTML('beforeend',`<article class="camera"><div class="camhead"><span>CAM 0${i}</span><span class="fps" id="fps${i}">等待数据</span></div><div class="picture" id="pic${i}"><span class="waiting">等待相机图像</span><video id="video${i}" muted playsinline></video><span class="overlay" id="size${i}">RGB · 1280 × 1088</span><div class="stale" id="stale${i}">数据已中断 · 保留最后一帧</div></div><div class="camfoot"><span id="seq${i}">序号 —</span><span id="gap${i}">缺口 —</span></div></article>`);$('pic'+i).onclick=()=>{$('video'+i).requestFullscreen?.()};}

function chart(id,history,key,minRange){const c=$(id),dpr=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;c.width=w*dpr;c.height=h*dpr;const ctx=c.getContext('2d');ctx.scale(dpr,dpr);const pad={l:42,r:8,t:10,b:22},pw=w-pad.l-pad.r,ph=h-pad.t-pad.b;const end=history.length?history.at(-1).t:10,start=end-10;let bound=minRange;history.forEach(p=>p[key].forEach(v=>bound=Math.max(bound,Math.abs(v)*1.12)));ctx.font='10px monospace';ctx.lineWidth=1;for(let j=0;j<=4;j++){const y=pad.t+ph*j/4;ctx.strokeStyle='#273344';ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(w-pad.r,y);ctx.stroke();ctx.fillStyle='#8092a9';ctx.fillText((bound*(1-j/2)).toFixed(bound<1?2:1),2,y+3)}ctx.fillText('−10s',pad.l,h-4);ctx.fillText('现在',w-34,h-4);['#79e4b9','#7faeff','#f2b381'].forEach((color,axis)=>{ctx.strokeStyle=color;ctx.lineWidth=1.5;ctx.beginPath();let first=true;for(const p of history){if(p.t<start)continue;const x=pad.l+(p.t-start)/10*pw,y=pad.t+ph/2-p[key][axis]/bound*ph/2;if(first){ctx.moveTo(x,y);first=false}else ctx.lineTo(x,y)}ctx.stroke()});}
function render(s){if(s.configuration){const c=s.configuration;$('data-link').textContent=(c.transport.mode==='usb'?'USB Bulk':'ADB 转发')+' → ROS 2';$('config-size').textContent=c.camera.width+' × '+c.camera.height+' · RGB';$('config-fps').textContent='四路均值 · 目标 '+c.camera.fps+' Hz';$('config-imu').textContent='标称 '+c.imu.frequency_hz+' Hz · ±'+c.imu.accel_range_g+' g / ±'+c.imu.gyro_range_dps+' °/s';$('config-trigger').textContent='GPIO417 · '+c.camera.fps+' Hz';$('foot').textContent=c.ros.topic_prefix+'/camera_0…3/image · '+c.ros.topic_prefix+'/imu';}const active=s.cameras.filter(c=>c.active).length,good=active===4&&s.imu.active&&s.diagnostics?.level===0&&s.diagnostics.age_ms<2500;$('connection').className='tag'+(good?' good':'');$('connection').innerHTML='<span class="dot"></span>'+(paused?'显示已暂停 · 采集继续':good?'采集正常 · 实时连接':'等待数据 / 请检查诊断');$('online').textContent=active;$('fps').textContent=active?(s.cameras.filter(c=>c.active).reduce((n,c)=>n+c.rate,0)/active).toFixed(1):'—';$('imu-rate').textContent=s.imu.active?s.imu.rate.toFixed(1):'—';const gaps=s.cameras.reduce((n,c)=>n+c.gaps,0)+s.imu.gaps;$('gaps').textContent=gaps;$('gaps').className='value'+(gaps?' error':'');s.cameras.forEach((c,i)=>{$('fps'+i).textContent=c.active?c.rate.toFixed(1)+' Hz':'离线';$('seq'+i).textContent='序号 '+(c.sequence??'—');$('gap'+i).textContent='缺口 '+c.gaps+' · 缓冲 '+(videoStats[i].buffer_ms??'—')+' ms · 等待 '+(videoStats[i].underruns??0);if(c.size)$('size'+i).textContent=c.size[0]+' × '+c.size[1]+' · '+c.size[2].toUpperCase();});$('sync-skew').textContent=s.sync?.groups?s.sync.max_skew_us.toFixed(1)+' µs':'—';$('imu-seq').textContent=s.imu.sequence??'—';$('imu-stamp').textContent=s.imu.stamp_ns;$('age').textContent=s.imu.age_ms===null?'—':s.imu.age_ms.toFixed(0);$('resets').textContent=s.imu.resets+s.cameras.reduce((n,c)=>n+c.resets,0);$('uptime').textContent=Math.floor(s.uptime/60)+'m '+Math.floor(s.uptime%60)+'s';renderDiagnostics(s);if(!paused){chart('accel',s.history,'accel',12);chart('gyro',s.history,'gyro',.05);}if(!paused&&s.latest_imu)for(const key of ['accel','gyro'])$(key+'-values').innerHTML=s.latest_imu[key].map((v,i)=>`<span class="${'xyz'[i]}">${'XYZ'[i]} ${v.toFixed(4)}</span>`).join('');}
function renderDiagnostics(s) {
 const d=s.diagnostics, values=d?.values;
 $('diagnostic-state').textContent=!d?'暂无诊断':d.age_ms>2500?'诊断已过期':(['正常','存在异常计数','驱动错误'][d.level]||'未知状态');
 $('diagnostic-state').className=d&&d.level===0&&d.age_ms<2500?'good':'error';
 $('diagnostic-state').title=d?.message||'';
 $('transport-errors').textContent=values?`${values.transport_drops} / ${values.transport_invalid}`:'—';
 $('callback-drops').textContent=values?`${values.image_callback_drops} / ${values.imu_callback_drops}`:'—';
 $('pair-errors').textContent=values?.imu_pairing_errors??'—';
}
async function poll(){
 if(busy)return;busy=true;
 try{
  const r=await fetch('/api/status',{signal:AbortSignal.timeout(2500)});
  if(!r.ok)throw Error('status unavailable');
  latest=await r.json();render(latest);connected=true;$('service-help').hidden=true;
 }catch(e){
  connected=false;$('service-help').hidden=false;
  $('connection').className='tag error';$('connection').innerHTML='<span class="dot"></span>本机采集服务未连接';
  for(let i=0;i<4;i++)$('fps'+i).textContent='连接中断';
  for(const id of ['online','fps','imu-rate','transport-errors','callback-drops','pair-errors','sync-skew','age'])$(id).textContent='—';
  $('diagnostic-state').textContent='诊断不可用';$('diagnostic-state').className='error';
 }finally{busy=false}
}
const videoStats=[{},{},{},{}], players=[];
const LIVE_BUFFER=.30;
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
function refreshOverlays(){
 for(let i=0;i<4;i++){
  const player=players[i],video=$('video'+i),fresh=player&&performance.now()-player.lastFrame<2000;
  if(player){
   Object.assign(videoStats[i],{buffer_ms:video.buffered.length?Math.max(0,Math.round(1000*(video.buffered.end(video.buffered.length-1)-video.currentTime))):0,
    underruns:player.underruns,catchup_seeks:player.catchupSeeks,frame_age_ms:Math.round(performance.now()-player.lastFrame)});
   if(!fresh&&!paused)videoStats[i].fps=0;
  }
  const reason=paused?'显示已暂停 · 采集继续':!connected?'采集状态连接中断':!latest?.cameras[i].active?'相机数据已中断':!fresh?'视频等待 / 已中断 · 自动重连':'';
  $('stale'+i).style.display=reason?'flex':'none';$('stale'+i).textContent=reason;
 }
}
async function videoLoop(i){
 const video=$('video'+i),player={lastFrame:0,controller:null,underruns:0,catchupSeeks:0,lastSeek:-Infinity,started:false};players[i]=player;video.muted=true;
 let last=performance.now(),lastFrames=0;
 function counted(now,meta){
  player.lastFrame=now;
  const elapsed=(now-last)/1000;
  if(elapsed>=1){
   videoStats[i]={fps:Math.max(0,meta.presentedFrames-lastFrames)/elapsed,frames:meta.presentedFrames,
    dropped:video.getVideoPlaybackQuality().droppedVideoFrames,width:video.videoWidth,height:video.videoHeight,
    buffer_ms:video.buffered.length?Math.round(1000*(video.buffered.end(video.buffered.length-1)-video.currentTime)):null,
    underruns:player.underruns,catchup_seeks:player.catchupSeeks,playback_rate:video.playbackRate};
   last=now;lastFrames=meta.presentedFrames;
   $('size'+i).textContent=`RGB · ${video.videoWidth} × ${video.videoHeight} · 显示 ${videoStats[i].fps.toFixed(1)} fps · 显示丢帧 ${videoStats[i].dropped}`;
  }
  video.requestVideoFrameCallback(counted);
 }
 if(video.requestVideoFrameCallback)video.requestVideoFrameCallback(counted);
 video.addEventListener('playing',()=>{player.started=true;$('pic'+i).classList.add('loaded')});
 video.addEventListener('waiting',()=>{if(player.started&&!paused&&!video.seeking)player.underruns++});
 const codec='video/mp4; codecs="avc1.64002A"';
 if(!window.MediaSource||!MediaSource.isTypeSupported(codec)){
  $('pic'+i).querySelector('.waiting').textContent='当前环境不支持 H.264 解码';return;
 }
 while(true){
  const controller=new AbortController();player.controller=controller;
  const media=new MediaSource(),url=URL.createObjectURL(media);let reader=null;
  let lastBytes=performance.now();
  const watchdog=setInterval(()=>{if(performance.now()-lastBytes>8000)controller.abort()},1000);
  try{
   await new Promise((resolve,reject)=>{
    media.addEventListener('sourceopen',resolve,{once:true});
    controller.signal.addEventListener('abort',()=>reject(Error('video timeout')),{once:true});
    video.src=url;
   });
   const source=media.addSourceBuffer(codec);
   const update=operation=>new Promise((resolve,reject)=>{
    const cleanup=()=>{source.removeEventListener('updateend',done);source.removeEventListener('error',failed);controller.signal.removeEventListener('abort',failed)};
    const done=()=>{cleanup();resolve()},failed=()=>{cleanup();reject(Error('video buffer interrupted'))};
    source.addEventListener('updateend',done,{once:true});source.addEventListener('error',failed,{once:true});
    controller.signal.addEventListener('abort',failed,{once:true});
    try{operation()}catch(error){cleanup();reject(error)}
   });
   const response=await fetch('/video/'+i+'.mp4',{signal:controller.signal});
   if(!response.ok)throw Error('video unavailable');
   reader=response.body.getReader();lastFrames=0;last=performance.now();player.started=false;video.playbackRate=1;
   while(true){
    const {value,done}=await reader.read();if(done)throw Error('video ended');lastBytes=performance.now();
    await update(()=>source.appendBuffer(value));
    if(source.buffered.length){
     const start=source.buffered.start(0),end=source.buffered.end(source.buffered.length-1);
     if(!paused){
      const ahead=end-video.currentTime,now=performance.now();
      // Absorb ordinary USB/Docker scheduling jitter without repeatedly seeking.
      // Seek only to recover a substantial backlog (e.g. after backgrounding).
      if(ahead>1&&!video.seeking&&now-player.lastSeek>2000){
       video.currentTime=Math.max(start,end-LIVE_BUFFER);player.lastSeek=now;player.catchupSeeks++;
       video.playbackRate=1;
      }else if(!video.seeking){
       const rate=ahead>.5?1.02:1;
       if(video.playbackRate!==rate)video.playbackRate=rate;
      }
      if(video.paused&&ahead>=LIVE_BUFFER)video.play().catch(()=>{});
     }
     // Retain a bounded live buffer even when display is paused.
     if(end-start>12)await update(()=>source.remove(start,end-5));
    }
   }
  }catch(error){ /* The visible watchdog reports the stale stream; retry below. */ }
  finally{
   clearInterval(watchdog);controller.abort();
   if(reader)await reader.cancel().catch(()=>{});
   video.removeAttribute('src');video.load();URL.revokeObjectURL(url);
  }
  await delay(2000);
 }
}
window.orbviVideoStats=videoStats;
$('pause').onclick=()=>{
 paused=!paused;
 for(let i=0;i<4;i++){
  const v=$('video'+i);
  if(paused)v.pause();else {if(v.buffered.length)v.currentTime=Math.max(v.buffered.start(0),v.buffered.end(v.buffered.length-1)-LIVE_BUFFER);v.playbackRate=1;v.play().catch(()=>{})}
 }
 $('pause').textContent=paused?'恢复显示':'暂停显示';refreshOverlays();if(latest)render(latest);
};
$('reconnect').onclick=()=>location.reload();
$('export').onclick=async()=>{
 if(!latest)return;
 const snapshot={...latest,viewer:{exported_at:new Date().toISOString(),connected,paused,
  display:videoStats,note:'Display statistics are independent of capture. IMU history is decimated.'}};
 const json=JSON.stringify(snapshot,null,2);
 try{
  if(window.orbviDesktop){
   $('export').disabled=true;await window.orbviDesktop.exportStatus(json);
  }else{
   const url=URL.createObjectURL(new Blob([json],{type:'application/json'})),a=document.createElement('a');
   a.href=url;a.download='orbvi-status-'+Date.now()+'.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
  }
 }catch(e){$('foot').textContent='导出失败：'+e.message}
 finally{$('export').disabled=false}
};
setInterval(poll,200);setInterval(refreshOverlays,250);poll();window.addEventListener('load',()=>setTimeout(()=>{for(let i=0;i<4;i++)videoLoop(i)},0),{once:true});

if(window.orbviDesktop){document.body.classList.add('desktop');document.title='ORBVI Viewer';document.querySelector('h1').textContent='ORBVI Viewer';}
