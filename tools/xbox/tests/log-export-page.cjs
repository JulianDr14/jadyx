// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const page=fs.readFileSync('src/jadyx/storage/network_portal_page.h','utf8');
const script=page.match(/<script>([\s\S]*?)<\/script>/)[1];
async function scenario(language){
 const elements=new Map(),callbacks=new Map();let clicks=0,grants=0,exports=0;
 const element=id=>{
  if(!elements.has(id))elements.set(id,{value:'',files:[],textContent:'',hidden:false,disabled:false,
   addEventListener:(event,callback)=>callbacks.set(id+':'+event,callback),
   click:()=>{assert.equal(id,'logs-file');clicks++;}});
  return elements.get(id);
 };
 let logs={supported:true,phase:'idle',busy:false,total:5*1024**3,processed:0,compressed:0};
 const response=(data,status=200)=>({ok:status<400,status,json:async()=>structuredClone(data)});
 const fetch=async(path,options={})=>{
  if(path==='/api/v1/pair')return response({session:'test-session'});
  assert.equal(options.headers.Authorization,'Bearer test-session');
  if(path==='/api/v1/status')return response({logs,transfer:{},system:{}});
  if(path==='/api/v1/logs/export'){
   if(options.method==='DELETE'){logs={...logs,phase:'cancelled',busy:false};return response(logs);}
   assert.equal(options.method,'POST');exports++;logs={...logs,phase:'scanning',busy:true};return response(logs,202);
  }
  if(path==='/api/v1/logs/download'){
   grants++;assert.equal(options.method,'POST');
   return response({download:'/api/v1/logs/archive?ticket='+'a'.repeat(64),filename:'jadyx-logs-20261020-120000.tar.zst'});
  }
  throw Error('Unexpected fetch (archive must be a native download): '+path);
 };
 const context=vm.createContext({document:{getElementById:element,documentElement:{}},navigator:{language},location:{search:''},
  URLSearchParams,TextEncoder,Uint8Array,Uint32Array,DataView,performance,fetch,
  setInterval:()=>1,clearInterval:()=>{},setTimeout,btoa:v=>Buffer.from(v,'binary').toString('base64')});
 vm.runInContext(script,context);
 await callbacks.get('pair:submit')({preventDefault(){}});
 assert.equal(element('log-export').hidden,false);
 await callbacks.get('logs-download:click')();assert.equal(exports,1);
 // The download starts with the export and streams while it compresses.
 assert.equal(grants,1);assert.equal(clicks,1);
 assert.equal(element('logs-download').disabled,true);assert.equal(element('logs-cancel').hidden,false);
 assert.equal(element('logs-file').hidden,false,'retry link while streaming');
 logs={...logs,phase:'compressing',processed:logs.total/2,compressed:1000000};
 await vm.runInContext('refresh()',context);assert.equal(element('logs-progress').value,0.5);
 assert.equal(element('logs-file').hidden,false);
 logs={...logs,phase:'ready',busy:false,processed:logs.total};
 await vm.runInContext('refresh()',context);await new Promise(setImmediate);
 assert.equal(grants,1);assert.equal(clicks,1,'ready does not download a second time');
 assert.equal(element('logs-file').hidden,false);
 assert.match(element('logs-file').href,/^\/api\/v1\/logs\/archive\?ticket=[0-9a-f]{64}$/);
 await vm.runInContext('refresh()',context);assert.equal(clicks,1,'polling cannot download repeatedly');
 await callbacks.get('logs-download:click')();assert.equal(clicks,2);await callbacks.get('logs-cancel:click')();
 assert.equal(logs.phase,'cancelled');assert.equal(element('logs-file').hidden,true);
 assert.equal(element('logs-download').disabled,false);
 logs={...logs,phase:'failed',error:'space'};await vm.runInContext('refresh()',context);
 assert.notEqual(element('logs-status').textContent,'');
 assert.match(element('logs-title').textContent,/Logs/);
}
(async()=>{for(const language of ['es','en','pt-BR'])await scenario(language);
 console.log('PASS: logs UI es/en/pt-BR, 5GiB progress, cancel/error, one native download started with the export, no browser archive Blob');
})().catch(error=>{console.error(error);process.exitCode=1;});
