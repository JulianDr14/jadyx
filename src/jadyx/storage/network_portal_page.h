// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
namespace Jadyx::PortalWeb {
inline constexpr std::string_view Page=R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Jadyx · Import</title><style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#08090b;color:#f5f5f7}
*{box-sizing:border-box}body{max-width:920px;margin:5vh auto;padding:24px}header{letter-spacing:.3em;font-weight:600}
main,section{margin-top:24px;padding:28px;border:1px solid #ffffff14;border-radius:18px;background:#111315}
h1{font-size:32px}p,label,small{color:#92959a;line-height:1.6}input,button{font:inherit;padding:14px;border-radius:10px}
input{width:100%;background:#08090b;color:inherit;border:1px solid #333537}input#code{letter-spacing:.25em}
button{margin-top:16px;border:0;background:#f5f5f7;color:#08090b;cursor:pointer}button.secondary{background:#1c1f23;color:#f5f5f7}
button:disabled{opacity:.45;cursor:default}label{display:block;margin:14px 0 8px}.actions{display:flex;gap:12px;flex-wrap:wrap}
progress{width:100%;height:10px;accent-color:#f5f5f7}#job,#message{white-space:pre-wrap;overflow-wrap:anywhere}
#message{color:#f0c27a}.stats{display:flex;gap:24px;flex-wrap:wrap;font-variant-numeric:tabular-nums}
[hidden]{display:none!important}@media(max-width:600px){body{padding:16px}main,section{padding:20px}h1{font-size:26px}.actions button{flex:1}}
</style><header>JADYX</header><main><h1 id="title">Connect to your console</h1>
<p id="intro">Enter the temporary code shown in Jadyx. Keep the app open in the library.</p>
<form id="pair"><label id="label" for="code">Connection code</label>
<input id="code" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{8}" maxlength="8" required>
<button id="connect">Connect</button></form><p id="connection" role="status" aria-live="polite"></p></main>
<section id="upload" hidden><h1 id="uploadtitle">Import NSP games</h1><p id="uploadintro">Base games only. Files are copied to your console's internal storage.</p>
<p id="space"></p><label id="fileslabel" for="files">Choose NSP files</label><input id="files" type="file" accept=".nsp" multiple>
<label id="namelabel" for="name">Destination name (one file)</label><input id="name" maxlength="100" autocomplete="off">
<div class="actions"><button id="send">Upload / resume</button><button class="secondary" id="rename">Rename pending file</button>
<button class="secondary" id="pause">Pause</button><button class="secondary" id="discard">Discard partial file</button></div>
<p id="job" role="status" aria-live="polite"></p><progress id="progress" max="1" value="0"></progress>
<div class="stats"><p id="confirmed"></p><p id="rate"></p><p id="eta"></p></div><p id="queue"></p></section>
<p id="message" role="alert"></p><p id="privacy">Local network · HTTP connection without encryption.</p>
<script>
'use strict';
const $=id=>document.getElementById(id),es=navigator.language.toLowerCase().startsWith('es');
const words=es?{
 title:'Conecta con tu consola',intro:'Introduce el código temporal que muestra Jadyx. Mantén la app abierta en la biblioteca.',label:'Código de conexión',connect:'Conectar',
 uploadtitle:'Importar juegos NSP',uploadintro:'Solo juegos base. Los archivos se copian al almacenamiento interno de la consola.',fileslabel:'Seleccionar archivos NSP',
 namelabel:'Nombre de destino (un solo archivo)',send:'Subir / reanudar',rename:'Renombrar archivo pendiente',pause:'Pausar',discard:'Eliminar archivo parcial',
 privacy:'Red local · Conexión HTTP sin cifrado.',ok:'Conexión establecida.',bad:'Código incorrecto o caducado. Genera otro en Jadyx.',
 lost:'La conexión se ha cerrado. Vuelve a conectarte desde Jadyx; el progreso confirmado se conserva.',free:'Espacio libre',confirmed:'Confirmado',
 speed:'Velocidad',eta:'Tiempo restante',queue:'En cola',choose:'Selecciona uno o varios archivos NSP.',same:'Hay un archivo pendiente distinto. Selecciona el mismo archivo para reanudarlo o elimina la transferencia parcial.',
 receiving:'Recibiendo',paused:'En pausa. Selecciona el mismo archivo para continuar.',ready:'Esperando a validar',validating:'Validando NSP',publishing:'Añadiendo a la biblioteca',complete:'Instalado en la biblioteca',
 busy:'Espera a que termine la operación en Jadyx.',exists:'Ese nombre ya existe. Cambia el nombre de destino y vuelve a intentarlo.',space:'No hay espacio suficiente.',
 file:'Nombre o archivo no válido. Selecciona un NSP.',base:'Solo se admiten juegos base; no actualizaciones, DLC ni paquetes mezclados.',
 nsp:'La estructura del NSP no es válida.',incomplete:'El NSP está incompleto.',keys:'No se pueden leer sus metadatos. Revisa las claves de tu consola y el archivo NSP.',
 disk:'No se pudo escribir o publicar el archivo.',interrupted:'Transferencia interrumpida. Puedes reanudar desde el progreso confirmado.',
 partial:'Hay una transferencia pendiente. Reanúdala o elimínala.',recovery:'No se pudo recuperar el parcial. Elimínalo antes de iniciar otro.',unavailable:'La importación no está disponible en este momento.',
 attempts:'Demasiados intentos. Espera un minuto.',version:'Versión de transferencia incompatible. Recarga la página.',idle:'Listo para importar.'
}:{ok:'Connected.',bad:'Incorrect or expired code. Generate another in Jadyx.',lost:'Connection closed. Reconnect in Jadyx; confirmed progress is retained.',
 free:'Free space',confirmed:'Confirmed',speed:'Speed',eta:'Time left',queue:'Queued',choose:'Choose one or more NSP files.',
 same:'A different file is pending. Select the same file to resume, or discard the partial upload.',receiving:'Receiving',paused:'Paused. Select the same file to continue.',
 ready:'Waiting for validation',validating:'Validating NSP',publishing:'Adding to library',complete:'Installed in your library',busy:'Wait for the operation in Jadyx to finish.',
 exists:'This destination already exists. Change its name and try again.',space:'Not enough free space.',file:'Invalid filename or file. Choose an NSP.',
 base:'Base games only; updates, DLC and mixed packages are not supported.',nsp:'Invalid NSP structure.',incomplete:'The NSP is incomplete.',
 keys:'Cannot read its metadata. Check your console keys and NSP file.',disk:'Could not write or publish this file.',
 interrupted:'Transfer interrupted. You can resume from confirmed progress.',partial:'An upload is pending. Resume or discard it.',
 recovery:'Cannot recover the partial file. Discard it before starting another.',unavailable:'Import is currently unavailable.',
 attempts:'Too many attempts. Wait one minute.',version:'Incompatible upload version. Reload this page.',idle:'Ready to import.'};
if(es){document.documentElement.lang='es';for(const id of ['title','intro','label','connect','uploadtitle','uploadintro','fileslabel','namelabel','send','rename','pause','discard','privacy'])$(id).textContent=words[id];}
const selectedChunk=Number(new URLSearchParams(location.search).get('chunk'));
const chunk=(selectedChunk===4||selectedChunk===16||selectedChunk===64?selectedChunk:16)*1048576;
let session='',timer,state={},running=false,stopped=false,currentXHR=null,currentId='',rates=[];
const delay=ms=>new Promise(r=>setTimeout(r,ms)),mib=n=>(n/1048576).toFixed(1),reason=e=>words[e.message]||words.interrupted;
function headers(extra={}){return {Authorization:'Bearer '+session,'Tus-Resumable':'1.0.0',...extra};}
async function api(path,options={}){
 const response=await fetch(path,{...options,headers:headers(options.headers||{})});
 if(response.status===401){throw Error('lost');}
 if(!response.ok){let error='interrupted';try{error=(await response.json()).error||error;}catch{}throw Error(error);}
 return response;
}
function render(){
 const s=state.transfer||{};
 $('upload').hidden=!session||!state.uploads;
 $('space').textContent=words.free+': '+((s.free_bytes||0)/1073741824).toFixed(2)+' GiB';
 $('job').textContent=(s.name?s.name+'\n':'')+(words[s.phase]||words.idle);
 $('progress').value=s.length?Math.min(1,(s.received||0)/s.length):0;
 $('confirmed').textContent=words.confirmed+': '+mib(s.offset||0)+' / '+mib(s.length||0)+' MiB';
 $('files').disabled=running;$('name').disabled=running||s.phase==='validating'||s.phase==='publishing';
 $('send').disabled=running||!session||!state.uploads||s.phase==='ready'||s.phase==='validating'||s.phase==='publishing';
 $('pause').disabled=!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('discard').disabled=!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('rename').disabled=running||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 if(s.error)$('message').textContent=words[s.error]||words.interrupted;
}
async function refresh(){const response=await api('/api/v1/status');state=await response.json();render();return state;}
$('pair').addEventListener('submit',async event=>{
 event.preventDefault();$('connect').disabled=true;$('message').textContent='';
 try{
 const response=await fetch('/api/v1/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:$('code').value})});
 if(!response.ok){$('connection').textContent=response.status===429?words.attempts:words.bad;return;}
 session=(await response.json()).session;$('code').value='';$('pair').hidden=true;$('connection').textContent=words.ok;
 await refresh();clearInterval(timer);let polling=false;
 timer=setInterval(async()=>{if(polling)return;polling=true;try{await refresh();$('connection').textContent=words.ok;}catch(error){
 if(error.message==='lost'){clearInterval(timer);stopped=true;if(currentXHR)currentXHR.abort();session='';$('pair').hidden=false;$('upload').hidden=true;$('connection').textContent=words.lost;}
 else $('connection').textContent=words.interrupted;
 }finally{polling=false;}},1000);
 }catch{$('connection').textContent=words.lost;}finally{$('connect').disabled=false;}
});
// SHA-256 fallback is required because WebCrypto is not available on ordinary HTTP LAN origins.
// Only first/last 64 KiB + file identity are sampled: this is a resume fingerprint, not full integrity validation.
function sha256(input){
 const k=[0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
 const h=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
 const data=new Uint8Array(Math.ceil((input.length+9)/64)*64);data.set(input);data[input.length]=128;
 const view=new DataView(data.buffer);view.setUint32(data.length-8,Math.floor(input.length/0x20000000));view.setUint32(data.length-4,(input.length*8)>>>0);
 const w=new Uint32Array(64),rotr=(n,r)=>(n>>>r)|(n<<(32-r));
 for(let offset=0;offset<data.length;offset+=64){
 for(let i=0;i<16;i++)w[i]=view.getUint32(offset+4*i);
 for(let i=16;i<64;i++){const a=w[i-15],b=w[i-2];w[i]=(w[i-16]+(rotr(a,7)^rotr(a,18)^(a>>>3))+w[i-7]+(rotr(b,17)^rotr(b,19)^(b>>>10)))>>>0;}
 let [a,b,c,d,e,f,g,j]=h;
 for(let i=0;i<64;i++){const t1=(j+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i])>>>0;
 const t2=((rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c)))>>>0;
 j=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;}
 const values=[a,b,c,d,e,f,g,j];for(let i=0;i<8;i++)h[i]=(h[i]+values[i])>>>0;
 }
 return Array.from(h,n=>n.toString(16).padStart(8,'0')).join('');
}
async function fingerprint(file){
 const identity=new TextEncoder().encode(JSON.stringify([file.name,file.size,file.lastModified]));
 const first=new Uint8Array(await file.slice(0,65536).arrayBuffer());
 const last=new Uint8Array(await file.slice(Math.max(0,file.size-65536)).arrayBuffer());
 const bytes=new Uint8Array(identity.length+first.length+last.length);bytes.set(identity);bytes.set(first,identity.length);bytes.set(last,identity.length+first.length);
 return sha256(bytes);
}
const base64=s=>btoa(String.fromCharCode(...new TextEncoder().encode(s)));
function patch(url,offset,blob){return new Promise((resolve,reject)=>{
 const xhr=new XMLHttpRequest();currentXHR=xhr;xhr.open('PATCH',url);
 for(const [key,value]of Object.entries(headers({'Content-Type':'application/offset+octet-stream','Upload-Offset':String(offset)})))xhr.setRequestHeader(key,value);
 xhr.onload=()=>{currentXHR=null;if(xhr.status===204)resolve(Number(xhr.getResponseHeader('Upload-Offset')));else{let error='interrupted';try{error=JSON.parse(xhr.responseText).error||error;}catch{}reject(Error(xhr.status===401?'lost':error));}};
 xhr.onerror=()=>{currentXHR=null;reject(Error('interrupted'));};xhr.onabort=()=>{currentXHR=null;reject(Error('paused'));};xhr.send(blob);
});}
async function sendFile(file,name){
 const signature=await fingerprint(file);if(stopped)throw Error('paused');
 await refresh();let s=state.transfer;
 if(s.id&&s.phase!=='complete'){
 if(s.fingerprint!==signature||s.length!==file.size)throw Error('same');
 currentId=s.id;
 if(s.name!==name)await api('/api/v1/uploads/'+currentId+'/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name})});
 }else{
 const created=await api('/api/v1/uploads',{method:'POST',headers:{'Upload-Length':String(file.size),'Upload-Metadata':'filename '+base64(name)+',fingerprint '+base64(signature)}});
 currentId=created.headers.get('Location').split('/').pop();
 }
 const url='/api/v1/uploads/'+currentId;
 let offset=Number((await api(url,{method:'HEAD'})).headers.get('Upload-Offset')),attempts=0;
 while(offset<file.size){
 if(stopped)throw Error('paused');
 const start=performance.now(),before=offset,blob=file.slice(offset,Math.min(file.size,offset+chunk));
 try{
 offset=await patch(url,offset,blob);
 if(!Number.isSafeInteger(offset)||offset<=before||offset>file.size)throw Error('offset');
 const elapsed=Math.max(.001,(performance.now()-start)/1000);rates.push({bytes:offset-before,time:elapsed});if(rates.length>8)rates.shift();
 const speed=rates.reduce((a,x)=>a+x.bytes,0)/rates.reduce((a,x)=>a+x.time,0);
 $('rate').textContent=words.speed+': '+mib(speed)+' MiB/s';$('eta').textContent=words.eta+': '+Math.ceil((file.size-offset)/speed)+' s';attempts=0;
 }catch(error){
 if(stopped||error.message==='paused'||error.message==='lost'||!['interrupted','offset'].includes(error.message)||attempts++>=5)throw error;
 await delay(Math.min(3000,250*2**attempts));if(stopped)throw Error('paused');
 offset=Number((await api(url,{method:'HEAD'})).headers.get('Upload-Offset'));
 }
 }
 if(stopped)throw Error('paused');await api(url+'/finish',{method:'POST'});
 for(;;){await delay(500);await refresh();s=state.transfer;if(s.phase==='complete')return;if(s.error)throw Error(s.error);if(stopped||s.phase==='paused')throw Error('paused');}
}
$('files').addEventListener('change',()=>{if($('files').files.length)$('name').value=$('files').files[0].name;});
$('send').addEventListener('click',async()=>{
 if(running)return;const files=Array.from($('files').files);if(!files.length){$('message').textContent=words.choose;return;}
 running=true;stopped=false;rates=[];$('message').textContent='';render();
 try{for(let i=0;i<files.length;i++){if(stopped)break;$('queue').textContent=words.queue+': '+(files.length-i-1);await sendFile(files[i],files.length===1?$('name').value:files[i].name);}}
 catch(error){$('message').textContent=reason(error);}finally{running=false;currentXHR=null;try{await refresh();}catch{}render();}
});
async function control(action){
 stopped=true;if(currentXHR)currentXHR.abort();$('message').textContent='';
 try{await refresh();const id=state.transfer.id;if(!id)return;
 for(let retry=0;;retry++)try{await api('/api/v1/uploads/'+id+(action==='pause'?'/pause':''),{method:action==='pause'?'POST':'DELETE'});break;}
 catch(error){if(error.message!=='busy'||retry>=15)throw error;await delay(100);}
 await refresh();}catch(error){$('message').textContent=reason(error);}
}
$('pause').addEventListener('click',()=>control('pause'));$('discard').addEventListener('click',()=>control('discard'));
$('rename').addEventListener('click',async()=>{try{await api('/api/v1/uploads/'+state.transfer.id+'/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:$('name').value})});$('message').textContent='';await refresh();}catch(error){$('message').textContent=reason(error);}});
</script></html>)HTML";
}
