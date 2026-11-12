/* Latest-frame USB preview: no media timeline, no deliberate playout buffer. */
class OrbviLatestPreview {
 constructor(canvases, stats, onFrame, onError) {
  this.canvases=canvases;this.stats=stats;this.onFrame=onFrame;this.onError=onError;
  this.contexts=canvases.map(c=>c.getContext('2d',{alpha:false,desynchronized:true}));
  this.paused=false;this.stopped=false;this.pending=[null,null,null,null];
  this.queued=[null,null,null,null];this.decoder=[];this.arrivals=Array.from({length:4},()=>new Map());
  this.lastRate=Array(4).fill(performance.now());this.rateFrames=[0,0,0,0];
  for(let i=0;i<4;i++)stats[i]={mode:'latest',fps:0,frames:0,dropped:0,width:0,height:0,
   buffer_ms:0,decode_queue:0,receive_to_draw_ms:null,reconnects:0};
 }
 pause(value){this.paused=value;this.lastRate.fill(performance.now());this.rateFrames.fill(0);if(value){this.clearPending();this.queued.fill(null)}}
 clearPending(){for(let i=0;i<4;i++){this.pending[i]?.frame.close();this.pending[i]=null}}
 stop(){this.stopped=true;this.controller?.abort();cancelAnimationFrame(this.raf);this.clearPending();for(const d of this.decoder)if(d.state!=='closed')d.close()}
 draw=()=>{
  const now=performance.now();
  for(let i=0;i<4;i++){
   const item=this.pending[i];if(!item)continue;this.pending[i]=null;
   try{
    if(this.paused)continue;
    const c=this.canvases[i],frame=item.frame,s=this.stats[i];
    if(c.width!==frame.displayWidth)c.width=frame.displayWidth;
    if(c.height!==frame.displayHeight)c.height=frame.displayHeight;
    this.contexts[i].drawImage(frame,0,0,c.width,c.height);
    s.frames++;this.rateFrames[i]++;s.width=c.width;s.height=c.height;
    s.receive_to_draw_ms=now-item.arrived;s.sensor_timestamp_us=String(frame.timestamp);
    s.decode_queue=this.decoder[i].decodeQueueSize;
    if(now-this.lastRate[i]>=1000){s.fps=this.rateFrames[i]*1000/(now-this.lastRate[i]);this.rateFrames[i]=0;this.lastRate[i]=now}
    this.onFrame(i,s);
   }finally{item.frame.close()}
  }
  if(!this.stopped)this.raf=requestAnimationFrame(this.draw);
 }
 configure(controller){
  this.clearPending();
  for(const d of this.decoder)if(d.state!=='closed')d.close();
  this.arrivals.forEach(m=>m.clear());this.queued.fill(null);
  this.decoder=this.canvases.map((_,i)=>{
   const decoder=new VideoDecoder({output:frame=>{
    const arrived=this.arrivals[i].get(frame.timestamp);this.arrivals[i].delete(frame.timestamp);
    if(this.paused||arrived===undefined){frame.close();return}
    if(this.pending[i]){this.pending[i].frame.close();this.stats[i].dropped++}
    this.pending[i]={frame,arrived};this.pump(i);
   },error:error=>{this.onError(error.message);controller.abort()}});
   decoder.addEventListener('dequeue',()=>this.pump(i));
   decoder.configure({codec:'avc1.64002A',optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});
   return decoder;
  });
 }
 accept(header,payload){
  if(header[5]!==0x10)return;
  if(payload.length<5)throw Error('Invalid camera record');
  const index=new DataView(payload.buffer,payload.byteOffset,4).getUint32(0,true);
  if(index>3)throw Error('Invalid camera index');
  if(this.paused)return;
  const data=payload.subarray(4),d=this.decoder[index],s=this.stats[index];
  // This BSP sends SPS/PPS/IDR on every frame. Never drop references silently
  // if a different encoder configuration is introduced in the future.
  let idr=false;
  for(let n=0;n+3<data.length;n++)if(data[n]===0&&data[n+1]===0&&data[n+2]===1&&(data[n+3]&31)===5){idr=true;break}
  if(!idr)throw Error('Latest-frame preview requires independent IDR pictures');
  if(this.queued[index])s.dropped++;
  this.queued[index]={header:header.slice(),data,arrived:performance.now()};
  this.pump(index);
 }
 pump(index){
  const item=this.queued[index],d=this.decoder[index];
  if(!item||this.paused||!d||d.state!=='configured'||d.decodeQueueSize>=1||this.arrivals[index].size>=2)return;
  const {header,data,arrived}=item,s=this.stats[index];this.queued[index]=null;
  const h=new DataView(header.buffer,header.byteOffset,24);
  const timestamp=Number(h.getBigUint64(16,true)/1000n);
  this.arrivals[index].set(timestamp,arrived);
  d.decode(new EncodedVideoChunk({type:'key',timestamp,data}));
  s.decode_queue=d.decodeQueueSize;
 }
 async start(){
  this.raf=requestAnimationFrame(this.draw);
  while(!this.stopped){
   const controller=new AbortController();this.controller=controller;let reader;
   this.configure(controller);let lastBytes=performance.now();
   const watchdog=setInterval(()=>{const now=performance.now();if(now-lastBytes>5000||this.arrivals.some(m=>[...m.values()].some(t=>now-t>2000)))controller.abort()},500);
   try{
    const response=await fetch('/video/live.x5us',{signal:controller.signal});
    if(!response.ok)throw Error('Latest-frame stream unavailable');
    reader=response.body.getReader();let pending=new Uint8Array(0);
    while(!this.stopped){
     const {value,done}=await reader.read();if(done)throw Error('Preview stream ended');lastBytes=performance.now();
     const joined=new Uint8Array(pending.length+value.length);joined.set(pending);joined.set(value,pending.length);
     let offset=0;
     while(joined.length-offset>=24){
      const h=joined.subarray(offset,offset+24);
      if(h[0]!==88||h[1]!==53||h[2]!==85||h[3]!==83||h[4]!==1||h[6]||h[7])throw Error('Invalid preview framing');
      const size=new DataView(h.buffer,h.byteOffset,24).getUint32(8,true);
      if(size===0||size>4*1024*1024)throw Error('Invalid preview record size');
      if(joined.length-offset<24+size)break;
      this.accept(h,joined.subarray(offset+24,offset+24+size));offset+=24+size;
     }
     pending=joined.slice(offset);
    }
   }catch(error){if(!this.stopped){this.stats.forEach(s=>s.reconnects++);this.onError(error.message)}}
   finally{clearInterval(watchdog);controller.abort();if(reader)await reader.cancel().catch(()=>{});this.clearPending();for(const d of this.decoder)if(d.state!=='closed')d.close()}
   if(!this.stopped)await new Promise(resolve=>setTimeout(resolve,500));
  }
 }
}
window.OrbviLatestPreview=OrbviLatestPreview;
