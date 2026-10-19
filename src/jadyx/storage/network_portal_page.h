// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
namespace Jadyx::PortalWeb {
inline constexpr std::string_view Page=R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Jadyx · Import</title><style>
.importcards{display:grid;grid-template-columns:1fr 1fr;gap:24px}.review{white-space:pre-line;border:1px solid #666;padding:20px;border-radius:12px}input[type=checkbox]{width:auto}@media(max-width:650px){.importcards{grid-template-columns:1fr}}
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
<section id="system-upload" hidden><div class="importcards"><article><h2 id="keys-title">Import keys</h2><p id="keys-help">ZIP or prod.keys / title.keys.</p>
<input id="keys-files" type="file" accept=".keys,.zip" multiple><button id="keys-send">Upload and review</button></article>
<article><h2 id="firmware-title">Import firmware</h2><p id="firmware-help">ZIP or multiple NCA files.</p>
<input id="firmware-files" type="file" accept=".zip,.nca" multiple><label id="firmware-folder-label" for="firmware-folder">Or choose an extracted folder</label>
<input id="firmware-folder" type="file" webkitdirectory multiple><button id="firmware-send">Upload and review</button></article></div>
<p id="system-job" role="status" aria-live="polite"></p><progress id="system-progress" max="1" value="0"></progress>
<div id="system-review" class="review" hidden><p id="system-versions"></p><p id="system-warnings"></p>
<label id="system-warning-row"><input id="system-warning-accept" type="checkbox"><span id="system-warning-label">I understand and accept these warnings.</span></label>
<button id="system-confirm">Confirm installation</button></div><button id="system-cancel" class="secondary">Cancel and discard system import</button></section>
<p id="message" role="alert"></p><p id="privacy">Local network · HTTP connection without encryption.</p>
<script>
'use strict';
const $=id=>document.getElementById(id),lang=navigator.language.toLowerCase(),es=lang.startsWith('es'),pt=lang.startsWith('pt');
const words=pt?{
 title:'Conecte-se ao seu console',intro:'Digite o código temporário que o Jadyx mostra. Mantenha o app aberto na biblioteca.',label:'Código de conexão',connect:'Conectar',
 uploadtitle:'Importar jogos NSP',uploadintro:'Somente jogos base. Os arquivos são copiados para o armazenamento interno do console.',fileslabel:'Selecionar arquivos NSP',
 namelabel:'Nome de destino (um único arquivo)',send:'Enviar / retomar',rename:'Renomear arquivo pendente',pause:'Pausar',discard:'Apagar arquivo parcial',
 privacy:'Rede local · Conexão HTTP sem criptografia.',ok:'Conexão estabelecida.',bad:'Código incorreto ou expirado. Gere outro no Jadyx.',
 lost:'A conexão foi encerrada. Conecte-se de novo pelo Jadyx; o progresso confirmado é mantido.',free:'Espaço livre',confirmed:'Confirmado',
 speed:'Velocidade',eta:'Tempo restante',queue:'Na fila',choose:'Selecione um ou mais arquivos NSP.',same:'Há outro arquivo pendente. Selecione o mesmo arquivo para retomá-lo ou apague a transferência parcial.',
 receiving:'Recebendo',paused:'Pausado. Selecione o mesmo arquivo para continuar.',ready:'Aguardando validação',validating:'Validando NSP',publishing:'Adicionando à biblioteca',complete:'Instalado na biblioteca',
 busy:'Aguarde o fim da operação no Jadyx.',exists:'Esse nome já existe. Altere o nome de destino e tente de novo.',space:'Não há espaço suficiente.',
 file:'Nome ou arquivo inválido. Selecione um NSP.',base:'Somente jogos base são aceitos; não atualizações, DLC nem pacotes mistos.',
 nsp:'A estrutura do NSP é inválida.',incomplete:'O NSP está incompleto.',keys:'Não foi possível ler os metadados. Verifique as chaves do seu console e o arquivo NSP.',
 disk:'Não foi possível gravar ou publicar o arquivo.',interrupted:'Transferência interrompida. Você pode retomar a partir do progresso confirmado.',
 partial:'Há uma transferência pendente. Retome ou apague.',recovery:'Não foi possível recuperar o arquivo parcial. Apague-o antes de iniciar outro.',unavailable:'A importação não está disponível no momento.',
 attempts:'Tentativas demais. Aguarde um minuto.',version:'Versão de transferência incompatível. Recarregue a página.',idle:'Pronto para importar.'
}:es?{
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
if(es||pt){document.documentElement.lang=pt?'pt-BR':'es';for(const id of ['title','intro','label','connect','uploadtitle','uploadintro','fileslabel','namelabel','send','rename','pause','discard','privacy'])$(id).textContent=words[id];}
const selectedChunk=Number(new URLSearchParams(location.search).get('chunk'));
const chunk=(selectedChunk===4||selectedChunk===16||selectedChunk===64?selectedChunk:16)*1048576;
let session='',timer,state={},running=false,stopped=false,currentXHR=null,currentId='',rates=[];
let systemRunning=false,systemReviewId='';
const delay=ms=>new Promise(r=>setTimeout(r,ms)),mib=n=>(n/1048576).toFixed(1),reason=e=>words[e.message]||words.interrupted;
function formatDuration(value){
 if(!Number.isFinite(value))return '—';
 const total=Math.max(0,Math.ceil(value)),hours=Math.floor(total/3600),minutes=Math.floor(total%3600/60),seconds=total%60;
 return (hours?hours+' h ':'')+(hours||minutes?minutes+' min ':'')+seconds+' s';
}
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
 $('send').disabled=running||systemRunning||state.system?.busy||!session||!state.uploads||s.phase==='ready'||s.phase==='validating'||s.phase==='publishing';
 $('pause').disabled=systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('discard').disabled=systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('rename').disabled=running||systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 if(s.error)$('message').textContent=words[s.error]||words.interrupted;
 renderSystem();
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
 $('rate').textContent=words.speed+': '+mib(speed)+' MiB/s';$('eta').textContent=words.eta+': '+formatDuration((file.size-offset)/speed);attempts=0;
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
 if(running||systemRunning||state.system?.busy)return;const files=Array.from($('files').files);if(!files.length){$('message').textContent=words.choose;return;}
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
const systemWords=pt?{
 keysTitle:'Importar chaves',firmware:'Importar firmware',keysHelp:'ZIP ou prod.keys / title.keys. Máximo de 4 MiB por arquivo de chaves.',
 firmwareHelp:'ZIP ou arquivos NCA extraídos. A versão será verificada antes de instalar.',folder:'Ou escolha uma pasta extraída',send:'Enviar e revisar',confirm:'Confirmar instalação',cancel:'Cancelar e apagar importação',accept:'Entendo e aceito os avisos.',
 installed:'Instalado',selected:'Selecionado',generation:'Geração das chaves (índice)',required:'Geração exigida pelo firmware',ready:'Arquivos validados. Revise as versões e confirme.',
 firmware_downgrade:'O firmware selecionado é anterior ao instalado.',keys_downgrade:'As novas chaves perdem gerações disponíveis nas chaves instaladas.',keys_firmware_old:'As chaves podem não abrir o firmware instalado. Continuar pode deixá-lo inutilizável.',keys_unknown:'Não foi possível verificar a geração das chaves.',installed_unknown:'Não foi possível verificar o firmware instalado.',
 receiving:'Recebendo arquivos',extracting:'Extraindo ZIP',validating:'Validando arquivos',prepare_pending:'Aguardando validação',commit_pending:'Aguardando instalação',publishing:'Instalando',complete:'Instalação concluída',failed:'A importação falhou; os dados anteriores foram preservados.',idle:'Escolha arquivos para importar.',
 archive:'O ZIP está danificado ou não é compatível.',archive_unsafe:'O ZIP contém caminhos perigosos, links ou arquivos criptografados.',archive_limit:'O arquivo ultrapassa os limites de tamanho ou quantidade.',file_limit:'Quantidade de arquivos inválida.',file:'Use somente ZIP ou os arquivos correspondentes, sem misturá-los.',keys:'Arquivo de chaves inválido.',keys_header:'Falta uma header_key válida em prod.keys.',keys_limit:'Cada arquivo de chaves deve ter no máximo 4 MiB.',firmware_keys:'As chaves instaladas não conseguem ler o firmware. Importe chaves compatíveis primeiro.',firmware_incomplete:'Firmware incompleto: falta SystemVersion ou MiiEdit.',firmware_game:'Este pacote contém arquivos de jogo.',firmware_duplicate:'O pacote contém nomes ou versões duplicados.',firmware_version:'Não foi possível verificar uma versão de firmware NX válida.',confirm_warnings:'Aceite os avisos antes de instalar.',recovery:'A importação interrompida não pôde ser recuperada. Apague-a e selecione os arquivos novamente.'
}:es?{
 keysTitle:'Importar claves',firmware:'Importar firmware',keysHelp:'ZIP o prod.keys / title.keys. Máximo de 4 MiB por archivo de claves.',
 firmwareHelp:'ZIP o archivos NCA extraídos. La versión se comprobará antes de instalar.',folder:'O elige una carpeta extraída',send:'Subir y revisar',confirm:'Confirmar instalación',cancel:'Cancelar y eliminar importación',accept:'Entiendo y acepto las advertencias.',
 installed:'Instalado',selected:'Seleccionado',generation:'Generación de claves (índice)',required:'Generación requerida por el firmware',ready:'Archivos validados. Revisa las versiones y confirma.',
 firmware_downgrade:'El firmware seleccionado es anterior al instalado.',keys_downgrade:'Las nuevas claves pierden generaciones disponibles en las instaladas.',keys_firmware_old:'Las claves podrían no abrir el firmware instalado. Continuar puede dejarlo inutilizable.',keys_unknown:'No se pudo verificar la generación de las claves.',installed_unknown:'No se pudo verificar el firmware instalado.',
 receiving:'Recibiendo archivos',extracting:'Extrayendo ZIP',validating:'Validando archivos',prepare_pending:'Esperando validación',commit_pending:'Esperando instalación',publishing:'Instalando',complete:'Instalación completada',failed:'La importación falló; se conservaron los datos anteriores.',idle:'Selecciona archivos para importar.',
 archive:'El ZIP está dañado o no es compatible.',archive_unsafe:'El ZIP contiene rutas peligrosas, enlaces o archivos cifrados.',archive_limit:'El archivo supera los límites de tamaño o cantidad.',file_limit:'Cantidad de archivos no válida.',file:'Usa solo ZIP o los archivos correspondientes, sin mezclarlos.',keys:'Archivo de claves no válido.',keys_header:'Falta una header_key válida en prod.keys.',keys_limit:'Cada archivo de claves debe ocupar como máximo 4 MiB.',firmware_keys:'Las claves instaladas no pueden leer el firmware. Importa antes claves compatibles.',firmware_incomplete:'Firmware incompleto: falta SystemVersion o MiiEdit.',firmware_game:'El paquete contiene archivos de juego.',firmware_duplicate:'El paquete contiene nombres o versiones duplicados.',firmware_version:'No se pudo verificar una versión de firmware NX válida.',confirm_warnings:'Acepta las advertencias antes de instalar.',recovery:'No se pudo recuperar la importación interrumpida. Elimínala y vuelve a seleccionar los archivos.'
}:{
 keysTitle:'Import keys',firmware:'Import firmware',keysHelp:'ZIP or prod.keys / title.keys. Maximum 4 MiB per key file.',firmwareHelp:'ZIP or extracted NCA files. The version is checked before installation.',folder:'Or choose an extracted folder',send:'Upload and review',confirm:'Confirm installation',cancel:'Cancel and discard import',accept:'I understand and accept these warnings.',
 installed:'Installed',selected:'Selected',generation:'Key generation (index)',required:'Firmware required generation',ready:'Files validated. Review versions and confirm.',firmware_downgrade:'The selected firmware is older than the installed firmware.',keys_downgrade:'The new keys lose generations present in the installed keys.',keys_firmware_old:'These keys may not open the installed firmware. Continuing may make it unusable.',keys_unknown:'The key generation could not be verified.',installed_unknown:'The installed firmware could not be verified.',
 receiving:'Receiving files',extracting:'Extracting ZIP',validating:'Validating files',prepare_pending:'Waiting for validation',commit_pending:'Waiting for installation',publishing:'Installing',complete:'Installation complete',failed:'Import failed; previous data was preserved.',idle:'Choose files to import.',archive:'The ZIP is damaged or unsupported.',archive_unsafe:'The ZIP contains unsafe paths, links or encrypted entries.',archive_limit:'The size or file count limit was exceeded.',file_limit:'Invalid file count.',file:'Use only a ZIP or the appropriate loose files, without mixing formats.',keys:'Invalid key file.',keys_header:'A valid header_key is required in prod.keys.',keys_limit:'Each key file must be at most 4 MiB.',firmware_keys:'Installed keys cannot read this firmware. Import compatible keys first.',firmware_incomplete:'Incomplete firmware: SystemVersion or MiiEdit is missing.',firmware_game:'The package contains game files.',firmware_duplicate:'The package contains duplicate names or versions.',firmware_version:'A valid NX firmware version could not be verified.',confirm_warnings:'Accept warnings before installing.',recovery:'The interrupted import could not be recovered. Discard it and select the files again.'};
for(const [id,text]of Object.entries({'keys-title':systemWords.keysTitle,'firmware-title':systemWords.firmware,'keys-help':systemWords.keysHelp,'firmware-help':systemWords.firmwareHelp,
 'firmware-folder-label':systemWords.folder,'keys-send':systemWords.send,'firmware-send':systemWords.send,'system-confirm':systemWords.confirm,'system-cancel':systemWords.cancel,'system-warning-label':systemWords.accept}))$(id).textContent=text;
function systemReason(error){return systemWords[error.message]||words[error.message]||systemWords.failed;}
function renderSystem(){
 const s=state.system||{},r=s.review||{},warnings=r.warnings||[];
 $('system-upload').hidden=!session||!state.system_supported;
 if(!state.system_supported)return;
 for(const kind of ['keys','firmware'])$(kind+'-send').disabled=running||systemRunning||state.transfer?.busy||!s.available||(s.busy&&s.kind!==kind)||!['idle','receiving','complete'].includes(s.phase);
 $('system-job').textContent=(systemWords[s.phase]||systemWords.idle)+(s.count?' · '+(s.completed||0)+' / '+s.count:'')+(s.error?'\n'+systemReason(Error(s.error)):'');
 const f=s.file||{};$('system-progress').value=f.length?Math.min(1,(f.received||0)/f.length):0;
 $('system-review').hidden=s.phase!=='review';
 if(s.phase!=='review')systemReviewId='';
 if(s.phase==='review'){
  if(systemReviewId!==s.id){systemReviewId=s.id;$('system-warning-accept').checked=false;}
  const gen=value=>value>=0?'0x'+value.toString(16).padStart(2,'0'):'—';
  $('system-versions').textContent=systemWords.installed+': '+(r.installed_firmware||'—')+'\n'+systemWords.selected+': '+(r.incoming_firmware||'—')+
    '\n'+systemWords.generation+': '+gen(r.installed_generation)+' → '+gen(r.incoming_generation)+'\n'+systemWords.required+': '+gen(r.required_generation);
  $('system-warnings').textContent=warnings.map(w=>systemWords[w]||systemWords.keys_unknown).join('\n');
  $('system-warning-row').hidden=!warnings.length;$('system-confirm').disabled=!!warnings.length&&!$('system-warning-accept').checked;
 }
 $('system-cancel').disabled=!s.id||s.phase==='complete'||s.phase==='publishing'||s.phase==='commit_pending';
}
async function sendSystem(kind){
 if(running||systemRunning)return;
 const folder=kind==='firmware'?Array.from($('firmware-folder').files):[];
 const files=(folder.length?folder:Array.from($(kind+'-files').files)).filter(f=>!folder.length||f.name.toLowerCase().endsWith('.nca'));
 if(!files.length){$('message').textContent=words.choose;return;}
 const zip=files.length===1&&files[0].name.toLowerCase().endsWith('.zip'),format=zip?'zip':'files';
 if(!zip&&files.some(f=>kind==='keys'?!['prod.keys','title.keys'].includes(f.name.toLowerCase()):!f.name.toLowerCase().endsWith('.nca'))){$('message').textContent=systemWords.file;return;}
 if(new Set(files.map(f=>f.name.toLowerCase())).size!==files.length){$('message').textContent=systemWords.firmware_duplicate;return;}
 systemRunning=true;stopped=false;$('message').textContent='';render();
 try{
  await refresh();let s=state.system;
  if(!s.id||s.phase==='complete'){
   await api('/api/v1/system/jobs',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({kind,format,count:files.length})});await refresh();s=state.system;
  }
  if(s.kind!==kind||s.format!==format||s.count!==files.length||s.phase!=='receiving')throw Error('same');
  const job='/api/v1/system/jobs/'+s.id;
  for(const file of files){
   if(stopped)throw Error('paused');const signature=await fingerprint(file);await refresh();s=state.system;
   const done=(s.files||[]).find(f=>f.name===file.name.toLowerCase());
   if(done){if(done.length!==file.size||done.fingerprint!==signature)throw Error('same');continue;}
   let f=s.file||{},url;
   if(f.id&&f.phase!=='complete'){
    if(f.name!==file.name.toLowerCase()||f.fingerprint!==signature||f.length!==file.size)throw Error('same');
    url='/api/v1/system/files/'+f.id;
   }else{
    const created=await api(job+'/files',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:file.name.toLowerCase(),length:file.size,fingerprint:signature})});
    url=created.headers.get('Location');
   }
   let offset=Number((await api(url,{method:'HEAD'})).headers.get('Upload-Offset')),attempts=0;
   while(offset<file.size){
    if(stopped)throw Error('paused');const before=offset;
    try{offset=await patch(url,offset,file.slice(offset,Math.min(file.size,offset+chunk)));
     if(!Number.isSafeInteger(offset)||offset<=before||offset>file.size)throw Error('offset');attempts=0;
    }catch(error){if(stopped||!['interrupted','offset'].includes(error.message)||attempts++>=5)throw error;
     await delay(Math.min(3000,250*2**attempts));offset=Number((await api(url,{method:'HEAD'})).headers.get('Upload-Offset'));}
   }
   await api(url+'/finish',{method:'POST'});
  }
  await api(job+'/prepare',{method:'POST'});await refresh();
 }catch(error){$('message').textContent=systemReason(error);}finally{systemRunning=false;currentXHR=null;try{await refresh();}catch{}render();}
}
$('keys-send').addEventListener('click',()=>sendSystem('keys'));$('firmware-send').addEventListener('click',()=>sendSystem('firmware'));
$('firmware-files').addEventListener('change',()=>{$('firmware-folder').value='';});$('firmware-folder').addEventListener('change',()=>{$('firmware-files').value='';});
$('system-warning-accept').addEventListener('change',renderSystem);
$('system-confirm').addEventListener('click',async()=>{try{
 await api('/api/v1/system/jobs/'+state.system.id+'/confirm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({accept_warnings:$('system-warning-accept').checked})});await refresh();
 }catch(error){$('message').textContent=systemReason(error);}});
$('system-cancel').addEventListener('click',async()=>{
 stopped=true;if(currentXHR)currentXHR.abort();
 try{for(let retry=0;;retry++)try{await api('/api/v1/system/jobs/'+state.system.id,{method:'DELETE'});break;}
 catch(error){if(error.message!=='busy'||retry>=30)throw error;await delay(100);}
 systemReviewId='';$('message').textContent='';await refresh();}catch(error){$('message').textContent=systemReason(error);}
});
</script></html>)HTML";
}
