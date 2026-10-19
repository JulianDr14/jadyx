// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
const fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const header=fs.readFileSync('src/jadyx/storage/network_portal_page.h','utf8');
const script=header.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script,{filename:'network-portal-page.js'});
const sha=script.match(/function sha256\(input\)\{[\s\S]*?\n\}/)[0];
const fingerprint=script.match(/async function fingerprint\(file\)\{[\s\S]*?\n\}/)[0];
const functions=vm.runInNewContext(sha+'\n'+fingerprint+'\n({sha256,fingerprint})',{TextEncoder});
for(const size of [0,3,55,56,63,64,65,128,131072]){
 const bytes=crypto.randomBytes(size);
 assert.equal(functions.sha256(bytes),crypto.createHash('sha256').update(bytes).digest('hex'));
}
(async()=>{
 const content=crypto.randomBytes(250000),blob=new Blob([content]);
 const file={name:'prueba á.nsp',size:blob.size,lastModified:123,slice:(a,b)=>blob.slice(a,b)};
 const identity=Buffer.from(JSON.stringify([file.name,file.size,file.lastModified]));
 const expected=crypto.createHash('sha256').update(identity).update(content.subarray(0,65536)).update(content.subarray(content.length-65536)).digest('hex');
 assert.equal(await functions.fingerprint(file),expected);
 assert.notEqual(await functions.fingerprint({...file,lastModified:124}),expected);
 console.log('PASS: page syntax, SHA-256 boundary vectors and bounded resume fingerprint (HTTP without WebCrypto)');
 for(const size of [4,16,64])await browserScenario({chunk:size});
 await browserScenario({lostAck:true});
 await browserScenario({resume:true});
 await browserScenario({different:true});
 await browserScenario({collision:true});
 await browserScenario({pause:true});
 await browserScenario({queue:true});
 console.log('PASS: browser client mocked 5GiB queue/chunks4-16-64/64-bit resume/lost ACK/different file/collision/pause; no whole-file arrayBuffer');
})().catch(error=>{console.error(error);process.exitCode=1;});

async function browserScenario(options){
 const elements=new Map(),callbacks=new Map();
 const element=id=>{
  if(!elements.has(id))elements.set(id,{value:'',textContent:'',files:[],hidden:false,disabled:false,
   addEventListener:(event,callback)=>callbacks.set(id+':'+event,callback)});
  return elements.get(id);
 };
 const length=5*1024**3,id='a'.repeat(32),name='base.nsp';
 const selected={name,size:length,lastModified:123,slice:(start,end=length)=>({size:end-start,arrayBuffer:async()=>{
  assert.ok(end-start<=65536,'large file must never be loaded as ArrayBuffer');return new Uint8Array(end-start).buffer;
 }})};
 let job={id:'',phase:'idle',offset:0,received:0,length:0,free_bytes:80*1024**3,available:true,busy:false,error:''};
 let writes=0,lost=false,controlPromise,context;const completed=[];
 const response=(status,json={},headers={})=>({status,ok:status>=200&&status<300,json:async()=>JSON.parse(JSON.stringify(json)),
  headers:{get:key=>Object.entries(headers).find(([name])=>name.toLowerCase()===key.toLowerCase())?.[1]??null}});
 const fetch=async(url,request={})=>{
  if(url==='/api/v1/pair')return response(200,{session:'test-session'});
  assert.equal(request.headers.Authorization,'Bearer test-session');
  if(url==='/api/v1/status')return response(200,{uploads:true,transfer:job});
  if(url==='/api/v1/uploads'){
   if(options.collision&&writes===0&&element('name').value===name)return response(409,{error:'exists'});
   assert.equal(request.headers['Tus-Resumable'],'1.0.0');
   const metadata=Object.fromEntries(request.headers['Upload-Metadata'].split(',').map(s=>{const [key,b64]=s.split(' ');return[key,Buffer.from(b64,'base64').toString('utf8')];}));
   job={...job,id,name:metadata.filename,fingerprint:metadata.fingerprint,length:Number(request.headers['Upload-Length']),offset:0,received:0,phase:'receiving',busy:true};
   return response(201,{}, {Location:'/api/v1/uploads/'+id});
  }
  assert.ok(url.startsWith('/api/v1/uploads/'+id));
  if(request.method==='HEAD')return response(200,{}, {'Upload-Offset':String(job.offset),'Upload-Length':String(job.length)});
  if(url.endsWith('/finish')){assert.equal(job.offset,length);job.phase='complete';job.busy=false;completed.push(job.name);return response(204);}
  if(url.endsWith('/pause')){job.phase='paused';job.busy=false;return response(204);}
  if(url.endsWith('/rename')){job.name=JSON.parse(request.body).name;return response(204);}
  throw Error('unexpected request '+url);
 };
 class XHR{
  open(method,url){assert.equal(method,'PATCH');this.url=url;this.headers={};}
  setRequestHeader(key,value){this.headers[key]=value;}
  getResponseHeader(key){return key==='Upload-Offset'?String(job.offset):null;}
  abort(){this.onabort?.();}
  send(blob){
   assert.equal(this.headers['Content-Type'],'application/offset+octet-stream');
   assert.equal(Number(this.headers['Upload-Offset']),job.offset);
   assert.ok(blob.size>0&&blob.size<=(options.chunk||16)*1048576);
   job.offset+=blob.size;job.received=job.offset;writes++;
   queueMicrotask(()=>{
    if(options.lostAck&&!lost){lost=true;this.onerror();return;}
    this.status=204;this.onload();
    if(options.pause&&writes===1)controlPromise=vm.runInContext('control("pause")',context);
   });
  }
 }
 context=vm.createContext({document:{getElementById:element,documentElement:{}},navigator:{language:'es'},
  location:{search:'?chunk='+(options.chunk||16)},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
  performance,XMLHttpRequest:XHR,fetch,btoa:value=>Buffer.from(value,'binary').toString('base64'),
  setTimeout:callback=>{queueMicrotask(callback);return 1;},setInterval:()=>1,clearInterval:()=>{},selected});
 vm.runInContext(script,context);
 if(options.resume||options.different){
  const signature=await vm.runInContext('fingerprint(selected)',context);
  const offset=4*1024**3+1048576;
  job={...job,id,name,fingerprint:options.different?'b'.repeat(64):signature,length,offset,received:offset,phase:'paused'};
 }
 element('code').value='12345678';await callbacks.get('pair:submit')({preventDefault(){}});
 element('files').files=options.queue?[selected,{...selected,name:'second.nsp'}]:[selected];callbacks.get('files:change')();
 await callbacks.get('send:click')();
 if(controlPromise)await controlPromise;
 if(options.different){assert.equal(writes,0);assert.match(element('message').textContent,/distinto/);}
 else if(options.collision){
  assert.equal(writes,0);assert.match(element('message').textContent,/existe/);
  element('name').value='renamed.nsp';await callbacks.get('send:click')();assert.equal(job.name,'renamed.nsp');assert.equal(job.phase,'complete');
 }else if(options.pause){assert.equal(job.offset,16*1048576);assert.equal(job.phase,'paused');}
 else{assert.equal(job.offset,length);assert.equal(job.phase,'complete');}
 if(options.queue)assert.deepEqual(completed,[name,'second.nsp']);
}
