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
 for(const language of ['es','en','pt-BR'])await saveScenario(language);
 console.log('PASS: browser save tab: structure guide, file checks before upload, console refusals shown with their detail and discarded, save metadata, zip download (mock HTTP)');
 for(const language of ['es','en','pt-BR']){await amiiboScenario(language);await amiiboScenario(language,true);}
 console.log('PASS: browser amiibo: separate list, add and key panes, search and sort, per-file results, explicit replace, two-press deletion, key upload and removal (mock HTTP)');
 for(const language of ['es','en','pt-BR'])await navigationScenario(language);
 console.log('PASS: browser navigation: library first, import from it, upload controls only with an upload, unfinished upload reopened, system panes, logs without system imports (mock HTTP)');
})().catch(error=>{console.error(error);process.exitCode=1;});

async function browserScenario(options){
 const elements=new Map(),callbacks=new Map();
 const element=id=>{
  if(!elements.has(id))elements.set(id,{...dom(),value:'',textContent:'',files:[],hidden:false,disabled:false,
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
 context=vm.createContext({document:{getElementById:element,documentElement:{},body:{classList:{toggle(){},add(){},remove(){}},append(){}}},navigator:{language:'es'},
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
 if(job.phase==='complete'){
  assert.equal(element('job').textContent,'Listo para importar.');
  assert.equal(element('progress').value,0);
  assert.equal(element('queue').textContent,'');
 }
 if(options.queue)assert.deepEqual(completed,[name,'second.nsp']);
}

async function systemScenario(kind,zip,language){
 const elements=new Map(),callbacks=new Map();
 const element=id=>{if(!elements.has(id))elements.set(id,{...dom(),value:'',checked:false,textContent:'',files:[],hidden:false,disabled:false,
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
 const context=vm.createContext({document:{getElementById:element,documentElement:{},body:{classList:{toggle(){},add(){},remove(){}},append(){}}},navigator:{language},location:{search:''},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
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
 const make=tag=>{const n={...dom(),tag,value:'',textContent:'',files:[],hidden:false,disabled:false,style:{},dataset:{},children:[],listeners:{},
  addEventListener:(event,callback)=>{n.listeners[event]=callback;},
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
 const context=vm.createContext({document:{getElementById:element,createElement:make,documentElement:{},body:{classList:{toggle(){},add(){},remove(){}},append(){}}},navigator:{language},location:{search:''},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
  performance,XMLHttpRequest:XHR,fetch,URL:{createObjectURL:()=>'blob:x',revokeObjectURL(){}},btoa:v=>Buffer.from(v,'binary').toString('base64'),
  setTimeout:(cb,ms)=>{if(!(ms>=6000))queueMicrotask(cb);return 1;},clearTimeout:()=>{},setInterval:()=>1,clearInterval:()=>{}});
 vm.runInContext(script,context);
 element('code').value='1234';await element('pair').listeners.submit({preventDefault(){}});
 await delay();
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
async function saveScenario(language){
 const elements=new Map(),requests=[],links=[];
 const make=tag=>{const n={...dom(),tag,value:'',textContent:'',files:[],hidden:false,disabled:false,style:{},dataset:{},children:[],listeners:{},
  addEventListener:(event,callback)=>{n.listeners[event]=callback;},click(){links.push(n);},
  append:(...c)=>n.children.push(...c),replaceChildren:(...c)=>{n.children=c;}};return n;};
 const element=id=>{if(!elements.has(id))elements.set(id,make('#'+id));return elements.get(id);};
 const click=n=>n.listeners.click();
 const app='0100000000010000',upload='e'.repeat(32);
 let poll;
 let content={},refuse='',exported={status:200,body:'PK\x05\x06'};
 const response=(status,data={},headers={})=>({status,ok:status>=200&&status<300,json:async()=>JSON.parse(JSON.stringify(data)),
  blob:async()=>new Blob([data]),headers:{get:k=>headers[k]??null}});
 const fetch=async(url,request={})=>{
  if(url==='/api/v1/pair')return response(200,{session:'test-session'});
  assert.equal(request.headers.Authorization,'Bearer test-session');requests.push((request.method||'GET')+' '+url);
  if(url==='/api/v1/status')return response(200,{uploads:true,transfer:{busy:exported.busy},library:true,content,install:{done:0,total:0},removal:{serial:0,phase:''}});
  if(url==='/api/v1/library')return response(200,{revision:1,games:[{id:app,name:'Game',developer:'Dev',version:'1.0.0',removable:true}]});
  if(url.endsWith('/cover'))return response(404,{error:'cover'});
  if(url==='/api/v1/library/'+app+'/content')return response(200,{contents:[]});
  if(url==='/api/v1/library/'+app+'/save')return exported.status===200?response(200,exported.body):response(exported.status,{error:exported.error});
  if(url==='/api/v1/content/uploads'){
   const metadata=Object.fromEntries(request.headers['Upload-Metadata'].split(',').map(s=>{const [key,b64]=s.split(' ');return[key,Buffer.from(b64,'base64').toString('utf8')];}));
   assert.equal(metadata.kind,'save');assert.equal(metadata.application,app);assert.equal(metadata.filename,'mi partida.zip');
   content={id:upload,name:metadata.filename,fingerprint:metadata.fingerprint,tag:'save:'+app,length:Number(request.headers['Upload-Length']),offset:0,received:0,phase:'receiving',busy:true,available:true};
   return response(201,{}, {Location:'/api/v1/content/uploads/'+upload});
  }
  if(request.method==='DELETE'&&url==='/api/v1/content/uploads/'+upload){content={};return response(204);}
  if(request.method==='HEAD')return response(200,{}, {'Upload-Offset':String(content.offset)});
  if(url.endsWith('/finish')){assert.equal(content.offset,content.length);
   content=refuse?{...content,phase:'paused',busy:false,error:refuse}:{...content,phase:'complete',busy:false};return response(204);}
  throw Error('unexpected save request '+url);
 };
 class XHR{
  open(method,url){assert.equal(method,'PATCH');assert.equal(url,'/api/v1/content/uploads/'+upload);this.headers={};}
  setRequestHeader(k,v){this.headers[k]=v;}
  getResponseHeader(k){return k==='Upload-Offset'?String(content.offset):null;}
  send(blob){assert.equal(Number(this.headers['Upload-Offset']),content.offset);content.offset+=blob.size;content.received=content.offset;queueMicrotask(()=>{this.status=204;this.onload();});}
 }
 const fileOf=(name,bytes)=>{const blob=new Blob([Buffer.from(bytes)]);return{name,size:blob.size,lastModified:1,slice:(a,b)=>blob.slice(a,b)};};
 const zip=fileOf('mi partida.zip',[0x50,0x4B,0x03,0x04,...Array(60).fill(7)]);
 const context=vm.createContext({document:{getElementById:element,createElement:make,documentElement:{},body:{classList:{toggle(){},add(){},remove(){}},append(){}}},navigator:{language},location:{search:''},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
  performance,XMLHttpRequest:XHR,fetch,URL:{createObjectURL:()=>'blob:x',revokeObjectURL(){}},btoa:v=>Buffer.from(v,'binary').toString('base64'),
  setTimeout:(cb,ms)=>{if(!(ms>=6000))queueMicrotask(cb);return 1;},clearTimeout:()=>{},setInterval:callback=>{poll=callback;return 1;},clearInterval:()=>{}});
 vm.runInContext(script,context);
 element('code').value='1234';await element('pair').listeners.submit({preventDefault(){}});
 await delay();click(element('games').children[0]);await delay();
 click(element('kind-save'));await delay();
 assert.equal(element('save-pane').hidden,false);assert.equal(element('items').hidden,true);assert.equal(element('content-zone').hidden,true);
 assert.notEqual(element('kind-save').textContent,'');assert.notEqual(element('save-warning').textContent,'');
 // The guide names this game's folder.
 click(element('save-help-toggle'));assert.equal(element('save-help').hidden,false);
 assert.match(element('save-tree').textContent,/0100000000010000\//);assert.match(element('save-tree').textContent,/├─/);
 const error=()=>({shown:!element('save-error').hidden,text:element('save-error-text').textContent,help:!element('save-error-help').hidden});
 // Checked in the browser: nothing is uploaded.
 const before=requests.length,messages=new Set();
 for(const [file,help] of [[null,false],[fileOf('juego.nsp',[1,2,3,4]),true],[fileOf('partida.zip',[]),false],
  [fileOf('partida.zip',[0x3C,0x68,0x74,0x6D,0x6C,0x3E,0,0]),true],[{name:'grande.7z',size:2**30+1,slice:()=>{throw Error('read');}},false]]){
  element('save-file').files=file?[file]:[];await click(element('save-send'));
  const e=error();assert.ok(e.shown&&e.text,'precheck error shown');assert.equal(e.help,help,'guide link on structural errors');messages.add(e.text);
 }
 assert.equal(messages.size,5,'each precheck has its own message');
 assert.ok(!requests.slice(before).some(r=>r.includes('/content/uploads')),'prechecks never upload');
 element('save-file').listeners.change();assert.equal(element('save-error').hidden,true,'a new file clears the error');
 // Refused by the console: the page says which game it found and drops the upload.
 refuse='wrong_game:0100FFFF00000000';element('save-file').files=[zip];await click(element('save-send'));await delay();
 let e=error();assert.ok(e.shown&&e.help);assert.match(e.text,/0100FFFF00000000/);assert.match(e.text,/0100000000010000\//);
 assert.ok(requests.includes('DELETE /api/v1/content/uploads/'+upload),'refused save discarded');
 refuse='encrypted';element('save-file').files=[zip];await click(element('save-send'));await delay();
 e=error();assert.ok(e.shown&&!e.help&&!/0100FFFF/.test(e.text));
 refuse='';element('save-file').files=[zip];await click(element('save-send'));await delay();
 assert.equal(element('save-error').hidden,true);assert.notEqual(element('library-note').textContent,'');assert.equal(content.phase,'complete');
 // An unrelated upload blocks library writes, but the save can still be downloaded.
 exported.busy=true;await poll();
 assert.equal(element('save-send').disabled,true);
 assert.equal(element('save-download').disabled,false);
 await click(element('save-download'));await delay();
 assert.equal(links.length,1);assert.equal(links[0].download,'0100000000010000-save.zip');assert.equal(element('save-error').hidden,true);
 exported={status:404,error:'no_save'};await click(element('save-download'));await delay();
 e=error();assert.ok(e.shown&&e.text&&!e.help);assert.equal(links.length,1);
}
async function amiiboScenario(language,empty=false){
 const elements=new Map(),requests=[];
 const make=tag=>{const n={...dom(),tag,value:'',textContent:'',files:[],hidden:false,disabled:false,style:{},dataset:{},children:[],listeners:{},
  addEventListener:(event,callback)=>{n.listeners[event]=callback;},
  append:(...c)=>n.children.push(...c),replaceChildren:(...c)=>{n.children=c;}};return n;};
 const element=id=>{if(!elements.has(id))elements.set(id,make('#'+id));return elements.get(id);};
 const click=n=>n.listeners.click();
 const stored=new Map(empty?[]:[['old.bin',new Uint8Array(540)]]);let key=false,body;
 const response=(status,data={})=>({status,ok:status>=200&&status<300,json:async()=>JSON.parse(JSON.stringify(data)),headers:{get:()=>null}});
 const fetch=async(url,request={})=>{
  if(url==='/api/v1/pair')return response(200,{session:'test-session'});
  assert.equal(request.headers.Authorization,'Bearer test-session');requests.push((request.method||'GET')+' '+url);
  if(url==='/api/v1/status')return response(200,{uploads:true,transfer:{},amiibo:true});
  if(url==='/api/v1/amiibo')return response(200,{files:[...stored].map(([name,bytes])=>({name,bytes:bytes.byteLength})),key});
  const [path,query]=url.split('?'),params=new URLSearchParams(query);
  if(path==='/api/v1/amiibo/files'){
   const name=params.get('name');
   if(request.method==='DELETE'){stored.delete(name);return response(204);}
   assert.equal(request.headers['Content-Type'],'application/octet-stream');body=request.body;
   if(request.body.byteLength!==540)return response(400,{error:'not_amiibo'});
   if(stored.has(name)&&params.get('replace')!=='1')return response(409,{error:'exists'});
   stored.set(name,request.body);return response(201);
  }
  if(path==='/api/v1/amiibo/key'){
   if(request.method==='DELETE'){key=false;return response(204);}
   if(request.body.byteLength!==160)return response(400,{error:'not_key'});
   key=true;return response(204);
  }
  throw Error('unexpected amiibo request '+url);
 };
 const fileOf=(name,size)=>({name,size,arrayBuffer:async()=>new ArrayBuffer(size)});
 const context=vm.createContext({document:{getElementById:element,createElement:make,documentElement:{},body:{classList:{toggle(){},add(){},remove(){}},append(){}}},navigator:{language},location:{search:''},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
  performance,fetch,URL:{createObjectURL:()=>'blob:x',revokeObjectURL(){}},btoa:v=>Buffer.from(v,'binary').toString('base64'),
  setTimeout:(cb,ms)=>{if(!(ms>=6000))queueMicrotask(cb);return 1;},clearTimeout:()=>{},setInterval:()=>1,clearInterval:()=>{}});
 vm.runInContext(script,context);
 assert.ok(/id="tab-amiibo"[^>]* hidden/.test(header)&&/id="view-amiibo"[^>]* hidden/.test(header),'not offered before pairing');
 assert.notEqual(element('amiibo-intro').textContent,'');
 element('code').value='1234';await element('pair').listeners.submit({preventDefault(){}});
 assert.equal(element('tab-amiibo').hidden,false);assert.equal(element('view-amiibo').hidden,true);
 click(element('tab-amiibo'));await delay();
 assert.equal(element('view-amiibo').hidden,false);assert.equal(element('view-content').hidden,true);
 // With nothing to manage it opens on adding; with something, on the list.
 assert.equal(element('amiibo-pane-add').hidden,!empty);assert.equal(element('amiibo-pane-list').hidden,empty);assert.equal(element('amiibo-pane-key').hidden,true);
 assert.equal(element('amiibo-tools').hidden,empty);
 if(empty)return;
 assert.equal(element('amiibo-items').children.length,1);assert.equal(element('amiibo-key-remove').hidden,true);
 const noKey=element('amiibo-key-status').textContent;assert.notEqual(noKey,'');
 // Nothing chosen: says so and sends nothing.
 const before=requests.length;await click(element('amiibo-send'));await click(element('amiibo-key-send'));
 assert.equal(requests.length,before);assert.notEqual(element('amiibo-note').textContent,'');
 // One bad file does not stop the others; a file too big never leaves the browser.
 element('amiibo-files').files=[fileOf('uno á.bin',540),fileOf('malo.bin',16),fileOf('grande.bin',5000),fileOf('old.bin',540),fileOf('dos.bin',540)];
 await click(element('amiibo-send'));await delay();
 assert.ok(stored.has('uno á.bin')&&stored.has('dos.bin'),'good files stored');assert.ok(!stored.has('malo.bin'));
 assert.ok(requests.includes('POST /api/v1/amiibo/files?name=uno%20%C3%A1.bin'),'name encoded');
 assert.ok(!requests.some(r=>r.includes('grande')),'oversized file not sent');
 const results=element('amiibo-results').children;assert.equal(results.length,5);
 const detail=i=>results[i].children[0].children[1].textContent;
 assert.equal(detail(0),detail(4));assert.notEqual(detail(1),detail(0));assert.equal(detail(1),detail(2));
 // Only the taken name offers to replace, and it takes that press.
 assert.equal(results.filter(r=>r.children.length>1).length,1);assert.ok(!requests.some(r=>r.includes('replace=1')));
 assert.equal(element('amiibo-items').children.length,3);assert.equal(element('amiibo-picked').textContent,'');
 await click(results[3].children[1]);await delay();
 assert.ok(requests.includes('POST /api/v1/amiibo/files?name=old.bin&replace=1'));
 assert.equal(element('amiibo-results').children[3].children.length,1,'replaced: nothing left to confirm');
 // One press arms a deletion; only the second sends it.
 const row=name=>element('amiibo-items').children.find(r=>r.children[0].children[0].textContent===name);
 await click(row('dos.bin').children[1]);assert.ok(stored.has('dos.bin'),'first press never deletes');
 await click(row('dos.bin').children[1]);await delay();
 assert.ok(!stored.has('dos.bin'));assert.equal(row('dos.bin'),undefined);
 // Managing a long list: A-Z by default, numbers in order, search ignoring case and accents.
 for(const name of ['Zeta.bin','Figura 10.bin','Figura 2.bin','Piñón.bin'])stored.set(name,new Uint8Array(540));
 click(element('tab-amiibo'));await delay();
 const names=()=>element('amiibo-items').children.map(r=>r.children[0].children[0].textContent);
 const search=async text=>{element('amiibo-search').value=text;await element('amiibo-search').listeners.input();};
 assert.deepEqual(Array.from(names()),['Figura 2.bin','Figura 10.bin','old.bin','Piñón.bin','uno á.bin','Zeta.bin']);
 const total=element('amiibo-count').textContent;assert.ok(total.startsWith('6'),total);
 click(element('amiibo-sort'));assert.equal(names()[0],'Zeta.bin');click(element('amiibo-sort'));assert.equal(names()[0],'Figura 2.bin');
 await search('PINON');assert.deepEqual(Array.from(names()),['Piñón.bin']);assert.ok(element('amiibo-count').textContent.startsWith('1 '));assert.ok(element('amiibo-count').textContent.includes('6'));
 // The search survives a removal, and a press only arms the row it was pressed on.
 await search('figura');await click(row('Figura 2.bin').children[1]);await click(row('Figura 2.bin').children[1]);await delay();
 assert.ok(!stored.has('Figura 2.bin'));assert.deepEqual(Array.from(names()),['Figura 10.bin']);
 await search('nada');assert.equal(element('amiibo-items').children[0].children.length,0,'no match: a message, no rows');assert.notEqual(element('amiibo-items').children[0].textContent,'');
 await search('');assert.equal(names().length,5);
 for(const name of ['Zeta.bin','Figura 10.bin','Piñón.bin'])stored.delete(name);click(element('tab-amiibo'));await delay();
 // Each part is on its own: the list, the upload and the key are never shown together.
 const shown=()=>['list','add','key'].filter(pane=>!element('amiibo-pane-'+pane).hidden);
 assert.deepEqual(Array.from(shown()),['list']);
 click(element('amiibo-tab-add'));assert.deepEqual(Array.from(shown()),['add']);assert.equal(element('amiibo-tab-add').ariaSelected,'true');
 click(element('amiibo-tab-key'));assert.deepEqual(Array.from(shown()),['key']);
 // The key: refused with its reason, then installed, then removed in two presses.
 element('amiibo-key-file').files=[fileOf('key_retail.bin',10)];await click(element('amiibo-key-send'));await delay();
 assert.equal(key,false);const refused=element('amiibo-note').textContent;assert.notEqual(refused,'');
 element('amiibo-key-file').files=[fileOf('key_retail.bin',160)];await click(element('amiibo-key-send'));await delay();
 assert.equal(key,true);assert.notEqual(element('amiibo-note').textContent,refused);
 assert.notEqual(element('amiibo-key-status').textContent,noKey);assert.equal(element('amiibo-key-remove').hidden,false);
 await click(element('amiibo-key-remove'));assert.equal(key,true,'first press never deletes');
 await click(element('amiibo-key-remove'));await delay();
 assert.equal(key,false);assert.equal(element('amiibo-key-status').textContent,noKey);assert.equal(element('amiibo-key-remove').hidden,true);
 // Back to another tab: the section hides.
 click(element('tab-games'));assert.equal(element('view-amiibo').hidden,true);assert.equal(element('upload').hidden,false);
}
async function navigationScenario(language){
 const open=async status=>{
  const elements=new Map();
  const make=tag=>{const n={...dom(),tag,value:'',textContent:'',files:[],hidden:false,disabled:false,style:{},dataset:{},children:[],listeners:{},
   addEventListener:(event,callback)=>{n.listeners[event]=callback;},
   append:(...c)=>n.children.push(...c),replaceChildren:(...c)=>{n.children=c;}};return n;};
  const element=id=>{if(!elements.has(id))elements.set(id,make('#'+id));return elements.get(id);};
  const response=data=>({status:200,ok:true,json:async()=>JSON.parse(JSON.stringify(data)),headers:{get:()=>null}});
  const fetch=async url=>url==='/api/v1/pair'?response({session:'test-session'}):url==='/api/v1/status'?response(status):
   url==='/api/v1/library'?response({revision:1,games:[{id:'0100000000010000',name:'Game',removable:true}]}):response({contents:[]});
  const context=vm.createContext({document:{getElementById:element,createElement:make,documentElement:{},body:{classList:{toggle(){},add(){},remove(){}},append(){}}},navigator:{language},location:{search:''},URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,
   performance,fetch,URL:{createObjectURL:()=>'blob:x',revokeObjectURL(){}},btoa:v=>Buffer.from(v,'binary').toString('base64'),
   setTimeout:(cb,ms)=>{if(!(ms>=6000))queueMicrotask(cb);return 1;},clearTimeout:()=>{},setInterval:()=>1,clearInterval:()=>{}});
  vm.runInContext(script,context);
  element('code').value='1234';await element('pair').listeners.submit({preventDefault(){}});await delay();
  return element;
 };
 const click=n=>n.listeners.click();
 // Nothing uploading: the library comes first and importing is one press away, without its controls.
 let page=await open({uploads:true,transfer:{},library:true,system_supported:true,system:{phase:'idle',available:true},logs:{supported:true}});
 assert.equal(page('library').hidden,false);assert.equal(page('upload').hidden,true);assert.equal(page('library-add').hidden,false);
 click(page('library-add'));assert.equal(page('upload').hidden,false);assert.equal(page('library').hidden,true);assert.equal(page('upload-back').hidden,false);
 for(const id of ['pause','discard','rename','upload-panel','name-field'])assert.equal(page(id).hidden,true,id+' waits for an upload');
 page('files').files=[{name:'a.nsp',size:1}];page('files').listeners.change();assert.equal(page('name-field').hidden,false,'one file can be named');
 page('files').files=[{name:'a.nsp',size:1},{name:'b.nsp',size:1}];page('files').listeners.change();assert.equal(page('name-field').hidden,true,'several keep their names');
 click(page('upload-back'));await delay();assert.equal(page('library').hidden,false);assert.equal(page('upload').hidden,true);
 // Games pressed again leaves a game for the library.
 click(page('games').children[0]);await delay();assert.equal(page('game').hidden,false);
 for(const id of ['content-pause','content-discard','content-panel'])assert.equal(page(id).hidden,true,id+' waits for an upload');
 click(page('tab-games'));await delay();assert.equal(page('game').hidden,true);assert.equal(page('library-list').hidden,false);
 // System: one pane at a time, the job only with a job.
 click(page('tab-system'));assert.equal(page('view-system').hidden,false);assert.equal(page('view-content').hidden,true);
 assert.equal(page('keys-pane').hidden,false);assert.equal(page('firmware-pane').hidden,true);assert.equal(page('log-export').hidden,true);
 assert.equal(page('system-panel').hidden,true);assert.equal(page('system-cancel').hidden,true);
 click(page('cat-firmware'));assert.equal(page('keys-pane').hidden,true);assert.equal(page('firmware-pane').hidden,false);
 click(page('cat-logs'));assert.equal(page('system-upload').hidden,true);assert.equal(page('log-export').hidden,false);
 // An unfinished upload is reopened with its controls, and the library would say so.
 page=await open({uploads:true,library:true,transfer:{id:'a'.repeat(32),name:'a.nsp',phase:'paused',length:100,received:40,offset:40}});
 assert.equal(page('upload').hidden,false);assert.equal(page('library').hidden,true);
 for(const id of ['pause','discard','rename','upload-panel','name-field'])assert.equal(page(id).hidden,false,id+' with an upload');
 assert.match(page('library-add').textContent,/40 %/);
 // No system imports: the logs stay reachable. Neither: no tab.
 page=await open({uploads:true,transfer:{},logs:{supported:true}});
 assert.equal(page('tab-system').hidden,false);assert.equal(page('cat-keys').hidden,true);assert.equal(page('cat-logs').hidden,false);assert.equal(page('upload').hidden,false);
 click(page('tab-system'));assert.equal(page('log-export').hidden,false);assert.equal(page('upload-back').hidden,true);
 page=await open({uploads:true,transfer:{}});assert.equal(page('tab-system').hidden,true);assert.equal(page('tab-amiibo').hidden,true);
}
function delay(){return new Promise(resolve=>setImmediate(resolve));}
// What the page touches on any element beyond its own fields.
function dom(){return{style:{},dataset:{},classList:{add(){},remove(){},toggle(){},contains:()=>false},setAttribute(){},removeAttribute(){},focus(){},append(){},replaceChildren(){},querySelector:()=>null};}
