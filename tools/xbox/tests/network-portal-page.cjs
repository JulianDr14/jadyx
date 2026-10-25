// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
const fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const header=fs.readFileSync('src/jadyx/storage/network_portal_page.h','utf8');
const script=header.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script,{filename:'network-portal-page.js'});
// /logo.png serves the app's controller logo; its embedded bytes must match the packaged asset.
const logo=fs.readFileSync('src/jadyx/storage/network_portal_logo.h','utf8').match(/Logo\[\]=\{([\s\S]*?)\};/)[1].match(/0x[0-9a-f]{2}/g).map(Number);
assert.ok(Buffer.from(logo).equals(fs.readFileSync('dist/uwp/Assets/Square150x150Logo.scale-200.png')),'network_portal_logo.h is stale');
const formatDuration=vm.runInNewContext('('+script.match(/function formatDuration\(value\)\{[\s\S]*?\n\}/)[0]+')');
for(const [seconds,expected] of [[0,'0 s'],[59.1,'1 min 0 s'],[890,'14 min 50 s'],[3661,'1 h 1 min 1 s'],[90061,'25 h 1 min 1 s'],[Infinity,'—']]){
 assert.equal(formatDuration(seconds),expected);
}
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
 for(const language of ['es','en','pt-BR'])for(const kind of ['keys','firmware'])for(const zip of [false,true])await systemScenario(kind,zip,language);
 console.log('PASS: browser keys/firmware, ZIP/loose, review warning checkbox and explicit confirmation (mock HTTP)');
 for(const language of ['es','en','pt-BR'])await libraryScenario(language);
 console.log('PASS: browser library, update upload with kind/game metadata, two-press content and game deletion (mock HTTP)');
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
 element('code').value='1234';await callbacks.get('pair:submit')({preventDefault(){}});
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

async function systemScenario(kind,zip,language){
 const elements=new Map(),callbacks=new Map();
 const element=id=>{if(!elements.has(id))elements.set(id,{value:'',checked:false,textContent:'',files:[],hidden:false,disabled:false,
  addEventListener:(event,callback)=>callbacks.set(id+':'+event,callback)});return elements.get(id);};
 const id='b'.repeat(32),fileid='c'.repeat(32),length=1024*1024+1;
 const selected={name:zip?'source.zip':kind==='keys'?'prod.keys':'0'.repeat(32)+'.nca',size:length,lastModified:123,
  slice:(start,end=length)=>({size:end-start,arrayBuffer:async()=>{assert.ok(end-start<=65536);return new Uint8Array(end-start).buffer;}})};
 let system={phase:'idle',available:true,busy:false},confirmations=0,writes=0;
 const response=(status,data={},headers={})=>({status,ok:status>=200&&status<300,json:async()=>JSON.parse(JSON.stringify(data)),headers:{get:k=>headers[k]??null}});
 const fetch=async(url,request={})=>{
  if(url==='/api/v1/pair')return response(200,{session:'test-session'});
  assert.equal(request.headers.Authorization,'Bearer test-session');
  if(url==='/api/v1/status')return response(200,{uploads:true,transfer:{available:false},system_supported:true,system});
  if(url==='/api/v1/system/jobs'){
   assert.deepEqual(JSON.parse(request.body),{kind,format:zip?'zip':'files',count:1});
   system={...system,id,kind,format:zip?'zip':'files',count:1,completed:0,files:[],phase:'receiving',busy:true,file:{}};return response(201,{id});
  }
  if(url.endsWith('/files')){
   const body=JSON.parse(request.body);system.file={...body,id:fileid,offset:0,received:0,phase:'receiving'};
   return response(201,{}, {Location:'/api/v1/system/files/'+fileid});
  }
  if(request.method==='HEAD')return response(200,{}, {'Upload-Offset':String(system.file.offset),'Upload-Length':String(length)});
  if(url.endsWith('/finish')){assert.equal(system.file.offset,length);system.completed=1;system.file.phase='complete';system.files=[system.file];return response(204);}
  if(url.endsWith('/prepare')){system.phase='review';system.review={installed_firmware:'20.0.0',incoming_firmware:'19.0.0',installed_generation:20,incoming_generation:19,required_generation:20,warnings:['keys_downgrade','keys_firmware_old']};return response(202);}
  if(url.endsWith('/confirm')){
   if(!JSON.parse(request.body).accept_warnings)return response(409,{error:'confirm_warnings'});
   confirmations++;system.phase='complete';system.busy=false;return response(202);
  }
  throw Error('unexpected system request '+url);
 };
 class XHR{
  open(method,url){assert.equal(method,'PATCH');assert.equal(url,'/api/v1/system/files/'+fileid);this.headers={};}
  setRequestHeader(k,v){this.headers[k]=v;}
  getResponseHeader(k){return k==='Upload-Offset'?String(system.file.offset):null;}
  send(blob){assert.equal(Number(this.headers['Upload-Offset']),system.file.offset);system.file.offset+=blob.size;system.file.received=system.file.offset;writes++;queueMicrotask(()=>{this.status=204;this.onload();});}
 }
 const context=vm.createContext({document:{getElementById:element,documentElement:{}},navigator:{language},location:{search:''},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
  performance,XMLHttpRequest:XHR,fetch,btoa:v=>Buffer.from(v,'binary').toString('base64'),setTimeout:cb=>{queueMicrotask(cb);return 1;},setInterval:()=>1,clearInterval:()=>{}});
 vm.runInContext(script,context);
 assert.equal(element('keys-title').textContent,language==='es'?'Importar claves':language==='pt-BR'?'Importar chaves':'Import keys','initial keys card is a title, never an error');
 element('code').value='1234';await callbacks.get('pair:submit')({preventDefault(){}});
 element(kind+'-files').files=[selected];await callbacks.get(kind+'-send:click')();
 assert.equal(writes,1);assert.equal(system.phase,'review');assert.equal(confirmations,0);
 assert.equal(element('system-confirm').disabled,true);assert.match(element('system-versions').textContent,/20\.0\.0/);
 await callbacks.get('system-confirm:click')();assert.equal(confirmations,0,'server still rejects direct unaccepted confirmation');
 element('system-warning-accept').checked=true;callbacks.get('system-warning-accept:change')();assert.equal(element('system-confirm').disabled,false);
 await callbacks.get('system-confirm:click')();assert.equal(confirmations,1);assert.equal(system.phase,'complete');
}

async function libraryScenario(language){
 const elements=new Map(),requests=[];
 const make=tag=>{const n={tag,value:'',textContent:'',files:[],hidden:false,disabled:false,style:{},dataset:{},children:[],listeners:{},
  classList:{add(){},remove(){}},addEventListener:(event,callback)=>{n.listeners[event]=callback;},
  append:(...c)=>n.children.push(...c),replaceChildren:(...c)=>{n.children=c;}};return n;};
 const element=id=>{if(!elements.has(id))elements.set(id,make('#'+id));return elements.get(id);};
 const click=n=>n.listeners.click();
 const app='0100000000010000',item='d'.repeat(64),upload='e'.repeat(32),length=3*1048576+5;
 let games=[{id:app,name:'Game',developer:'Dev',version:'1.0.0',removable:true}],contents=[{id:item,kind:'update',name:'Update',version:'1.0.1',bytes:1048576,internal:true}];
 let removal={serial:0,game:'',content:'',phase:'',error:''},content={},revision=1;
 const response=(status,data={},headers={})=>({status,ok:status>=200&&status<300,json:async()=>JSON.parse(JSON.stringify(data)),headers:{get:k=>headers[k]??null}});
 const fetch=async(url,request={})=>{
  if(url==='/api/v1/pair')return response(200,{session:'test-session'});
  assert.equal(request.headers.Authorization,'Bearer test-session');requests.push((request.method||'GET')+' '+url);
  // The console runs a queued deletion before the next status read.
  if(url==='/api/v1/status'){
   if(removal.phase==='pending'){removal={...removal,phase:'complete'};
    if(removal.content)contents=contents.filter(c=>c.id!==removal.content);else{games=[];revision++;}}
   return response(200,{uploads:true,transfer:{},library:true,content,install:{done:0,total:0},removal});
  }
  if(url==='/api/v1/library')return response(200,{revision,games});
  if(url.endsWith('/cover'))return response(404,{error:'cover'});
  if(url==='/api/v1/library/'+app+'/content')return response(200,{contents});
  if(request.method==='DELETE'&&url.startsWith('/api/v1/library/'+app)){
   const id=url.split('/content/')[1]||'';removal={serial:removal.serial+1,game:app,content:id,phase:'pending',error:''};return response(202,{removal});
  }
  if(url==='/api/v1/content/uploads'){
   const metadata=Object.fromEntries(request.headers['Upload-Metadata'].split(',').map(s=>{const [key,b64]=s.split(' ');return[key,Buffer.from(b64,'base64').toString('utf8')];}));
   assert.equal(metadata.kind,'update');assert.equal(metadata.application,app);assert.equal(metadata.filename,'patch.nsp');
   content={id:upload,name:metadata.filename,fingerprint:metadata.fingerprint,tag:'update:'+app,length,offset:0,received:0,phase:'receiving',busy:true,available:true};
   return response(201,{}, {Location:'/api/v1/content/uploads/'+upload});
  }
  if(request.method==='HEAD')return response(200,{}, {'Upload-Offset':String(content.offset)});
  if(url.endsWith('/finish')){assert.equal(content.offset,length);content={...content,phase:'complete',busy:false};
   contents=[...contents,{id:'f'.repeat(64),kind:'update',name:'Update',version:'1.0.2',bytes:length,internal:true}];return response(204);}
  throw Error('unexpected library request '+url);
 };
 class XHR{
  open(method,url){assert.equal(method,'PATCH');assert.equal(url,'/api/v1/content/uploads/'+upload);this.headers={};}
  setRequestHeader(k,v){this.headers[k]=v;}
  getResponseHeader(k){return k==='Upload-Offset'?String(content.offset):null;}
  send(blob){assert.equal(Number(this.headers['Upload-Offset']),content.offset);content.offset+=blob.size;content.received=content.offset;queueMicrotask(()=>{this.status=204;this.onload();});}
 }
 const file={name:'patch.nsp',size:length,lastModified:1,slice:(start,end=length)=>({size:end-start,arrayBuffer:async()=>new Uint8Array(end-start).buffer})};
 const context=vm.createContext({document:{getElementById:element,createElement:make,documentElement:{}},navigator:{language},location:{search:''},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
  performance,XMLHttpRequest:XHR,fetch,URL:{createObjectURL:()=>'blob:x',revokeObjectURL(){}},btoa:v=>Buffer.from(v,'binary').toString('base64'),
  setTimeout:(cb,ms)=>{if(!(ms>=6000))queueMicrotask(cb);return 1;},clearTimeout:()=>{},setInterval:()=>1,clearInterval:()=>{}});
 vm.runInContext(script,context);
 element('code').value='1234';await element('pair').listeners.submit({preventDefault(){}});
 assert.equal(element('cat-library').hidden,false);
 click(element('cat-library'));await delay();
 assert.equal(element('upload').hidden,true);assert.equal(element('library').hidden,false);assert.equal(element('games').children.length,1);
 click(element('games').children[0]);await delay();
 assert.equal(element('game').hidden,false);assert.equal(element('game-name').textContent,'Game');assert.equal(element('items').children.length,1);
 // One press arms the deletion; only the second sends it.
 let remove=()=>element('items').children[0].children[1];
 await click(remove());assert.ok(!requests.some(r=>r.startsWith('DELETE')),'first press never deletes');
 await click(remove());assert.ok(requests.includes('DELETE /api/v1/library/'+app+'/content/'+item));
 await delay();assert.equal(element('items').children[0].tag,'li');assert.equal(element('items').children[0].className,'empty');
 assert.notEqual(element('library-note').textContent,'');
 element('content-files').files=[file];await click(element('content-send'));await delay();
 assert.equal(content.phase,'complete');assert.equal(element('items').children.length,1,'installed update is listed');
 await click(element('game-remove'));assert.ok(!requests.includes('DELETE /api/v1/library/'+app));
 await click(element('game-remove'));assert.ok(requests.includes('DELETE /api/v1/library/'+app));
 await delay();assert.equal(element('game').hidden,true);assert.equal(element('library-list').hidden,false);assert.equal(element('games').children.length,0);
}
function delay(){return new Promise(resolve=>setImmediate(resolve));}
