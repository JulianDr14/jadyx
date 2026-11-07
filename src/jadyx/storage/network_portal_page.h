// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
namespace Jadyx::PortalWeb {
// The page is laid out like the console frontend: the wordmark and segmented tabs in the header,
// one tab for each kind of thing the player manages. What exists comes first and adding to it
// second, and upload controls appear only while there is an upload. Games open into their
// updates/DLC/save. Palette, Inter and capsule buttons follow
// Jadyx::Palette; the only colour is the logo's. The CSP allows images from the portal itself
// (/logo.png) and nothing external, so icons are inline SVG.
inline constexpr std::string_view Page=R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#08090b"><link rel="icon" href="/logo.png"><title>Jadyx Portal</title><style>
:root{color-scheme:dark;--canvas:#08090b;--surface:#111315;--row:#16181b;--graphite:#1c1f23;--active:#26282b;--text:#f5f5f7;--muted:#92959a;--hint:#b4b7bb;
 --warn:#f0c27a;--cyan:#2eb5e5;--crimson:#e02b41;--line:rgba(255,255,255,.08);--field:#2a2d31;
 font-family:Inter,"Segoe UI Variable Text","Segoe UI",system-ui,-apple-system,Roboto,sans-serif;font-size:15px;line-height:22px;-webkit-font-smoothing:antialiased}
*{box-sizing:border-box}button,input{font-family:inherit}[hidden]{display:none!important}
body{margin:0;min-height:100vh;display:flex;flex-direction:column;color:var(--text);background:var(--canvas);
 background-image:radial-gradient(1100px 520px at 50% -18%,rgba(255,255,255,.07),transparent 70%);background-repeat:no-repeat}
h1,h2,h3,p{margin:0}h1{font-size:34px;line-height:42px;font-weight:600;letter-spacing:-.02em}
h2{font-size:24px;line-height:30px;font-weight:600;letter-spacing:-.01em}h3{font-size:16px;line-height:22px;font-weight:600}
p,small{color:var(--muted)}svg{flex:none}
.ico{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:1.75;stroke-linecap:round;stroke-linejoin:round}
.bar{position:sticky;top:0;z-index:10;display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:16px;min-height:76px;padding:0 40px;
 background:rgba(8,9,11,.8);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);border-bottom:1px solid var(--line)}
.brand{display:flex;align-items:center;gap:12px;min-width:0}.brand img{width:44px;height:44px}
.word{font-size:19px;font-weight:600;letter-spacing:.42em;white-space:nowrap}
.caps{font-size:11px;font-weight:600;letter-spacing:.16em;text-transform:uppercase;color:var(--muted);padding-left:12px;border-left:1px solid rgba(255,255,255,.18)}
.tabs{display:none;padding:4px;border-radius:999px;background:rgba(28,31,35,.78);border:1px solid rgba(255,255,255,.12)}body:has(#pair[hidden]) .tabs{display:flex}
.tabs button{height:36px;padding:0 24px;border:0;background:none;color:var(--muted)}.tabs button:hover:not(:disabled){background:none;color:var(--text)}
.tabs button[aria-selected=true],.tabs button[aria-selected=true]:hover{background:var(--text);color:var(--canvas)}
.link{justify-self:end;display:inline-flex;align-items:center;gap:8px;height:34px;padding:0 14px;border-radius:999px;background:rgba(255,255,255,.05);border:1px solid var(--line);
 font-size:13px;font-weight:600;color:var(--hint);white-space:nowrap}
.link i{width:8px;height:8px;border-radius:50%;background:var(--muted)}.status-on{display:none}
body:has(#pair[hidden]) .link i{background:var(--cyan);box-shadow:0 0 0 3px rgba(46,181,229,.22)}
body:has(#pair[hidden]) .status-off{display:none}body:has(#pair[hidden]) .status-on{display:inline}
.shell{flex:1;width:100%;max-width:1200px;margin:0 auto;padding:36px 40px 48px}
.kicker{font-size:12px;line-height:16px;font-weight:600;letter-spacing:.16em;text-transform:uppercase;color:var(--muted)}
#message{position:sticky;top:92px;z-index:5;margin-bottom:24px;padding:14px 16px;border-radius:14px;color:var(--warn);background:rgba(28,24,16,.94);
 border:1px solid rgba(240,194,122,.3);white-space:pre-wrap;overflow-wrap:anywhere}#message:empty{display:none}
.gate{max-width:500px;margin:4vh auto 0;text-align:center}
.gate img{width:132px;height:132px;filter:drop-shadow(0 18px 40px rgba(0,0,0,.6))}.gate .kicker{margin-top:20px}.gate h1{margin-top:10px}
#intro{font-size:17px;line-height:25px;margin:12px auto 0;max-width:26em}
#pair{margin-top:32px;padding:28px;border-radius:20px;background:var(--surface);border:1px solid var(--line);box-shadow:0 32px 64px -32px rgba(0,0,0,.7)}
#pair label{display:block;font-size:14px;font-weight:500;color:var(--hint);margin-bottom:12px}
#code{width:100%;height:76px;text-align:center;font-size:34px;font-weight:600;line-height:1;font-variant-numeric:tabular-nums;letter-spacing:.38em;text-indent:.38em;
 background:var(--canvas);color:var(--text);border:1px solid var(--field);border-radius:16px;transition:border-color .15s}
#code::placeholder{color:#3a3d42}#code:focus{border-color:var(--text);outline:none;box-shadow:0 0 0 6px rgba(255,255,255,.08)}
#connect{width:100%;height:52px;margin-top:14px}.hint{font-size:13px;margin-top:12px}
#connection{margin-top:16px;font-size:14px;color:var(--hint)}#connection:empty{display:none}
.app{display:none}body:has(#pair[hidden]) .app{display:block}body:has(#pair[hidden]) .gate{display:none}
.view{max-width:960px;margin:0 auto}
.head-row{display:flex;align-items:flex-start;justify-content:space-between;gap:20px;margin-bottom:24px}.head-row .head{margin:0}.head-row button{flex:none}
.items:empty{display:none}#amiibo-results{margin:16px 0 0}#amiibo-items{margin:0}
.amiibo-tools{display:flex;gap:10px;margin-bottom:12px}.amiibo-tools button{flex:none;height:48px}
#amiibo-search{flex:1;min-width:0;height:48px;padding:0 16px;font:inherit;background:var(--canvas);color:var(--text);border:1px solid var(--field);border-radius:12px}#amiibo-search:focus{outline:none;border-color:var(--text)}
#amiibo-count{font-size:13px;margin:0 0 12px;font-variant-numeric:tabular-nums}#amiibo-count:empty{display:none}#amiibo-key-status{margin-bottom:20px;color:var(--text)}
.actions:not(:has(>:not([hidden]))){display:none}
#amiibo-note{position:sticky;top:92px;z-index:5;margin-bottom:16px;padding:12px 14px;border-radius:12px;color:var(--cyan);background:var(--row);border:1px solid var(--line)}#amiibo-note:empty{display:none}
.tag{font-size:10px;line-height:20px;font-style:normal;font-weight:600;letter-spacing:.08em;text-transform:uppercase;padding:0 8px;border-radius:999px;border:1px solid var(--line);color:var(--muted);white-space:nowrap}
.store{display:flex;align-items:center;gap:14px;margin-top:16px;padding:14px 16px;border-radius:16px;background:var(--surface);border:1px solid var(--line);color:var(--hint)}
#space{font-size:14px;font-weight:600;color:var(--text);font-variant-numeric:tabular-nums;white-space:pre-line}#space:empty::before{content:"—"}
.card{background:var(--surface);border:1px solid var(--line);border-radius:20px;padding:32px;box-shadow:0 32px 64px -32px rgba(0,0,0,.7)}
.head{margin-bottom:24px}.head p{margin-top:6px;max-width:44em}
.drop{position:relative;display:flex;flex-direction:column;align-items:center;gap:6px;padding:40px 20px;border:1.5px dashed rgba(255,255,255,.16);border-radius:16px;
 background:rgba(255,255,255,.02);text-align:center;cursor:pointer;transition:border-color .15s,background .15s}
.drop .ico{width:30px;height:30px;color:var(--hint);margin-bottom:4px}.drop strong{font-weight:600;color:var(--text)}.drop span{font-size:13px;color:var(--muted)}
.drop .picked{margin-top:6px;color:var(--cyan);font-weight:600;overflow-wrap:anywhere}.drop .picked:empty{display:none}
.drop input,.folder input{position:absolute;inset:0;width:100%;height:100%;opacity:0;cursor:pointer}
.drop:hover,.drop.over{border-color:rgba(255,255,255,.5);background:rgba(255,255,255,.05)}
.drop:focus-within,.folder:focus-within{outline:2px solid var(--text);outline-offset:3px}
.drop:has(input:disabled){opacity:.45;cursor:default}.drop:has(input:disabled) input{cursor:default}
.field{margin-top:20px}.field label{display:block;font-size:14px;font-weight:500;color:var(--hint);margin-bottom:8px}
input#name{width:100%;height:48px;padding:0 16px;font:inherit;background:var(--canvas);color:var(--text);border:1px solid var(--field);border-radius:12px}
input#name:focus{outline:none;border-color:var(--text)}input#name:disabled{opacity:.5}
button{font-size:14px;font-weight:600;line-height:1;height:44px;padding:0 22px;border-radius:999px;border:1px solid var(--line);cursor:pointer;color:var(--text);background:var(--graphite);
 transition:background .15s,color .15s,transform .15s,box-shadow .15s,opacity .15s}
button:hover:not(:disabled){background:var(--active)}
button.primary{background:var(--text);color:var(--canvas);border-color:transparent}
button.primary:hover:not(:disabled){background:#fff;transform:translateY(-1px);box-shadow:0 10px 26px -10px rgba(255,255,255,.35)}
button.danger{color:#f59aa5}button:disabled{opacity:.4;cursor:default}
button:focus-visible,input:focus-visible{outline:2px solid var(--text);outline-offset:3px}
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:24px}
.panel{margin-top:24px;padding:20px;border-radius:16px;background:var(--row);border:1px solid var(--line)}
.panel-top{display:flex;align-items:baseline;justify-content:space-between;gap:16px;margin-bottom:12px}
#job,#system-job{color:var(--text);font-weight:500;white-space:pre-wrap;overflow-wrap:anywhere}
.pct{font-size:22px;font-weight:600;font-variant-numeric:tabular-nums;white-space:nowrap}
progress{display:block;width:100%;height:8px;border:0;border-radius:999px;overflow:hidden;background:var(--graphite);-webkit-appearance:none;appearance:none}
progress::-webkit-progress-bar{background:var(--graphite);border-radius:999px}
progress::-webkit-progress-value{background:linear-gradient(90deg,var(--cyan),var(--crimson));border-radius:999px;transition:width .3s}
progress::-moz-progress-bar{background:linear-gradient(90deg,var(--cyan),var(--crimson));border-radius:999px}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:16px;font-variant-numeric:tabular-nums}
.stats p{font-size:13px;color:var(--hint);padding:10px 12px;border-radius:10px;background:rgba(255,255,255,.03)}.stats p:empty{display:none}
.sub{display:flex;flex-direction:column;gap:16px}.sub p{margin-top:-10px}
.folder{position:relative;display:inline-flex;align-items:center;gap:8px;align-self:flex-start;font-size:13px;font-weight:600;color:var(--hint);
 padding:8px 14px;border-radius:999px;border:1px solid var(--line);cursor:pointer}.folder:hover{color:var(--text);border-color:rgba(255,255,255,.3)}.folder .ico{width:16px;height:16px}
.sub .primary{align-self:flex-start}
.review{margin-top:16px;padding:20px;border-radius:16px;background:var(--canvas);border:1px solid var(--line)}
#system-versions{white-space:pre-line;color:var(--text);font-variant-numeric:tabular-nums;line-height:26px}
#system-warnings{white-space:pre-line;margin-top:14px;padding:12px 14px;border-radius:12px;color:var(--warn);background:rgba(240,194,122,.08);border:1px solid rgba(240,194,122,.24)}
#system-warnings:empty{display:none}
#system-warning-row{display:flex;align-items:center;gap:10px;margin-top:14px;color:var(--text);cursor:pointer}
input[type=checkbox]{width:18px;height:18px;margin:0;accent-color:var(--text)}
#system-confirm,#system-cancel{margin-top:16px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:20px 18px}
button.tile{display:flex;flex-direction:column;align-items:stretch;gap:6px;height:auto;padding:0;border:0;border-radius:16px;background:none;text-align:left;font-weight:400}
button.tile:hover:not(:disabled){background:none}.tile:hover .art{transform:translateY(-2px);border-color:rgba(255,255,255,.32);box-shadow:0 18px 36px -18px rgba(0,0,0,.9)}
.art{display:grid;place-items:center;aspect-ratio:1;width:100%;margin-bottom:4px;border-radius:14px;border:1px solid var(--line);background:var(--row) center/cover no-repeat;
 font-size:34px;font-weight:600;color:var(--muted);transition:transform .15s,border-color .15s,box-shadow .15s}
.tile b{font-size:14px;line-height:19px;font-weight:600;overflow:hidden;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical}.tile small{font-size:12px;line-height:16px}
#library-empty{padding:40px 0;text-align:center}#library-note{margin:-8px 0 20px;color:var(--cyan);font-weight:500}#library-note:empty{display:none}
button.back{height:34px;padding:0 14px;margin-bottom:20px;font-size:13px;color:var(--hint)}
.hero{display:grid;grid-template-columns:96px minmax(0,1fr) auto;gap:20px;align-items:center;margin-bottom:8px}.hero .art{width:96px;margin:0}
.hero p{margin-top:4px}#game-warning{margin-bottom:24px;font-size:13px}
.seg{display:inline-flex;padding:4px;margin-bottom:16px;border-radius:999px;background:var(--row);border:1px solid var(--line)}
.seg button{height:34px;border:0;background:none;color:var(--muted)}.seg button:hover:not(:disabled){background:none;color:var(--text)}
.seg button[aria-selected=true],.seg button[aria-selected=true]:hover{background:var(--text);color:var(--canvas)}
.items{display:flex;flex-direction:column;gap:8px;margin:0 0 20px;padding:0;list-style:none}
.items li{display:flex;align-items:center;gap:14px;padding:12px 14px;border-radius:14px;background:var(--row);border:1px solid var(--line)}
.items li span{flex:1;min-width:0}.items b{display:block;font-weight:600;overflow-wrap:anywhere}.items small{display:block;font-size:13px}
.items li.empty{justify-content:center;color:var(--muted)}.items button{height:36px;padding:0 16px}
#save-intro,#save-warning{font-size:13px;margin:0 0 16px}#save-pane .drop{margin-top:20px}#save-warning{margin-top:12px}#logs-help{margin-top:6px;max-width:44em}
#save-error{margin:0 0 16px;padding:12px 14px;border-radius:12px;color:var(--warn);background:rgba(240,194,122,.08);border:1px solid rgba(240,194,122,.24);overflow-wrap:anywhere}
button.link{height:auto;padding:0;border:0;background:none;color:var(--cyan);text-decoration:underline;font-weight:500}button.link:hover:not(:disabled){background:none}
#save-help{margin:0 0 20px;padding:14px 16px;border-radius:12px;background:var(--row);border:1px solid var(--line)}#save-help.flash{border-color:var(--cyan)}
#save-help pre{margin:0 0 10px;font:13px/1.5 ui-monospace,Consolas,monospace;overflow-x:auto;color:var(--text)}#save-help p{font-size:13px;margin-top:6px}
footer{display:flex;align-items:center;justify-content:center;gap:8px;padding:24px 16px 32px;font-size:13px;color:var(--muted)}footer .ico{width:16px;height:16px}
body.updating .tabs{visibility:hidden}body.updating #view-content,body.updating #view-amiibo,body.updating #view-system{display:none!important}
#view-update{display:none;max-width:760px;margin:0 auto}body.updating #view-update{display:block}
.update-top{display:flex;align-items:flex-start;justify-content:space-between;gap:16px;margin-bottom:32px}.update-top h1{margin-top:8px}.update-top p{margin-top:6px}
.badge{display:inline-flex;align-items:center;gap:6px;height:30px;padding:0 12px;border-radius:999px;font-size:12px;font-weight:600;white-space:nowrap;
 color:var(--cyan);background:rgba(46,181,229,.1);border:1px solid rgba(46,181,229,.3)}.badge .ico{width:15px;height:15px;stroke-width:2.25}
.steps{display:flex;flex-direction:column;margin:0;padding:0;list-style:none}
.steps li{position:relative;display:grid;grid-template-columns:36px minmax(0,1fr);gap:18px;padding-bottom:28px}
.steps li:not(:last-child)::after{content:"";position:absolute;left:17px;top:46px;bottom:8px;width:2px;border-radius:1px;background:var(--graphite)}
.steps li.done::after{background:var(--text)}
.steps .n{display:grid;place-items:center;width:36px;height:36px;border-radius:50%;font-weight:600;background:var(--graphite);color:var(--muted);transition:background .2s,box-shadow .2s}
.steps li.done .n,.steps li.now .n{background:var(--text);color:var(--canvas)}.steps li.now .n{box-shadow:0 0 0 6px rgba(255,255,255,.1)}
.steps h3{margin-top:7px}.steps li:not(.done):not(.now) h3{color:var(--muted)}.steps p{margin-top:4px;max-width:36em}.steps .actions{margin-top:16px}
a.pill{display:inline-flex;align-items:center;gap:8px;height:44px;padding:0 20px;border-radius:999px;border:1px solid var(--line);font-size:14px;font-weight:600;
 color:var(--hint);text-decoration:none}a.pill:hover{color:var(--text);border-color:rgba(255,255,255,.3)}a.pill .ico{width:16px;height:16px}
.keep{padding:14px 16px;border-radius:14px;color:var(--warn);background:rgba(240,194,122,.08);border:1px solid rgba(240,194,122,.24)}
#update-skip{margin:24px 0 0;align-self:center}.update-foot{display:flex;justify-content:center}
.installing{padding:24px 0 8px;text-align:center}.installing p{max-width:30em;margin:10px auto 0}.installing button{margin-top:24px}
.spin{width:48px;height:48px;margin:0 auto 22px;border-radius:50%;border:3px solid var(--graphite);border-top-color:var(--cyan);animation:spin 1s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
#update-banner{max-width:960px;margin:0 auto 24px}#update-banner .panel-top{align-items:center;margin:0}#update-banner p{margin-top:4px}
@media(max-width:860px){.bar{grid-template-columns:minmax(0,1fr) auto;padding:12px 16px;row-gap:12px}.tabs{grid-column:1/-1}.tabs button{flex:1}
 .shell{padding:20px 16px 32px}
 #message,#amiibo-note{top:132px}.head-row{flex-direction:column}
 .grid{grid-template-columns:repeat(auto-fill,minmax(120px,1fr))}.hero{grid-template-columns:72px minmax(0,1fr)}.hero .art{width:72px}.hero .danger{grid-column:1/-1}
 .card{padding:22px}h1{font-size:28px;line-height:34px}.actions button{flex:1 1 calc(50% - 10px);height:auto;min-height:44px;padding:10px 16px;line-height:1.25}#code{font-size:28px;letter-spacing:.28em;text-indent:.28em}}
@media(max-width:600px){.update-top{flex-direction:column}.steps li{gap:14px}.steps .actions>*{flex:1 1 100%;justify-content:center}}
@media(max-width:420px){.brand img{width:36px;height:36px}.word{font-size:16px;letter-spacing:.3em}.caps,.link span{display:none}.link{padding:0 11px}}
@media(prefers-reduced-motion:reduce){*{transition:none!important}}
</style>
<body><header class="bar"><div class="brand"><img src="/logo.png" alt="Jadyx" width="44" height="44"><span class="word">JADYX</span><span class="caps">Portal</span></div>
<nav class="tabs" role="tablist"><button id="tab-games" role="tab" aria-selected="true">Games</button><button id="tab-amiibo" role="tab" aria-selected="false" hidden>Amiibo</button><button id="tab-system" role="tab" aria-selected="false">System</button></nav>
<span class="link" role="status"><i></i><span class="status-off" id="status-off">Not connected</span><span class="status-on" id="status-on">Connected</span></span></header>
<div class="shell"><p id="message" role="alert"></p>
<main class="gate"><img src="/logo.png" alt="" width="132" height="132"><p class="kicker" id="kicker">Local network portal</p><h1 id="title">Connect to your console</h1>
<p id="intro">Enter the temporary code shown in Jadyx. Keep the app open in the library.</p>
<form id="pair"><label id="label" for="code">Connection code</label>
<input id="code" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{4}" maxlength="4" placeholder="0000" required>
<button id="connect" class="primary">Connect</button><p class="hint" id="code-hint">4 digits · it changes every time you open the portal</p></form>
<p id="connection" role="status" aria-live="polite"></p></main>
<div class="app">
<section class="card" id="view-update" aria-live="polite"><div class="update-top"><div><p class="kicker" id="update-kicker">Jadyx update</p><h1 id="update-title">Update Jadyx</h1>
<p id="update-meta"></p></div><span class="badge" id="update-badge" hidden><svg class="ico" viewBox="0 0 24 24"><path d="M12 3 5 6v5c0 4.5 3 8.3 7 10 4-1.7 7-5.5 7-10V6z"/><path d="m9 12 2 2 4-4"/></svg><span id="update-verified">Signature verified</span></span></div>
<ol class="steps" id="update-steps">
<li class="done" id="update-step-1"><span class="n">1</span><div><h3 id="update-s1">Connect the browser</h3><p id="update-s1b">Done: this browser is connected to the console.</p></div></li>
<li class="now" id="update-step-2"><span class="n">2</span><div><h3 id="update-s2">Download and install</h3><p id="update-s2b">The package downloads and Xbox Device Portal opens in another tab.</p>
<div class="actions"><button class="primary" id="update-go" disabled>Download and install</button></div><a id="update-package" hidden></a></div></li>
<li id="update-step-3"><span class="n">3</span><div><h3 id="update-s3">Finish in Device Portal</h3><p id="update-s3b"></p>
<div class="actions"><a class="pill" id="update-portal" target="_blank" rel="noopener noreferrer"><svg class="ico" viewBox="0 0 24 24"><path d="M14 4h6v6M20 4l-9 9"/><path d="M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"/></svg><span id="update-portal-label">Open Device Portal</span></a></div></div></li>
</ol>
<div class="installing" id="update-installing" hidden><div class="spin"></div><h2 id="update-installing-title">Installing the update</h2><p id="update-installing-body"></p>
<button id="update-reconnect">Connect again</button></div>
<p class="keep" id="update-keep"></p><div class="update-foot"><button class="back" id="update-skip">Go to content</button></div></section>
<section class="panel" id="update-banner" hidden><div class="panel-top"><div><h3 id="update-banner-title"></h3><p id="update-banner-body"></p></div>
<button class="primary" id="update-banner-open"></button></div></section>
<div class="view" id="view-content">
<section id="upload" class="card" hidden><button class="back" id="upload-back">← Library</button><div class="head"><h2 id="uploadtitle">Import games</h2><p id="uploadintro">Base games only. Choose where your console keeps them.</p></div>
<nav class="seg" id="where" role="tablist" hidden><button id="where-internal" role="tab" aria-selected="true">Internal storage</button><button id="where-developer" role="tab" aria-selected="false">Dev storage (D:)</button></nav>
<label class="drop" id="files-zone"><input id="files" type="file" accept=".nsp,.xci,.nro" multiple><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="fileslabel">Choose NSP, XCI or NRO files</strong><span id="files-drop">or drop them here</span><span class="picked" id="files-picked"></span></label>
<div class="field" id="name-field" hidden><label id="namelabel" for="name">Destination name (one file)</label><input id="name" maxlength="100" autocomplete="off"></div>
<div class="actions"><button id="send" class="primary">Upload / resume</button><button id="pause">Pause</button><button id="rename">Rename pending file</button>
<button class="danger" id="discard">Discard partial file</button></div>
<div class="panel" id="upload-panel" hidden><div class="panel-top"><p id="job" role="status" aria-live="polite"></p><span class="pct" id="percent"></span></div>
<progress id="progress" max="1" value="0"></progress><div class="stats"><p id="confirmed"></p><p id="rate"></p><p id="eta"></p><p id="queue"></p></div></div></section>
<section id="library" class="card" hidden><p id="library-note" role="status" aria-live="polite"></p>
<div id="library-list"><div class="head-row"><div class="head"><h2 id="library-title">Your library</h2><p id="library-intro">Open a game to manage its updates, DLC and save, or to delete it.</p></div><button class="primary" id="library-add" hidden></button></div>
<div class="grid" id="games"></div><p id="library-empty" hidden>No games yet.</p></div>
<div id="game" hidden><button class="back" id="game-back">← Library</button>
<div class="hero"><div class="art" id="game-art"></div><div><h2 id="game-name"></h2><p id="game-meta"></p></div><button class="danger" id="game-remove">Delete game</button></div>
<p id="game-warning"></p>
<nav class="seg" role="tablist"><button id="kind-update" role="tab" aria-selected="true">Updates</button><button id="kind-dlc" role="tab" aria-selected="false">DLC</button><button id="kind-save" role="tab" aria-selected="false">Save</button></nav>
<ul class="items" id="items"></ul>
<label class="drop" id="content-zone"><input id="content-files" type="file" accept=".nsp,.xci" multiple><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="content-pick">Choose update NSP or XCI files</strong><span id="content-drop">or drop them here</span><span class="picked" id="content-picked"></span></label>
<div id="save-pane" hidden><p id="save-intro"></p><div class="actions"><button id="save-download" class="primary">Download save</button></div>
<label class="drop"><input id="save-file" type="file" accept=".zip,.7z,.rar"><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="save-pick">Choose the save (.zip, .7z or .rar)</strong><span id="save-drop">or drop it here</span><span class="picked" id="save-picked"></span></label>
<p id="save-warning"></p><div id="save-error" role="alert" hidden><span id="save-error-text"></span> <button class="link" id="save-error-help" type="button"></button></div>
<div class="actions"><button id="save-send" class="primary">Load save</button><button id="save-help-toggle" type="button"></button></div>
<div id="save-help" hidden><pre id="save-tree"></pre><p id="save-pc"></p><p id="save-rules"></p></div></div>
<div class="actions"><button id="content-send" class="primary">Upload and install</button><button id="content-pause">Pause</button><button class="danger" id="content-discard">Discard partial file</button></div>
<div class="panel" id="content-panel" hidden><div class="panel-top"><p id="content-job" role="status" aria-live="polite"></p><span class="pct" id="content-percent"></span></div>
<progress id="content-progress" max="1" value="0"></progress><div class="stats"><p id="content-rate"></p><p id="content-eta"></p></div></div></div></section>
<div class="store"><svg class="ico" viewBox="0 0 24 24"><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M5 13 7.5 5h9L19 13M7 16.5h.01"/></svg><span id="space"></span></div></div>
<div class="view" id="view-amiibo" hidden><nav class="seg" role="tablist"><button id="amiibo-tab-list" role="tab" aria-selected="true"></button><button id="amiibo-tab-add" role="tab" aria-selected="false"></button><button id="amiibo-tab-key" role="tab" aria-selected="false"></button></nav>
<section id="amiibo" class="card"><p id="amiibo-note" role="status" aria-live="polite"></p>
<div id="amiibo-pane-list"><div class="head"><h2 id="amiibo-list-title"></h2><p id="amiibo-intro"></p></div>
<div class="amiibo-tools" id="amiibo-tools"><input id="amiibo-search" type="search" autocomplete="off"><button id="amiibo-sort"></button></div>
<p id="amiibo-count"></p><ul class="items" id="amiibo-items"></ul></div>
<div id="amiibo-pane-add" hidden><div class="head"><h2 id="amiibo-add-title"></h2><p id="amiibo-add-help"></p></div>
<label class="drop"><input id="amiibo-files" type="file" accept=".bin" multiple><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="amiibo-pick"></strong><span id="amiibo-drop"></span><span class="picked" id="amiibo-picked"></span></label>
<div class="actions"><button id="amiibo-send" class="primary"></button></div><ul class="items" id="amiibo-results"></ul></div>
<div id="amiibo-pane-key" hidden><div class="head"><h2 id="amiibo-key-title"></h2><p id="amiibo-key-help"></p></div><p id="amiibo-key-status"></p>
<label class="drop"><input id="amiibo-key-file" type="file" accept=".bin"><svg class="ico" viewBox="0 0 24 24"><circle cx="8" cy="15" r="4"/><path d="M11 12l9-9M17 6l3 3M14 9l2 2"/></svg>
<strong id="amiibo-key-pick"></strong><span class="picked" id="amiibo-key-picked"></span></label>
<div class="actions"><button id="amiibo-key-send" class="primary"></button><button class="danger" id="amiibo-key-remove"></button></div></div></section></div>
<div class="view" id="view-system" hidden><nav class="seg" role="tablist"><button id="cat-keys" role="tab" aria-selected="true">Keys</button><button id="cat-firmware" role="tab" aria-selected="false">Firmware</button><button id="cat-logs" role="tab" aria-selected="false" hidden>Logs</button></nav>
<section id="system-upload" class="card" hidden><div class="head"><h2 id="system-title">Keys and firmware</h2><p id="system-intro">Everything is checked before installing. Your current files stay until you confirm.</p></div>
<article class="sub" id="keys-pane"><h3 id="keys-title">Import keys</h3><p id="keys-help">ZIP or prod.keys / title.keys.</p>
<label class="drop"><input id="keys-files" type="file" accept=".keys,.zip" multiple><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="keys-pick">Choose files</strong><span id="keys-drop">or drop them here</span><span class="picked" id="keys-picked"></span></label>
<button id="keys-send" class="primary">Upload and review</button></article>
<article class="sub" id="firmware-pane" hidden><h3 id="firmware-title">Import firmware</h3><p id="firmware-help">ZIP or multiple NCA files.</p>
<label class="drop"><input id="firmware-files" type="file" accept=".zip,.nca" multiple><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="firmware-pick">Choose files</strong><span id="firmware-drop">or drop them here</span><span class="picked" id="firmware-picked"></span></label>
<label class="folder"><input id="firmware-folder" type="file" webkitdirectory multiple><svg class="ico" viewBox="0 0 24 24"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg><span id="firmware-folder-label">Or choose an extracted folder</span></label>
<button id="firmware-send" class="primary">Upload and review</button></article>
<div class="panel" id="system-panel" hidden><div class="panel-top"><p id="system-job" role="status" aria-live="polite"></p><span class="pct" id="system-percent"></span></div><progress id="system-progress" max="1" value="0"></progress>
<div id="system-review" class="review" hidden><p id="system-versions"></p><p id="system-warnings"></p>
<label id="system-warning-row"><input id="system-warning-accept" type="checkbox"><span id="system-warning-label">I understand and accept these warnings.</span></label>
<button id="system-confirm" class="primary">Confirm installation</button></div></div><button id="system-cancel" class="danger">Cancel and discard system import</button></section>
<section class="card" id="log-export" hidden>
<h2 id="logs-title"></h2><p id="logs-help"></p>
<div class="actions"><button id="logs-download" class="primary"></button><button id="logs-cancel" hidden></button>
<a id="logs-file" hidden></a></div><progress id="logs-progress" max="1" value="0" hidden></progress>
<p id="logs-status" role="status" aria-live="polite"></p>
</section></div>
</div>
</div>
<footer><svg class="ico" viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 7.5-2"/></svg><span id="privacy">Local network · HTTP connection without encryption.</span></footer>
<script>
'use strict';
const $=id=>document.getElementById(id),lang=navigator.language.toLowerCase(),es=lang.startsWith('es'),pt=lang.startsWith('pt');
const words=pt?{
 title:'Conecte-se ao seu console',intro:'Digite o código temporário que o Jadyx mostra. Mantenha o app aberto na biblioteca.',label:'Código de conexão',connect:'Conectar',
 uploadtitle:'Importar jogos',uploadintro:'Somente jogos base. Escolha onde o console os guarda.','where-internal':'Armazenamento interno','where-developer':'Armazenamento dev (D:)',fileslabel:'Selecionar arquivos NSP, XCI ou NRO',
 namelabel:'Nome de destino (um único arquivo)',send:'Enviar / retomar',rename:'Renomear arquivo pendente',pause:'Pausar',discard:'Apagar arquivo parcial',
 kicker:'Portal da rede local','status-off':'Desconectado','status-on':'Conectado','code-hint':'4 dígitos · muda cada vez que você abre o portal','files-drop':'ou arraste-os para cá',files:'arquivos',
 privacy:'Rede local · Conexão HTTP sem criptografia.',ok:'Conexão estabelecida.',bad:'Código incorreto ou expirado. Gere outro no Jadyx.',
 lost:'A conexão foi encerrada. Conecte-se de novo pelo Jadyx; o progresso confirmado é mantido.',free:'Espaço livre',confirmed:'Confirmado',
 speed:'Velocidade',eta:'Tempo restante',queue:'Na fila',choose:'Selecione um ou mais arquivos NSP, XCI ou NRO.',same:'Há outro arquivo pendente. Selecione o mesmo arquivo para retomá-lo ou apague a transferência parcial.',
 receiving:'Recebendo',paused:'Pausado. Selecione o mesmo arquivo para continuar.',ready:'Aguardando validação',validating:'Validando arquivo',publishing:'Adicionando à biblioteca',complete:'Instalado na biblioteca',
 busy:'Aguarde o fim da operação no Jadyx.',exists:'Esse nome já existe. Altere o nome de destino e tente de novo.',space:'Não há espaço suficiente.',
 file:'Nome ou arquivo inválido. Selecione um NSP, XCI ou NRO.',base:'Somente jogos base são aceitos; não atualizações, DLC nem pacotes mistos.',
 nsp:'A estrutura do arquivo é inválida.',incomplete:'O arquivo está incompleto.',keys:'Não foi possível ler os metadados. Verifique as chaves do seu console e o arquivo.',
 disk:'Não foi possível gravar ou publicar o arquivo.',interrupted:'Transferência interrompida. Você pode retomar a partir do progresso confirmado.',
 partial:'Há uma transferência pendente. Retome ou apague.',recovery:'Não foi possível recuperar o arquivo parcial. Apague-o antes de iniciar outro.',unavailable:'A importação não está disponível no momento.',destination:'O armazenamento dev (D:) não está disponível. Escolha o armazenamento interno.',
 attempts:'Tentativas demais. Aguarde um minuto.',version:'Versão de transferência incompatível. Recarregue a página.',idle:'Pronto para importar.'
}:es?{
 title:'Conecta con tu consola',intro:'Introduce el código temporal que muestra Jadyx. Mantén la app abierta en la biblioteca.',label:'Código de conexión',connect:'Conectar',
 uploadtitle:'Importar juegos',uploadintro:'Solo juegos base. Elige dónde los guarda la consola.','where-internal':'Almacenamiento interno','where-developer':'Almacenamiento dev (D:)',fileslabel:'Seleccionar archivos NSP, XCI o NRO',
 namelabel:'Nombre de destino (un solo archivo)',send:'Subir / reanudar',rename:'Renombrar archivo pendiente',pause:'Pausar',discard:'Eliminar archivo parcial',
 kicker:'Portal de red local','status-off':'Sin conectar','status-on':'Conectado','code-hint':'4 dígitos · cambia cada vez que abres el portal','files-drop':'o arrástralos aquí',files:'archivos',
 privacy:'Red local · Conexión HTTP sin cifrado.',ok:'Conexión establecida.',bad:'Código incorrecto o caducado. Genera otro en Jadyx.',
 lost:'La conexión se ha cerrado. Vuelve a conectarte desde Jadyx; el progreso confirmado se conserva.',free:'Espacio libre',confirmed:'Confirmado',
 speed:'Velocidad',eta:'Tiempo restante',queue:'En cola',choose:'Selecciona uno o varios archivos NSP, XCI o NRO.',same:'Hay un archivo pendiente distinto. Selecciona el mismo archivo para reanudarlo o elimina la transferencia parcial.',
 receiving:'Recibiendo',paused:'En pausa. Selecciona el mismo archivo para continuar.',ready:'Esperando a validar',validating:'Validando archivo',publishing:'Añadiendo a la biblioteca',complete:'Instalado en la biblioteca',
 busy:'Espera a que termine la operación en Jadyx.',exists:'Ese nombre ya existe. Cambia el nombre de destino y vuelve a intentarlo.',space:'No hay espacio suficiente.',
 file:'Nombre o archivo no válido. Selecciona un NSP, XCI o NRO.',base:'Solo se admiten juegos base; no actualizaciones, DLC ni paquetes mezclados.',
 nsp:'La estructura del archivo no es válida.',incomplete:'El archivo está incompleto.',keys:'No se pueden leer sus metadatos. Revisa las claves de tu consola y el archivo.',
 disk:'No se pudo escribir o publicar el archivo.',interrupted:'Transferencia interrumpida. Puedes reanudar desde el progreso confirmado.',
 partial:'Hay una transferencia pendiente. Reanúdala o elimínala.',recovery:'No se pudo recuperar el parcial. Elimínalo antes de iniciar otro.',unavailable:'La importación no está disponible en este momento.',destination:'El almacenamiento dev (D:) no está disponible. Elige el almacenamiento interno.',
 attempts:'Demasiados intentos. Espera un minuto.',version:'Versión de transferencia incompatible. Recarga la página.',idle:'Listo para importar.'
}:{ok:'Connected.',bad:'Incorrect or expired code. Generate another in Jadyx.',lost:'Connection closed. Reconnect in Jadyx; confirmed progress is retained.',
 free:'Free space','where-internal':'Internal storage','where-developer':'Dev storage (D:)',confirmed:'Confirmed',speed:'Speed',eta:'Time left',queue:'Queued',choose:'Choose one or more NSP, XCI or NRO files.',files:'files',
 same:'A different file is pending. Select the same file to resume, or discard the partial upload.',receiving:'Receiving',paused:'Paused. Select the same file to continue.',
 ready:'Waiting for validation',validating:'Validating file',publishing:'Adding to library',complete:'Installed in your library',busy:'Wait for the operation in Jadyx to finish.',
 exists:'This destination already exists. Change its name and try again.',space:'Not enough free space.',file:'Invalid filename or file. Choose an NSP, XCI or NRO file.',
 base:'Base games only; updates, DLC and mixed packages are not supported.',nsp:'Invalid file structure.',incomplete:'The file is incomplete.',
 keys:'Cannot read its metadata. Check your console keys and the file.',disk:'Could not write or publish this file.',
 interrupted:'Transfer interrupted. You can resume from confirmed progress.',partial:'An upload is pending. Resume or discard it.',
 recovery:'Cannot recover the partial file. Discard it before starting another.',unavailable:'Import is currently unavailable.',destination:'Dev storage (D:) is not available. Choose internal storage.',
 attempts:'Too many attempts. Wait one minute.',version:'Incompatible upload version. Reload this page.',idle:'Ready to import.'};
if(es||pt){document.documentElement.lang=pt?'pt-BR':'es';for(const id of ['title','intro','label','connect','uploadtitle','uploadintro','fileslabel','namelabel','send','rename','pause','discard','privacy',
 'kicker','status-off','status-on','code-hint','files-drop','where-internal','where-developer'])$(id).textContent=words[id];}
const labels=pt?{'tab-games':'Jogos','tab-system':'Sistema','cat-keys':'Chaves'}
:es?{'tab-games':'Juegos','tab-system':'Sistema','cat-keys':'Claves'}:{};
const nav=pt?{add:'Adicionar jogos',uploading:'Envio em andamento'}:es?{add:'Añadir juegos',uploading:'Subida en curso'}:{add:'Add games',uploading:'Upload in progress'};
for(const [id,text]of Object.entries(labels))$(id).textContent=text;
if(es||pt)document.title='Portal Jadyx';
// The header's tabs, one view each; one is shown.
const tabs=[['tab-games','view-content'],['tab-amiibo','view-amiibo'],['tab-system','view-system']];
function choose(group,chosen){for(const [button,view]of group){$(button).ariaSelected=String(button===chosen);$(view).hidden=button!==chosen;}}
for(const [button]of tabs)$(button).addEventListener('click',()=>{
 const again=$(button).ariaSelected==='true';choose(tabs,button);$('message').textContent='';
 if(button==='tab-amiibo')loadAmiibo();
 // Games pressed again, from a game or from importing, is the way back to the library.
 if(button==='tab-games'&&again&&state.library){gameId='';armed='';choosePane('library');}
});
choose(tabs,'tab-games');
for(const kind of ['keys','firmware','logs'])$('cat-'+kind).addEventListener('click',()=>{systemPane=kind;render();});
const selectedChunk=Number(new URLSearchParams(location.search).get('chunk'));
const chunk=(selectedChunk===4||selectedChunk===16||selectedChunk===64?selectedChunk:16)*1048576;
let session='',timer,state={},running=false,stopped=false,currentXHR=null,currentId='',rates=[],pane='library',contentRunning=false,where='internal';
let systemPane='keys',systemRunning=false,systemReviewId='',saveDownloading=false;
const delay=ms=>new Promise(r=>setTimeout(r,ms)),mib=n=>(n/1048576).toFixed(1),reason=e=>words[e.message]||words.interrupted;
const size=n=>n>=1073741824?(n/1073741824).toFixed(2)+' GiB':mib(n)+' MiB',percent=(done,total)=>total?Math.floor(100*Math.min(1,done/total))+' %':'';
// What a drop zone holds: the file's name, or how many and their total size.
function picked(output,input){
 const list=Array.from($(input).files||[]),total=list.reduce((sum,f)=>sum+f.size,0);
 $(output).textContent=!list.length?'':(list.length===1?list[0].name:list.length+' '+words.files)+' · '+size(total);
}
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
 if(session&&!state.library&&pane==='library')pane='games';
 if(session&&!state.system_supported)systemPane='logs';
 renderLogs();
 renderUpdate();
 const s=state.transfer||{};
 $('upload').hidden=!session||!state.uploads||pane!=='games';
 const pending=!!s.id&&s.phase!=='complete';
 if(pending)where=s.destination==='developer'?'developer':'internal';else if(!s.developer_available)where='internal';
 $('where').hidden=!s.developer_available&&where!=='developer';
 for(const id of ['internal','developer']){const b=$('where-'+id);b.setAttribute('aria-selected',String(where===id));b.disabled=running||pending;}
 // Both volumes: the internal one and, when present, Dev Mode's storage on D:.
 const gib=bytes=>((bytes||0)/1073741824).toFixed(2)+' GiB';
 $('space').textContent=words.free+': '+(s.developer_available?'\n'+words['where-internal']+' · '+gib(s.free_bytes)+'\n'+words['where-developer']+' · '+gib(s.developer_free_bytes):gib(s.free_bytes));
 const transfer=s.phase==='complete'&&!running?{}:s;
 $('job').textContent=(transfer.name?transfer.name+'\n':'')+(words[transfer.phase]||words.idle);
 $('progress').value=transfer.length?Math.min(1,(transfer.received||0)/transfer.length):0;
 $('percent').textContent=percent(transfer.received||0,transfer.length);
 $('confirmed').textContent=words.confirmed+': '+mib(transfer.offset||0)+' / '+mib(transfer.length||0)+' MiB';
 $('files').disabled=running;$('name').disabled=running||s.phase==='validating'||s.phase==='publishing';
 $('send').disabled=running||contentRunning||systemRunning||state.system?.busy||!session||!state.uploads||s.phase==='ready'||s.phase==='validating'||s.phase==='publishing';
 $('pause').disabled=systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('discard').disabled=systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('rename').disabled=running||systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('name-field').hidden=!pending&&$('files').files.length!==1;
 $('pause').hidden=$('discard').hidden=$('rename').hidden=!pending;$('upload-panel').hidden=!pending&&!running;
 $('upload-back').hidden=!state.library;$('library-add').hidden=!state.uploads;
 $('library-add').textContent=pending?nav.uploading+' · '+percent(s.received||0,s.length):nav.add;
 if(s.error)$('message').textContent=words[s.error]||words.interrupted;
 renderSystem();
 renderLibrary();
 renderAmiibo();
}
async function refresh(){const response=await api('/api/v1/status');state=await response.json();render();followLibrary();return state;}
$('pair').addEventListener('submit',async event=>{
 event.preventDefault();$('connect').disabled=true;$('message').textContent='';
 try{
 const response=await fetch('/api/v1/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:$('code').value})});
 if(!response.ok){$('connection').textContent=response.status===429?words.attempts:words.bad;return;}
 session=(await response.json()).session;$('code').value='';$('pair').hidden=true;$('connection').textContent=words.ok;
 await refresh();clearInterval(timer);let polling=false;
 // An upload left unfinished is what the player came back for.
 if(state.uploads&&state.transfer?.id&&state.transfer.phase!=='complete'){pane='games';render();}
 timer=setInterval(async()=>{if(polling)return;polling=true;try{await refresh();failures=0;$('connection').textContent=words.ok;}catch(error){
 // After the package went to Device Portal, the console going away is the install closing Jadyx.
 if(updateStarted&&(error.message==='lost'||++failures>=3)){clearInterval(timer);session='';updateGone=true;renderUpdate();}
 else if(error.message==='lost'){clearInterval(timer);stopped=true;if(currentXHR)currentXHR.abort();session='';$('pair').hidden=false;$('upload').hidden=true;$('connection').textContent=words.lost;}
 else $('connection').textContent=words.interrupted;
 }finally{polling=false;}},1000);
 }catch{$('connection').textContent=words.lost;}finally{$('connect').disabled=false;}
});
// Only digits; the eighth one connects without reaching for the button.
$('code').addEventListener('input',()=>{const code=$('code').value.replace(/\D/g,'').slice(0,4);$('code').value=code;
 if(code.length===4&&!$('connect').disabled)$('pair').requestSubmit?.();});
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
// Games and updates/DLC share this resumable client. A target names the status record, its
// route, the extra metadata (an update/DLC names its kind and game) and where speed is shown.
const gameTarget={key:'transfer',base:'/api/v1/uploads',tag:'',get metadata(){return {destination:where};},rate:'rate',eta:'eta'};
for(const id of ['internal','developer'])$('where-'+id).addEventListener('click',()=>{if(!running){where=id;render();}});
async function sendFile(file,name,target=gameTarget){
 const signature=await fingerprint(file);if(stopped)throw Error('paused');
 await refresh();let s=state[target.key];
 if(s.id&&s.phase!=='complete'){
 if(s.fingerprint!==signature||s.length!==file.size||(s.tag||'')!==target.tag)throw Error('same');
 currentId=s.id;
 if(s.name!==name)await api(target.base+'/'+currentId+'/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name})});
 }else{
 const metadata=Object.entries({filename:name,fingerprint:signature,...target.metadata}).map(([key,value])=>key+' '+base64(value)).join(',');
 const created=await api(target.base,{method:'POST',headers:{'Upload-Length':String(file.size),'Upload-Metadata':metadata}});
 currentId=created.headers.get('Location').split('/').pop();
 }
 const url=target.base+'/'+currentId;
 let offset=Number((await api(url,{method:'HEAD'})).headers.get('Upload-Offset')),attempts=0;
 while(offset<file.size){
 if(stopped)throw Error('paused');
 const start=performance.now(),before=offset,blob=file.slice(offset,Math.min(file.size,offset+chunk));
 try{
 offset=await patch(url,offset,blob);
 if(!Number.isSafeInteger(offset)||offset<=before||offset>file.size)throw Error('offset');
 const elapsed=Math.max(.001,(performance.now()-start)/1000);rates.push({bytes:offset-before,time:elapsed});if(rates.length>8)rates.shift();
 const speed=rates.reduce((a,x)=>a+x.bytes,0)/rates.reduce((a,x)=>a+x.time,0);
 $(target.rate).textContent=words.speed+': '+mib(speed)+' MiB/s';$(target.eta).textContent=words.eta+': '+formatDuration((file.size-offset)/speed);attempts=0;
 }catch(error){
 if(stopped||error.message==='paused'||error.message==='lost'||!['interrupted','offset'].includes(error.message)||attempts++>=5)throw error;
 await delay(Math.min(3000,250*2**attempts));if(stopped)throw Error('paused');
 offset=Number((await api(url,{method:'HEAD'})).headers.get('Upload-Offset'));
 }
 }
 if(stopped)throw Error('paused');await api(url+'/finish',{method:'POST'});
 for(;;){await delay(500);await refresh();s=state[target.key];if(s.phase==='complete')return;if(s.error)throw Error(s.error);if(stopped||s.phase==='paused')throw Error('paused');}
}
$('files').addEventListener('change',()=>{if($('files').files.length)$('name').value=$('files').files[0].name;picked('files-picked','files');render();});
// The file inputs cover their drop zones, so a drop lands on the input itself; this only lights the zone.
for(const id of ['files','content-files','keys-files','firmware-files']){
 for(const event of ['dragenter','dragover'])$(id).addEventListener(event,e=>e.target.parentElement.classList.add('over'));
 for(const event of ['dragleave','drop'])$(id).addEventListener(event,e=>e.target.parentElement.classList.remove('over'));
}
$('send').addEventListener('click',async()=>{
 if(running||systemRunning||state.system?.busy)return;const files=Array.from($('files').files);if(!files.length){$('message').textContent=words.choose;return;}
 running=true;stopped=false;rates=[];$('message').textContent='';render();
 try{for(let i=0;i<files.length;i++){if(stopped)break;$('queue').textContent=words.queue+': '+(files.length-i-1);await sendFile(files[i],files.length===1?$('name').value:files[i].name);}if(!stopped){$('files').value='';$('name').value='';$('message').textContent=words.complete;}}
 catch(error){$('message').textContent=reason(error);}finally{running=false;currentXHR=null;for(const id of ['rate','eta','queue'])$(id).textContent='';try{await refresh();}catch{}render();}
});
async function control(action,target=gameTarget){
 stopped=true;if(currentXHR)currentXHR.abort();$('message').textContent='';
 try{await refresh();const id=state[target.key]?.id;if(!id)return;
 for(let retry=0;;retry++)try{await api(target.base+'/'+id+(action==='pause'?'/pause':''),{method:action==='pause'?'POST':'DELETE'});break;}
 catch(error){if(error.message!=='busy'||retry>=15)throw error;await delay(100);}
 await refresh();}catch(error){$('message').textContent=reason(error);}
}
$('pause').addEventListener('click',()=>control('pause'));$('discard').addEventListener('click',()=>control('discard'));
$('rename').addEventListener('click',async()=>{try{await api('/api/v1/uploads/'+state.transfer.id+'/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:$('name').value})});$('message').textContent='';await refresh();}catch(error){$('message').textContent=reason(error);}});

// Library: the console's games, each opening into its updates and DLC. An upload is installed
// in internal storage by the console's content import; a deletion runs there too, after the
// same second press the console asks for.
const libraryWords=pt?{
 title:'Sua biblioteca',intro:'Abra um jogo para gerenciar suas atualizações, DLC e save, ou para apagá-lo.',empty:'Ainda não há jogos.',back:'← Biblioteca',
 update:'Atualizações',dlc:'DLC',none_update:'Nenhuma atualização instalada.',none_dlc:'Nenhum DLC instalado.',pick_update:'Selecionar NSP ou XCI de atualização',pick_dlc:'Selecionar NSP ou XCI de DLC',
 send:'Enviar e instalar',remove_game:'Apagar jogo',remove:'Apagar',confirm:'Clique de novo para confirmar',external_item:'Pasta externa',
 warning:'Apagar o jogo remove também seus saves, atualizações e DLC deste console. Mods e cheats são mantidos. Não pode ser desfeito.',
 external:'Este jogo está em uma pasta externa; o Jadyx não apaga arquivos de lá.',removing:'Apagando…',removed_game:'Jogo apagado.',removed:'Conteúdo apagado.',
 ready:'Aguardando instalação',validating:'Instalando',complete:'Instalado',idle:'Escolha arquivos para instalar.',failed:'Não foi possível concluir a operação.',
 game:'O jogo não está mais na biblioteca ou não pôde ser apagado.',content:'Não foi possível apagar esse conteúdo.',saves:'O jogo foi apagado, mas alguns saves não puderam ser removidos.',
 wrong_game:'Esse pacote pertence a outro jogo.',kind:'O pacote não é do tipo escolhido (atualização ou DLC).',conflict:'Já existe uma versão conflitante desse conteúdo.',
 hash:'O pacote está danificado.',requirements:'O pacote exige uma versão mais nova do jogo ou do firmware.',unsupported:'Esse pacote não é compatível.',invalid:'O pacote não é válido.',metadata:'Recarregue a página e tente de novo.'
}:es?{
 title:'Tu biblioteca',intro:'Abre un juego para gestionar sus actualizaciones, DLC y partida, o para eliminarlo.',empty:'Todavía no hay juegos.',back:'← Biblioteca',
 update:'Actualizaciones',dlc:'DLC',none_update:'No hay actualizaciones instaladas.',none_dlc:'No hay DLC instalados.',pick_update:'Seleccionar NSP o XCI de actualización',pick_dlc:'Seleccionar NSP o XCI de DLC',
 send:'Subir e instalar',remove_game:'Eliminar juego',remove:'Eliminar',confirm:'Pulsa otra vez para confirmar',external_item:'Carpeta externa',
 warning:'Eliminar el juego borra también sus partidas, actualizaciones y DLC de esta consola. Los mods y trucos se conservan. No se puede deshacer.',
 external:'Este juego está en una carpeta externa; Jadyx no borra archivos de ahí.',removing:'Eliminando…',removed_game:'Juego eliminado.',removed:'Contenido eliminado.',
 ready:'Esperando a instalar',validating:'Instalando',complete:'Instalado',idle:'Selecciona archivos para instalar.',failed:'No se pudo completar la operación.',
 game:'El juego ya no está en la biblioteca o no se pudo eliminar.',content:'No se pudo eliminar ese contenido.',saves:'El juego se eliminó, pero algunas partidas no se pudieron borrar.',
 wrong_game:'Ese paquete es de otro juego.',kind:'El paquete no es del tipo elegido (actualización o DLC).',conflict:'Ya hay una versión de ese contenido que choca con esta.',
 hash:'El paquete está dañado.',requirements:'El paquete necesita una versión más nueva del juego o del firmware.',unsupported:'Ese paquete no es compatible.',invalid:'El paquete no es válido.',metadata:'Recarga la página y vuelve a intentarlo.'
}:{
 title:'Your library',intro:'Open a game to manage its updates, DLC and save, or to delete it.',empty:'No games yet.',back:'← Library',
 update:'Updates',dlc:'DLC',none_update:'No updates installed.',none_dlc:'No DLC installed.',pick_update:'Choose update NSP or XCI files',pick_dlc:'Choose DLC NSP or XCI files',
 send:'Upload and install',remove_game:'Delete game',remove:'Delete',confirm:'Click again to confirm',external_item:'External folder',
 warning:'Deleting the game also deletes its saves, updates and DLC from this console. Mods and cheats are kept. This cannot be undone.',
 external:'This game is in an external folder; Jadyx does not delete files there.',removing:'Deleting…',removed_game:'Game deleted.',removed:'Content deleted.',
 ready:'Waiting to install',validating:'Installing',complete:'Installed',idle:'Choose files to install.',failed:'The operation could not be completed.',
 game:'The game is no longer in the library or could not be deleted.',content:'That content could not be deleted.',saves:'The game was deleted, but some saves could not be removed.',
 wrong_game:'That package belongs to another game.',kind:'The package is not the chosen kind (update or DLC).',conflict:'A conflicting version of this content is already installed.',
 hash:'The package is damaged.',requirements:'The package needs a newer game or firmware version.',unsupported:'That package is not supported.',invalid:'The package is not valid.',metadata:'Reload this page and try again.'};
for(const [id,key]of [['library-title','title'],['library-intro','intro'],['library-empty','empty'],['game-back','back'],['upload-back','back'],['game-warning','warning'],
 ['kind-update','update'],['kind-dlc','dlc'],['content-send','send'],['content-pick','pick_update']])$(id).textContent=libraryWords[key];
for(const [id,key]of [['content-drop','files-drop'],['content-pause','pause'],['content-discard','discard']])if(words[key])$(id).textContent=words[key];
const libraryReason=error=>libraryWords[error.message]||words[error.message]||libraryWords.failed;
// A game's save: downloaded as a .zip, loaded from an archive the player made anywhere. The file
// is checked here first so a wrong one never spends an upload; the console checks it all again.
const saveWords=pt?{
 tab:'Save',intro:'Baixe o save deste jogo como .zip, ou carregue um feito aqui, em outro console ou no PC.',download:'Baixar save',downloaded:'Save baixado.',
 pick:'Selecionar o save (.zip, .7z ou .rar)',drop:'ou arraste-o para cá',send:'Carregar save',choose:'Selecione o arquivo do save.',
 warning:'Carregar substitui o save atual deste jogo. O anterior é guardado como cópia no console.',loaded:'Save carregado. Se havia um, o anterior foi guardado como cópia no console.',
 help:'Como o arquivo deve ser?',
 direct:'✔ Os arquivos do save direto',inside:'✔ Ou dentro da pasta do jogo',archive_name:'save.zip',file1:'arquivo do save',more:'pasta/…',
 pc:'No PC (Eden ou yuzu): clique com o botão direito no jogo → Abrir pasta de save e comprima o que há dentro (ou a pasta inteira). Do Ryujinx serve a pasta …/bis/user/save/<número>/0.',
 rules:'✘ Sem senha · ✘ Sem jogos nem atualizações (.nsp/.xci) · ✘ Sem saves de outros jogos · Máximo de 1 GiB.',
 save_file:'Escolha um arquivo .zip, .7z ou .rar com o save.',save_limit:'O arquivo é grande demais para ser um save (máx. 1 GiB e 8192 arquivos).',
 not_archive:'O arquivo não é um compactado válido (foi renomeado para .zip?).',file_empty:'O arquivo está vazio.',
 archive:'O compactado está danificado ou incompleto. Crie-o de novo.',encrypted:'O compactado tem senha. Crie-o sem senha.',
 empty:'O compactado não tem nenhum arquivo de save.',archive_unsafe:'O compactado tem caminhos não permitidos e não será usado: «{detail}».',
 bad_name:'Há arquivos com nomes que o console não aceita: «{detail}». Renomeie-os.',not_save:'Isto parece um jogo ou uma atualização, não um save: «{detail}».',
 wrong_game:'Este save é de outro jogo. Encontrado: {detail}. Esperado: a pasta {game}/ ou os arquivos direto.',
 save_folder:'O save está dentro da pasta «{detail}» e não dá para saber se ela faz parte dele. Comprima o que há dentro, sem a pasta. Se a pasta for mesmo do jogo, jogue até salvar uma vez e carregue de novo.',
 save_unknown:'A pasta «{detail}» não tem nada em comum com o save deste jogo. Pode estar danificada ou ser de outro jogo, por isso não foi carregada. Abra-a e confira.',
 space:'Não há espaço livre suficiente no console.',profile:'Não há um usuário no Jadyx. Abra um jogo uma vez e tente de novo.',
 disk:'Não foi possível guardar o save; o anterior foi mantido.',export_disk:'Não foi possível ler o save no console.',
 no_save:'Este jogo ainda não tem save neste console.',failed:'Não foi possível carregar o save; o anterior foi mantido.'
}:es?{
 tab:'Partida',intro:'Descarga la partida de este juego como .zip, o carga una hecha aquí, en otra consola o en el PC.',download:'Descargar partida',downloaded:'Partida descargada.',
 pick:'Seleccionar la partida (.zip, .7z o .rar)',drop:'o suéltala aquí',send:'Cargar partida',choose:'Selecciona el archivo de la partida.',
 warning:'Cargar reemplaza la partida actual de este juego. La anterior se guarda como copia en la consola.',loaded:'Partida cargada. Si había una, la anterior se guardó como copia en la consola.',
 help:'¿Cómo debe ser el archivo?',
 direct:'✔ Los archivos de la partida directamente',inside:'✔ O dentro de la carpeta del juego',archive_name:'partida.zip',file1:'archivo de la partida',more:'carpeta/…',
 pc:'En el PC (Eden o yuzu): clic derecho en el juego → Abrir carpeta de partida, y comprime lo que hay dentro (o la carpeta entera). De Ryujinx vale la carpeta …/bis/user/save/<número>/0.',
 rules:'✘ Sin contraseña · ✘ Sin juegos ni actualizaciones (.nsp/.xci) · ✘ Sin partidas de otros juegos · Máximo 1 GiB.',
 save_file:'Elige un archivo .zip, .7z o .rar con la partida.',save_limit:'El archivo es demasiado grande para ser una partida (máx. 1 GiB y 8192 archivos).',
 not_archive:'El archivo no es un comprimido válido (¿se renombró a .zip?).',file_empty:'El archivo está vacío.',
 archive:'El comprimido está dañado o incompleto. Vuelve a crearlo.',encrypted:'El comprimido tiene contraseña. Créalo sin contraseña.',
 empty:'El comprimido no tiene ningún archivo de partida.',archive_unsafe:'El comprimido tiene rutas no permitidas y no se usará: «{detail}».',
 bad_name:'Hay archivos con nombres que la consola no admite: «{detail}». Renómbralos.',not_save:'Esto parece un juego o una actualización, no una partida: «{detail}».',
 wrong_game:'Esta partida es de otro juego. Encontrado: {detail}. Se esperaba la carpeta {game}/ o los archivos directamente.',
 save_folder:'La partida está dentro de la carpeta «{detail}» y no se puede saber si es parte de ella. Comprime lo que hay dentro, sin la carpeta. Si la carpeta sí es del juego, juega hasta guardar una vez y vuelve a cargarla.',
 save_unknown:'La carpeta «{detail}» no tiene nada en común con la partida de este juego. Puede estar dañada o ser de otro juego, así que no se cargó. Ábrela y revísala.',
 space:'No hay espacio libre suficiente en la consola.',profile:'No hay un usuario en Jadyx. Abre un juego una vez y vuelve a intentarlo.',
 disk:'No se pudo guardar la partida; la anterior se conserva.',export_disk:'No se pudo leer la partida en la consola.',
 no_save:'Este juego todavía no tiene partida en esta consola.',failed:'No se pudo cargar la partida; la anterior se conserva.'
}:{
 tab:'Save',intro:'Download this game\'s save as a .zip, or load one made here, on another console or on a PC.',download:'Download save',downloaded:'Save downloaded.',
 pick:'Choose the save (.zip, .7z or .rar)',drop:'or drop it here',send:'Load save',choose:'Choose the save file.',
 warning:'Loading replaces this game\'s current save. The previous one is kept as a copy on the console.',loaded:'Save loaded. If there was one, the previous save was kept as a copy on the console.',
 help:'What should the file look like?',
 direct:'✔ The save files directly',inside:'✔ Or inside the game\'s folder',archive_name:'save.zip',file1:'save file',more:'folder/…',
 pc:'On a PC (Eden or yuzu): right-click the game → Open Save Data Location, and compress what is inside (or the whole folder). From Ryujinx, the …/bis/user/save/<number>/0 folder works.',
 rules:'✘ No password · ✘ No games or updates (.nsp/.xci) · ✘ No saves of other games · 1 GiB at most.',
 save_file:'Choose a .zip, .7z or .rar file with the save.',save_limit:'The file is too large to be a save (1 GiB and 8192 files at most).',
 not_archive:'The file is not a valid archive (was it renamed to .zip?).',file_empty:'The file is empty.',
 archive:'The archive is damaged or incomplete. Create it again.',encrypted:'The archive has a password. Create it without one.',
 empty:'The archive has no save files.',archive_unsafe:'The archive has paths that are not allowed and will not be used: «{detail}».',
 bad_name:'Some files have names the console does not accept: «{detail}». Rename them.',not_save:'This looks like a game or an update, not a save: «{detail}».',
 wrong_game:'This save belongs to another game. Found: {detail}. Expected: the {game}/ folder or the files directly.',
 save_folder:'The save is inside the folder «{detail}», and there is no way to tell whether it is part of the save. Compress what is inside, without the folder. If the folder really belongs to the game, play until it saves once and load it again.',
 save_unknown:'The folder «{detail}» has nothing in common with this game\'s save. It may be damaged or from another game, so it was not loaded. Open it and check.',
 space:'There is not enough free space on the console.',profile:'There is no user in Jadyx. Open a game once and try again.',
 disk:'The save could not be written; the previous one was kept.',export_disk:'The save could not be read on the console.',
 no_save:'This game has no save on this console yet.',failed:'The save could not be loaded; the previous one was kept.'};
// The errors the guide answers: what the archive holds, not how it travelled.
const saveStructural=['empty','not_save','wrong_game','archive_unsafe','bad_name','not_archive','save_file','save_folder','save_unknown'];
for(const [id,key]of [['kind-save','tab'],['save-intro','intro'],['save-download','download'],['save-pick','pick'],['save-drop','drop'],['save-send','send'],
 ['save-warning','warning'],['save-help-toggle','help'],['save-error-help','help'],['save-pc','pc'],['save-rules','rules']])$(id).textContent=saveWords[key];
function saveTree(){
 const id=(gameId||'0100000000000000').toUpperCase(),w=saveWords;
 const left=[w.direct,'  '+w.archive_name,'  ├─ '+w.file1,'  └─ '+w.more,''];
 const right=[w.inside,'  '+w.archive_name,'  └─ '+id+'/','     ├─ '+w.file1,'     └─ '+w.more];
 const width=Math.max(...left.map(l=>l.length))+4;
 $('save-tree').textContent=left.map((l,i)=>l.padEnd(width)+right[i]).join('\n');
}
function showSaveHelp(open,flash){
 saveTree();$('save-help').hidden=!open;
 if(flash){$('save-help').classList.add('flash');setTimeout(()=>$('save-help').classList.remove('flash'),2000);}
}
// "word" or "word:detail", as the console reports it.
function saveError(error,download){
 const text=String(error&&error.message||error),colon=text.indexOf(':');
 let word=colon<0?text:text.slice(0,colon);const detail=colon<0?'':text.slice(colon+1);
 if(download&&word==='disk')word='export_disk';
 const message=(saveWords[word]||libraryWords[word]||words[word]||saveWords.failed).replace('{detail}',detail||'?').replace('{game}',(gameId||'').toUpperCase());
 $('save-error-text').textContent=message;$('save-error-help').hidden=!saveStructural.includes(word);$('save-error').hidden=false;
}
function clearSaveError(){$('save-error').hidden=true;$('save-error-text').textContent='';}
// What the browser can tell before uploading: the name, the size and the first bytes.
async function checkSave(file){
 if(!/\.(zip|7z|rar)$/i.test(file.name))return 'save_file';
 if(!file.size)return 'file_empty';
 if(file.size>1073741824)return 'save_limit';
 const b=new Uint8Array(await file.slice(0,8).arrayBuffer());
 const zip=b[0]===0x50&&b[1]===0x4B&&[3,5,7].includes(b[2])&&b[3]===b[2]+1;
 const rar=b[0]===0x52&&b[1]===0x61&&b[2]===0x72&&b[3]===0x21&&b[4]===0x1A&&b[5]===0x07;
 const seven=b[0]===0x37&&b[1]===0x7A&&b[2]===0xBC&&b[3]===0xAF&&b[4]===0x27&&b[5]===0x1C;
 return zip||rar||seven?'':'not_archive';
}
let games=[],revision=-1,libraryAt=0,gameId='',kind='update',contents=null,armed='',armedTimer=0,removalSerial=0,lastContent='';
const covers=new Map(),gone=new Set();
const contentTarget=()=>({key:'content',base:'/api/v1/content/uploads',tag:kind+':'+gameId,metadata:{kind,application:gameId},rate:'content-rate',eta:'content-eta'});
function node(tag,className,text){const n=document.createElement(tag);if(className)n.className=className;if(text)n.textContent=text;return n;}
// Covers come with the session header, so they are fetched once into blob URLs.
async function cover(id,art){
 art.textContent=(games.find(g=>g.id===id)?.name||'?').slice(0,1);art.style.backgroundImage='';
 if(!covers.has(id))covers.set(id,api('/api/v1/library/'+id+'/cover').then(r=>r.blob()).then(b=>URL.createObjectURL(b)).catch(()=>''));
 const url=await covers.get(id);
 if(url&&art.dataset.id===id){art.style.backgroundImage='url("'+url+'")';art.textContent='';}
}
function showCover(art,id){art.dataset.id=id;cover(id,art);}
async function loadLibrary(){
 if(!session||!state.library)return;libraryAt=Date.now();
 try{
  const data=await(await api('/api/v1/library')).json();
  if(data.revision!==revision){
   revision=data.revision;
   for(const url of covers.values())url.then(u=>u&&URL.revokeObjectURL(u));covers.clear();
   for(const id of gone)if(!data.games.some(g=>g.id===id))gone.delete(id);
   games=data.games;renderGames();
   if(gameId)openGame(gameId,false);
  }
 }catch(error){$('message').textContent=libraryReason(error);}
}
function renderGames(){
 const shown=games.filter(g=>!gone.has(g.id));
 $('games').replaceChildren(...shown.map(game=>{
  const tile=node('button','tile'),art=node('div','art');tile.type='button';showCover(art,game.id);
  tile.append(art,node('b','',game.name),node('small','',[game.developer,game.version].filter(Boolean).join(' · ')));
  tile.addEventListener('click',()=>openGame(game.id,true));return tile;
 }));
 $('library-empty').hidden=shown.length>0;
}
async function loadContents(){
 const id=gameId;if(!id)return;
 try{const data=await(await api('/api/v1/library/'+id+'/content')).json();if(id===gameId){contents=data.contents;renderItems();}}
 catch(error){$('message').textContent=libraryReason(error);}
}
function openGame(id,fresh){
 const game=games.find(g=>g.id===id);
 if(!game||gone.has(id)){gameId='';render();return;}
 if(fresh){gameId=id;contents=null;armed='';chooseKind('update');$('library-note').textContent='';$('content-files').value='';$('content-picked').textContent='';
  $('save-file').value='';$('save-picked').textContent='';clearSaveError();showSaveHelp(false);}
 $('game-name').textContent=game.name;$('game-meta').textContent=[game.developer,game.version].filter(Boolean).join(' · ');
 showCover($('game-art'),id);loadContents();render();
}
function chooseKind(next){
 kind=next;armed='';const save=kind==='save';
 for(const k of ['update','dlc','save'])$('kind-'+k).ariaSelected=String(k===kind);
 $('items').hidden=save;$('content-zone').hidden=save;$('content-send').hidden=save;$('save-pane').hidden=!save;
 if(!save)$('content-pick').textContent=libraryWords['pick_'+kind];else saveTree();
 renderItems();
}
// Nothing deletes or installs while another storage job runs; the server refuses it as well.
function libraryBusy(){
 const r=state.removal||{};
 return running||contentRunning||systemRunning||!!state.system?.busy||!!state.transfer?.busy||!!state.content?.busy||r.phase==='pending'||r.phase==='running';
}
// The first press arms a deletion and the second, within six seconds, sends it.
function arm(key){
 if(armed===key){armed='';clearTimeout(armedTimer);return true;}
 armed=key;clearTimeout(armedTimer);armedTimer=setTimeout(()=>{armed='';renderItems();renderAmiiboItems();render();},6000);renderItems();renderAmiiboItems();render();return false;
}
async function remove(path,key){
 if(!arm(key))return;
 try{const data=await(await api('/api/v1/library/'+path,{method:'DELETE'})).json();removalSerial=data.removal.serial;$('message').textContent='';await refresh();}
 catch(error){$('message').textContent=libraryReason(error);}
 renderItems();
}
function renderItems(){
 if(!gameId||kind==='save')return;
 const list=(contents||[]).filter(c=>c.kind===kind),busy=libraryBusy();
 $('items').replaceChildren(...(list.length?list.map(item=>{
  const row=node('li'),text=node('span'),button=node('button','danger',armed===item.id?libraryWords.confirm:libraryWords.remove);
  text.append(node('b','',item.name||libraryWords[kind]),node('small','',[item.version,size(item.bytes||0),item.internal?'':libraryWords.external_item].filter(Boolean).join(' · ')));
  button.disabled=busy;button.addEventListener('click',()=>remove(gameId+'/content/'+item.id,item.id));
  row.append(text,button);return row;
 }):[node('li','empty',contents?libraryWords['none_'+kind]:'…')]));
}
let lastBusy=false;
function renderLibrary(){
 const on=!!session&&!!state.library;
 $('library').hidden=!on||pane!=='library';if($('library').hidden)return;
 const game=gameId&&games.find(g=>g.id===gameId);
 $('library-list').hidden=!!game;$('game').hidden=!game;
 const r=state.removal||{};
 if(r.phase==='pending'||r.phase==='running')$('library-note').textContent=libraryWords.removing;
 if(!game)return;
 const busy=libraryBusy(),content=state.content||{},s=content.phase==='complete'&&!contentRunning?{}:content;
 const install=state.install||{},installing=s.phase==='validating';
 if(busy!==lastBusy){lastBusy=busy;renderItems();}
 $('game-remove').hidden=!game.removable;$('game-remove').disabled=busy;
 $('game-remove').textContent=armed==='game'?libraryWords.confirm:libraryWords.remove_game;
 $('game-warning').textContent=game.removable?libraryWords.warning:libraryWords.external;
 const active=s.id&&!['complete','validating','publishing'].includes(s.phase);
 $('content-files').disabled=contentRunning;
 $('content-send').disabled=busy||s.available===false||['ready','validating','publishing'].includes(s.phase);
 $('save-send').disabled=$('content-send').disabled;$('save-file').disabled=contentRunning;$('save-download').disabled=saveDownloading;
 $('content-pause').disabled=!active||systemRunning;$('content-discard').disabled=!active||systemRunning;
 $('content-job').textContent=(s.name?s.name+'\n':'')+(installing&&install.total?libraryWords.validating+' · '+install.done+' / '+install.total+' MiB':
  libraryWords[s.phase]||words[s.phase]||libraryWords.idle);
 $('content-progress').value=installing?(install.total?Math.min(1,install.done/install.total):0):s.length?Math.min(1,(s.received||0)/s.length):0;
 $('content-percent').textContent=installing?percent(install.done||0,install.total):percent(s.received||0,s.length);
 $('content-pause').hidden=$('content-discard').hidden=!active;$('content-panel').hidden=!s.id&&!contentRunning;
}
// Follows this page's own deletion and installs to their end, then reloads what they changed.
function followLibrary(){
 if(!state.library)return;
 const r=state.removal||{},s=state.content||{};
 if(removalSerial&&r.serial===removalSerial&&(r.phase==='complete'||r.phase==='failed')){
  removalSerial=0;
  if(r.phase==='failed'){$('library-note').textContent='';$('message').textContent=libraryReason(Error(r.error));}
  else{
   $('library-note').textContent=r.content?libraryWords.removed:libraryWords.removed_game;
   if(!r.content){gone.add(r.game);if(gameId===r.game)gameId='';renderGames();}
  }
  loadContents();render();
 }
 const phase=s.id+':'+s.phase;
 if(phase!==lastContent){lastContent=phase;if(s.phase==='complete'&&s.tag===kind+':'+gameId)loadContents();}
 if(pane==='library'&&Date.now()-libraryAt>10000)loadLibrary();
}
function choosePane(next){pane=next;$('message').textContent='';if(next==='library')loadLibrary();render();}
$('library-add').addEventListener('click',()=>choosePane('games'));$('upload-back').addEventListener('click',()=>choosePane('library'));
// Amiibo: small files, one request each, so one bad file never stops the others. The key can be
// sent or removed from here, never read back.
const amiiboWords=pt?{
 'amiibo-tab-list':'Meus amiibos','amiibo-tab-add':'Adicionar','amiibo-tab-key':'Chave','amiibo-list-title':'Seus amiibos','amiibo-intro':'Quando um jogo pedir um amiibo, abra o menu do jogo e escolha um.',
 'amiibo-add-title':'Adicionar amiibos','amiibo-add-help':'Selecione seus arquivos .bin. Você pode enviar vários de uma vez.','amiibo-pick':'Selecionar arquivos .bin','amiibo-drop':'ou arraste-os para cá','amiibo-send':'Enviar',
 search:'Buscar amiibo',sort_up:'A–Z',sort_down:'Z–A',sort_label:'Mudar a ordem',count:n=>n+(n===1?' amiibo':' amiibos'),count_of:(n,total)=>n+' de '+total,no_match:'Nenhum amiibo corresponde à sua busca.',
 'amiibo-key-title':'Chave de amiibo (opcional)','amiibo-key-help':'O arquivo key_retail.bin permite que os jogos salvem dados nos seus amiibos. Ele fica no console e não pode ser baixado por aqui.','amiibo-key-pick':'Selecionar key_retail.bin',
 key_on:'Chave instalada: os jogos podem salvar dados nos seus amiibos.',key_off:'Sem chave: os amiibos são somente leitura.',key_send:'Enviar chave',key_replace:'Substituir chave',key_saved:'Chave instalada.',key_removed:'Chave removida.',
 none:'Ainda não há amiibos.',uploaded:'Enviado',replace:'Substituir',choose:'Selecione um ou mais arquivos .bin.',choose_key:'Selecione o arquivo key_retail.bin.',
 name:'Nome de arquivo inválido.',not_amiibo:'Não é um amiibo: deve ser um .bin de 532, 540 ou 572 bytes.',exists:'Já está no console. Substituí-lo apaga o que os jogos salvaram nele.',not_key:'Não é um key_retail.bin válido.',disk:'Não foi possível gravar no console.',missing:'Esse arquivo já não está no console.',unavailable:'Os amiibos não estão disponíveis no momento.'
}:es?{
 'amiibo-tab-list':'Mis amiibos','amiibo-tab-add':'Añadir','amiibo-tab-key':'Clave','amiibo-list-title':'Tus amiibos','amiibo-intro':'Cuando un juego pida un amiibo, abre el menú del juego y elige uno.',
 'amiibo-add-title':'Añadir amiibos','amiibo-add-help':'Selecciona tus archivos .bin. Puedes subir varios a la vez.','amiibo-pick':'Seleccionar archivos .bin','amiibo-drop':'o arrástralos aquí','amiibo-send':'Subir',
 search:'Buscar amiibo',sort_up:'A–Z',sort_down:'Z–A',sort_label:'Cambiar el orden',count:n=>n+(n===1?' amiibo':' amiibos'),count_of:(n,total)=>n+' de '+total,no_match:'Ningún amiibo coincide con tu búsqueda.',
 'amiibo-key-title':'Clave de amiibo (opcional)','amiibo-key-help':'El archivo key_retail.bin permite que los juegos guarden datos en tus amiibos. Se queda en la consola y no se puede descargar desde aquí.','amiibo-key-pick':'Seleccionar key_retail.bin',
 key_on:'Clave instalada: los juegos pueden guardar datos en tus amiibos.',key_off:'Sin clave: los amiibos son de solo lectura.',key_send:'Subir clave',key_replace:'Reemplazar clave',key_saved:'Clave instalada.',key_removed:'Clave eliminada.',
 none:'Aún no hay amiibos.',uploaded:'Subido',replace:'Reemplazar',choose:'Selecciona uno o varios archivos .bin.',choose_key:'Selecciona el archivo key_retail.bin.',
 name:'Nombre de archivo no válido.',not_amiibo:'No es un amiibo: debe ser un .bin de 532, 540 o 572 bytes.',exists:'Ya está en la consola. Reemplazarlo borra lo que los juegos guardaron en él.',not_key:'No es un key_retail.bin válido.',disk:'No se pudo escribir en la consola.',missing:'Ese archivo ya no está en la consola.',unavailable:'Los amiibos no están disponibles en este momento.'
}:{
 'amiibo-tab-list':'My amiibo','amiibo-tab-add':'Add','amiibo-tab-key':'Key','amiibo-list-title':'Your amiibo','amiibo-intro':'When a game asks for an amiibo, open the in-game menu and choose one.',
 'amiibo-add-title':'Add amiibo','amiibo-add-help':'Choose your .bin files. You can upload several at once.','amiibo-pick':'Choose .bin files','amiibo-drop':'or drop them here','amiibo-send':'Upload',
 search:'Search amiibo',sort_up:'A–Z',sort_down:'Z–A',sort_label:'Change the order',count:n=>n+' amiibo',count_of:(n,total)=>n+' of '+total,no_match:'No amiibo match your search.',
 'amiibo-key-title':'Amiibo key (optional)','amiibo-key-help':'The key_retail.bin file lets games save data on your amiibo. It stays on the console and can\'t be downloaded from here.','amiibo-key-pick':'Choose key_retail.bin',
 key_on:'Key installed: games can save data on your amiibo.',key_off:'No key: amiibo are read-only.',key_send:'Upload key',key_replace:'Replace key',key_saved:'Key installed.',key_removed:'Key removed.',
 none:'No amiibo yet.',uploaded:'Uploaded',replace:'Replace',choose:'Choose one or more .bin files.',choose_key:'Choose the key_retail.bin file.',
 name:'Invalid file name.',not_amiibo:'Not an amiibo: it must be a 532, 540 or 572 byte .bin file.',exists:'Already on the console. Replacing it erases what games saved on it.',not_key:'Not a valid key_retail.bin.',disk:'Could not write on the console.',missing:'That file is no longer on the console.',unavailable:'Amiibo are not available right now.'
};
for(const id of ['amiibo-tab-list','amiibo-tab-add','amiibo-tab-key','amiibo-intro','amiibo-add-title','amiibo-add-help','amiibo-pick','amiibo-drop','amiibo-send','amiibo-list-title','amiibo-key-title','amiibo-key-help','amiibo-key-pick'])$(id).textContent=amiiboWords[id];
$('amiibo-search').placeholder=$('amiibo-search').ariaLabel=amiiboWords.search;$('amiibo-sort').title=$('amiibo-sort').ariaLabel=amiiboWords.sort_label;
let amiiboFiles=null,amiiboKey=false,amiiboResults=[],amiiboSending=false,amiiboDescending=false;
// The list, the upload and the key are one view each; one is shown.
const amiiboPanes=[['amiibo-tab-list','amiibo-pane-list'],['amiibo-tab-add','amiibo-pane-add'],['amiibo-tab-key','amiibo-pane-key']];
for(const [button]of amiiboPanes)$(button).addEventListener('click',()=>{choose(amiiboPanes,button);armed='';$('amiibo-note').textContent='';renderAmiiboItems();render();});
choose(amiiboPanes,'amiibo-tab-list');
$('amiibo-search').addEventListener('input',renderAmiiboItems);
$('amiibo-sort').addEventListener('click',()=>{amiiboDescending=!amiiboDescending;renderAmiiboItems();});
// Case and accents don't count when searching, so "pinon" finds "Piñón".
const amiiboFold=text=>text.normalize('NFD').replace(/\p{M}/gu,'').toLowerCase();
const amiiboReason=error=>amiiboWords[error.message]||words[error.message]||words.interrupted;
const amiiboBody=file=>file.arrayBuffer().then(body=>({method:'POST',headers:{'Content-Type':'application/octet-stream'},body}));
async function loadAmiibo(){
 try{
  const data=await(await api('/api/v1/amiibo')).json(),first=!amiiboFiles;amiiboFiles=data.files;amiiboKey=data.key;
  // With nothing to manage yet, the first thing to do is adding some.
  if(first&&!amiiboFiles.length)choose(amiiboPanes,'amiibo-tab-add');
  renderAmiiboItems();render();
 }catch(error){$('amiibo-note').textContent=amiiboReason(error);}
}
function renderAmiiboItems(){
 if(!amiiboFiles)return;
 const search=amiiboFold($('amiibo-search').value.trim()),sign=amiiboDescending?-1:1;
 const shown=amiiboFiles.filter(file=>amiiboFold(file.name).includes(search)).sort((a,b)=>sign*a.name.localeCompare(b.name,undefined,{numeric:true,sensitivity:'base'}));
 $('amiibo-tools').hidden=!amiiboFiles.length;
 $('amiibo-sort').textContent=amiiboWords[amiiboDescending?'sort_down':'sort_up'];
 $('amiibo-count').textContent=!amiiboFiles.length?'':search?amiiboWords.count_of(shown.length,amiiboFiles.length):amiiboWords.count(amiiboFiles.length);
 $('amiibo-items').replaceChildren(...(shown.length?shown.map(file=>{
  const row=node('li'),text=node('span'),key='amiibo:'+file.name,button=node('button','danger',armed===key?libraryWords.confirm:libraryWords.remove);
  text.append(node('b','',file.name));
  button.addEventListener('click',()=>removeAmiibo(file.name));row.append(text,button);return row;
 }):[node('li','empty',amiiboWords[amiiboFiles.length?'no_match':'none'])]));
}
function renderAmiiboResults(){
 $('amiibo-results').replaceChildren(...amiiboResults.map(result=>{
  const row=node('li'),text=node('span');
  text.append(node('b','',result.file.name),node('small','',result.error?amiiboReason(Error(result.error)):amiiboWords.uploaded));row.append(text);
  if(result.error==='exists'){const button=node('button','danger',amiiboWords.replace);button.disabled=amiiboSending;button.addEventListener('click',()=>replaceAmiibo(result));row.append(button);}
  return row;
 }));
}
function renderAmiibo(){
 const on=!!session&&!!state.amiibo;
 $('tab-amiibo').hidden=!on;if(!on&&!$('view-amiibo').hidden)choose(tabs,'tab-games');
 if(!on||$('view-amiibo').hidden)return;
 $('amiibo-send').disabled=amiiboSending;$('amiibo-key-send').disabled=amiiboSending;
 $('amiibo-key-status').textContent=amiiboWords[amiiboKey?'key_on':'key_off'];
 $('amiibo-key-send').textContent=amiiboWords[amiiboKey?'key_replace':'key_send'];
 $('amiibo-key-remove').hidden=!amiiboKey;$('amiibo-key-remove').disabled=amiiboSending;
 $('amiibo-key-remove').textContent=armed==='amiibo-key'?libraryWords.confirm:libraryWords.remove;
}
// Refused here when it can't fit: the console would refuse it anyway, with a less useful word.
async function sendAmiibo(file,replace){
 if(file.size>4096)return 'not_amiibo';
 try{await api('/api/v1/amiibo/files?name='+encodeURIComponent(file.name)+(replace?'&replace=1':''),await amiiboBody(file));return '';}
 catch(error){return error.message;}
}
async function replaceAmiibo(result){
 if(amiiboSending)return;amiiboSending=true;renderAmiiboResults();render();
 result.error=await sendAmiibo(result.file,true);
 amiiboSending=false;renderAmiiboResults();await loadAmiibo();
}
async function removeAmiibo(name){
 if(!arm('amiibo:'+name))return;
 try{await api('/api/v1/amiibo/files?name='+encodeURIComponent(name),{method:'DELETE'});$('amiibo-note').textContent='';}
 catch(error){$('amiibo-note').textContent=amiiboReason(error);}
 await loadAmiibo();
}
$('amiibo-files').addEventListener('change',()=>picked('amiibo-picked','amiibo-files'));
$('amiibo-key-file').addEventListener('change',()=>picked('amiibo-key-picked','amiibo-key-file'));
$('amiibo-send').addEventListener('click',async()=>{
 const list=Array.from($('amiibo-files').files||[]);$('amiibo-note').textContent=list.length?'':amiiboWords.choose;
 if(!list.length||amiiboSending)return;
 amiiboSending=true;amiiboResults=[];render();
 for(const file of list){const result={file,error:await sendAmiibo(file,false)};amiiboResults.push(result);renderAmiiboResults();}
 amiiboSending=false;renderAmiiboResults();$('amiibo-files').value='';$('amiibo-picked').textContent='';await loadAmiibo();
});
$('amiibo-key-send').addEventListener('click',async()=>{
 const file=($('amiibo-key-file').files||[])[0];$('amiibo-note').textContent=file?'':amiiboWords.choose_key;
 if(!file||amiiboSending)return;
 if(file.size>4096){$('amiibo-note').textContent=amiiboWords.not_key;return;}
 amiiboSending=true;render();
 try{await api('/api/v1/amiibo/key',await amiiboBody(file));$('amiibo-note').textContent=amiiboWords.key_saved;$('amiibo-key-file').value='';$('amiibo-key-picked').textContent='';}
 catch(error){$('amiibo-note').textContent=amiiboReason(error);}
 amiiboSending=false;await loadAmiibo();
});
$('amiibo-key-remove').addEventListener('click',async()=>{
 if(!arm('amiibo-key'))return;
 try{await api('/api/v1/amiibo/key',{method:'DELETE'});$('amiibo-note').textContent=amiiboWords.key_removed;}
 catch(error){$('amiibo-note').textContent=amiiboReason(error);}
 await loadAmiibo();
});
$('game-back').addEventListener('click',()=>{gameId='';armed='';render();});
$('kind-update').addEventListener('click',()=>chooseKind('update'));$('kind-dlc').addEventListener('click',()=>chooseKind('dlc'));
$('kind-save').addEventListener('click',()=>chooseKind('save'));
$('save-help-toggle').addEventListener('click',()=>showSaveHelp($('save-help').hidden,false));
$('save-error-help').addEventListener('click',()=>showSaveHelp(true,true));
$('save-file').addEventListener('change',()=>{picked('save-picked','save-file');clearSaveError();});
// Fetched with the session header, then handed to the browser as a file.
$('save-download').addEventListener('click',async()=>{
 if(!gameId||saveDownloading)return;const id=gameId;
 saveDownloading=true;clearSaveError();$('library-note').textContent='';$('message').textContent='';render();
 try{
  const blob=await(await api('/api/v1/library/'+id+'/save')).blob(),url=URL.createObjectURL(blob),link=document.createElement('a');
  link.href=url;link.download=id.toUpperCase()+'-save.zip';document.body?.append(link);link.click();link.remove?.();
  setTimeout(()=>URL.revokeObjectURL(url),60000);$('library-note').textContent=saveWords.downloaded;
 }catch(error){saveError(error,true);}finally{saveDownloading=false;render();}
});
$('save-send').addEventListener('click',async()=>{
 if(libraryBusy()||!gameId)return;const file=$('save-file').files[0];clearSaveError();$('library-note').textContent='';$('message').textContent='';
 if(!file){saveError(Error('choose'));return;}
 const problem=await checkSave(file);if(problem){saveError(Error(problem));return;}
 contentRunning=true;stopped=false;rates=[];const target=contentTarget();render();
 try{await sendFile(file,file.name,target);$('library-note').textContent=saveWords.loaded;$('save-file').value='';$('save-picked').textContent='';}
 catch(error){
  saveError(error);
  // A save the console refused is not resumed: it is dropped so the next file can go.
  const s=state.content||{};
  if(s.id&&s.error&&s.phase==='paused'&&s.tag===target.tag)try{await api(target.base+'/'+s.id,{method:'DELETE'});}catch{}
 }finally{contentRunning=false;currentXHR=null;try{await refresh();}catch{}render();}
});
$('game-remove').addEventListener('click',()=>remove(gameId,'game'));
$('content-files').addEventListener('change',()=>picked('content-picked','content-files'));
$('content-send').addEventListener('click',async()=>{
 if(libraryBusy()||!gameId)return;const files=Array.from($('content-files').files);if(!files.length){$('message').textContent=words.choose;return;}
 contentRunning=true;stopped=false;rates=[];$('message').textContent='';$('library-note').textContent='';const target=contentTarget();render();
 try{for(const file of files){if(stopped)break;await sendFile(file,file.name,target);}await loadContents();}
 catch(error){$('message').textContent=libraryReason(error);}finally{contentRunning=false;currentXHR=null;try{await refresh();}catch{}render();}
});
$('content-pause').addEventListener('click',()=>control('pause',contentTarget()));$('content-discard').addEventListener('click',()=>control('discard',contentTarget()));
const systemWords=pt?{
 keysTitle:'Importar chaves',firmware:'Importar firmware',keysHelp:'ZIP ou prod.keys / title.keys. Máximo de 4 MiB por arquivo de chaves.',
 firmwareHelp:'ZIP ou arquivos NCA extraídos. A versão será verificada antes de instalar.',folder:'Ou escolha uma pasta extraída',send:'Enviar e revisar',confirm:'Confirmar instalação',cancel:'Cancelar e apagar importação',accept:'Entendo e aceito os avisos.',
 systemTitle:'Chaves e firmware',systemIntro:'Tudo é verificado antes de instalar. Os arquivos atuais ficam até você confirmar.',pick:'Selecionar arquivos',drop:'ou arraste-os para cá',
 installed:'Instalado',selected:'Selecionado',generation:'Geração das chaves (índice)',required:'Geração exigida pelo firmware',ready:'Arquivos validados. Revise as versões e confirme.',review:'Revise as versões e confirme.',
 firmware_downgrade:'O firmware selecionado é anterior ao instalado.',keys_downgrade:'As novas chaves perdem gerações disponíveis nas chaves instaladas.',keys_firmware_old:'As chaves podem não abrir o firmware instalado. Continuar pode deixá-lo inutilizável.',keys_unknown:'Não foi possível verificar a geração das chaves.',installed_unknown:'Não foi possível verificar o firmware instalado.',firmware_games_removed:'Arquivos de jogo encontrados no pacote de firmware foram deixados de fora. Só o firmware será instalado.',
 receiving:'Recebendo arquivos',extracting:'Extraindo ZIP',validating:'Validando arquivos',prepare_pending:'Aguardando validação',commit_pending:'Aguardando instalação',publishing:'Instalando',complete:'Instalação concluída',failed:'A importação falhou; os dados anteriores foram preservados.',idle:'Escolha arquivos para importar.',
 archive:'O ZIP está danificado ou não é compatível.',archive_unsafe:'O ZIP contém caminhos perigosos, links ou arquivos criptografados.',archive_limit:'O arquivo ultrapassa os limites de tamanho ou quantidade.',file_limit:'Quantidade de arquivos inválida.',file:'Use somente ZIP ou os arquivos correspondentes, sem misturá-los.',keys:'Arquivo de chaves inválido.',keys_header:'Falta uma header_key válida em prod.keys.',keys_limit:'Cada arquivo de chaves deve ter no máximo 4 MiB.',firmware_keys:'As chaves instaladas não conseguem ler o firmware. Importe chaves compatíveis primeiro.',firmware_keys_newer:'Este firmware é mais novo que as chaves instaladas. Importe primeiro um prod.keys de um console com este firmware ou mais recente.',firmware_incomplete:'Firmware incompleto: falta SystemVersion ou MiiEdit.',firmware_game:'Este pacote contém arquivos de jogo.',firmware_duplicate:'O pacote contém nomes ou versões duplicados.',firmware_version:'Não foi possível verificar uma versão de firmware NX válida.',confirm_warnings:'Aceite os avisos antes de instalar.',recovery:'A importação interrompida não pôde ser recuperada. Apague-a e selecione os arquivos novamente.'
}:es?{
 keysTitle:'Importar claves',firmware:'Importar firmware',keysHelp:'ZIP o prod.keys / title.keys. Máximo de 4 MiB por archivo de claves.',
 firmwareHelp:'ZIP o archivos NCA extraídos. La versión se comprobará antes de instalar.',folder:'O elige una carpeta extraída',send:'Subir y revisar',confirm:'Confirmar instalación',cancel:'Cancelar y eliminar importación',accept:'Entiendo y acepto las advertencias.',
 systemTitle:'Claves y firmware',systemIntro:'Todo se comprueba antes de instalar. Tus archivos actuales se conservan hasta que confirmes.',pick:'Seleccionar archivos',drop:'o arrástralos aquí',
 installed:'Instalado',selected:'Seleccionado',generation:'Generación de claves (índice)',required:'Generación requerida por el firmware',ready:'Archivos validados. Revisa las versiones y confirma.',review:'Revisa las versiones y confirma.',
 firmware_downgrade:'El firmware seleccionado es anterior al instalado.',keys_downgrade:'Las nuevas claves pierden generaciones disponibles en las instaladas.',keys_firmware_old:'Las claves podrían no abrir el firmware instalado. Continuar puede dejarlo inutilizable.',keys_unknown:'No se pudo verificar la generación de las claves.',installed_unknown:'No se pudo verificar el firmware instalado.',firmware_games_removed:'Se quitaron archivos de juego que venían en el paquete de firmware. Solo se instalará el firmware.',
 receiving:'Recibiendo archivos',extracting:'Extrayendo ZIP',validating:'Validando archivos',prepare_pending:'Esperando validación',commit_pending:'Esperando instalación',publishing:'Instalando',complete:'Instalación completada',failed:'La importación falló; se conservaron los datos anteriores.',idle:'Selecciona archivos para importar.',
 archive:'El ZIP está dañado o no es compatible.',archive_unsafe:'El ZIP contiene rutas peligrosas, enlaces o archivos cifrados.',archive_limit:'El archivo supera los límites de tamaño o cantidad.',file_limit:'Cantidad de archivos no válida.',file:'Usa solo ZIP o los archivos correspondientes, sin mezclarlos.',keys:'Archivo de claves no válido.',keys_header:'Falta una header_key válida en prod.keys.',keys_limit:'Cada archivo de claves debe ocupar como máximo 4 MiB.',firmware_keys:'Las claves instaladas no pueden leer el firmware. Importa antes claves compatibles.',firmware_keys_newer:'Este firmware es más nuevo que las claves instaladas. Importa antes un prod.keys de una consola con este firmware o uno más reciente.',firmware_incomplete:'Firmware incompleto: falta SystemVersion o MiiEdit.',firmware_game:'El paquete contiene archivos de juego.',firmware_duplicate:'El paquete contiene nombres o versiones duplicados.',firmware_version:'No se pudo verificar una versión de firmware NX válida.',confirm_warnings:'Acepta las advertencias antes de instalar.',recovery:'No se pudo recuperar la importación interrumpida. Elimínala y vuelve a seleccionar los archivos.'
}:{
 keysTitle:'Import keys',firmware:'Import firmware',keysHelp:'ZIP or prod.keys / title.keys. Maximum 4 MiB per key file.',firmwareHelp:'ZIP or extracted NCA files. The version is checked before installation.',folder:'Or choose an extracted folder',send:'Upload and review',confirm:'Confirm installation',cancel:'Cancel and discard import',accept:'I understand and accept these warnings.',
 systemTitle:'Keys and firmware',systemIntro:'Everything is checked before installing. Your current files stay until you confirm.',pick:'Choose files',drop:'or drop them here',
 installed:'Installed',selected:'Selected',generation:'Key generation (index)',required:'Firmware required generation',ready:'Files validated. Review versions and confirm.',review:'Review versions and confirm.',firmware_downgrade:'The selected firmware is older than the installed firmware.',keys_downgrade:'The new keys lose generations present in the installed keys.',keys_firmware_old:'These keys may not open the installed firmware. Continuing may make it unusable.',keys_unknown:'The key generation could not be verified.',installed_unknown:'The installed firmware could not be verified.',firmware_games_removed:'Game files found in the firmware package were left out. Only the firmware will be installed.',
 receiving:'Receiving files',extracting:'Extracting ZIP',validating:'Validating files',prepare_pending:'Waiting for validation',commit_pending:'Waiting for installation',publishing:'Installing',complete:'Installation complete',failed:'Import failed; previous data was preserved.',idle:'Choose files to import.',archive:'The ZIP is damaged or unsupported.',archive_unsafe:'The ZIP contains unsafe paths, links or encrypted entries.',archive_limit:'The size or file count limit was exceeded.',file_limit:'Invalid file count.',file:'Use only a ZIP or the appropriate loose files, without mixing formats.',keys:'Invalid key file.',keys_header:'A valid header_key is required in prod.keys.',keys_limit:'Each key file must be at most 4 MiB.',firmware_keys:'Installed keys cannot read this firmware. Import compatible keys first.',firmware_keys_newer:'This firmware is newer than the installed keys. Import a prod.keys from a console on this firmware or a later one first.',firmware_incomplete:'Incomplete firmware: SystemVersion or MiiEdit is missing.',firmware_game:'The package contains game files.',firmware_duplicate:'The package contains duplicate names or versions.',firmware_version:'A valid NX firmware version could not be verified.',confirm_warnings:'Accept warnings before installing.',recovery:'The interrupted import could not be recovered. Discard it and select the files again.'};
for(const [id,text]of Object.entries({'keys-title':systemWords.keysTitle,'firmware-title':systemWords.firmware,'keys-help':systemWords.keysHelp,'firmware-help':systemWords.firmwareHelp,
 'firmware-folder-label':systemWords.folder,'keys-send':systemWords.send,'firmware-send':systemWords.send,'system-confirm':systemWords.confirm,'system-cancel':systemWords.cancel,'system-warning-label':systemWords.accept,
 'system-title':systemWords.systemTitle,'system-intro':systemWords.systemIntro,'keys-pick':systemWords.pick,'firmware-pick':systemWords.pick,'keys-drop':systemWords.drop,'firmware-drop':systemWords.drop}))$(id).textContent=text;
function systemReason(error){return systemWords[error.message]||words[error.message]||systemWords.failed;}
function renderSystem(){
 const s=state.system||{},r=s.review||{},warnings=r.warnings||[];
 const supported=!!state.system_supported,logs=!!state.logs?.supported;
 for(const kind of ['keys','firmware','logs'])$('cat-'+kind).ariaSelected=String(kind===systemPane);
 $('cat-keys').hidden=$('cat-firmware').hidden=!supported;$('cat-logs').hidden=!logs;
 $('keys-pane').hidden=systemPane!=='keys';$('firmware-pane').hidden=systemPane!=='firmware';
 $('system-upload').hidden=!session||!supported||systemPane==='logs';
 $('tab-system').hidden=!supported&&!logs;
 if(!supported){if($('tab-system').hidden&&!$('view-system').hidden)choose(tabs,'tab-games');return;}
 for(const kind of ['keys','firmware'])$(kind+'-send').disabled=running||systemRunning||state.transfer?.busy||!s.available||(s.busy&&s.kind!==kind)||!['idle','receiving','complete'].includes(s.phase);
 const job=s.phase==='complete'&&!systemRunning?{}:s;
 $('system-job').textContent=(systemWords[job.phase]||systemWords.idle)+(job.count?' · '+(job.completed||0)+' / '+job.count:'')+(job.error?'\n'+systemReason(Error(job.error)):'');
 const f=job.file||{};$('system-progress').value=f.length?Math.min(1,(f.received||0)/f.length):0;$('system-percent').textContent=percent(f.received||0,f.length);
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
 $('system-cancel').hidden=$('system-cancel').disabled;
 $('system-panel').hidden=!systemRunning&&!job.error&&!(s.id&&s.phase!=='complete');
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
$('keys-files').addEventListener('change',()=>picked('keys-picked','keys-files'));
$('firmware-files').addEventListener('change',()=>{$('firmware-folder').value='';picked('firmware-picked','firmware-files');});
$('firmware-folder').addEventListener('change',()=>{$('firmware-files').value='';picked('firmware-picked','firmware-folder');});
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

// Export logs to a file download; never fetch multi-GiB archives into a browser Blob. The
// download starts with the export and follows it while it compresses; the link stays for a retry.
let logsWaiting=false,logsRequesting=false;
const logWords=pt?{title:'Logs e capturas',help:'Baixe toda a pasta de logs, incluindo as capturas gráficas, em um arquivo comprimido .tar.zst. Mantenha o Jadyx na biblioteca.',
 download:'Comprimir e baixar logs',cancel:'Cancelar',save:'Salvar arquivo',scanning:'Preparando arquivos…',compressing:'Comprimindo e enviando',ready:'Arquivo pronto. Se o download não começar, clique em Salvar arquivo.',cancelled:'Compressão cancelada.',failed:'Não foi possível preparar os logs. Os originais foram preservados.',space:'Não há espaço livre suficiente para preparar o arquivo.',read:'Um arquivo de log mudou ou não pôde ser lido. Tente novamente.',saving:'Iniciando download…'}:es?{
 title:'Logs y capturas',help:'Descarga toda la carpeta de logs, incluidas las capturas gráficas, en un archivo comprimido .tar.zst. Mantén Jadyx en la biblioteca.',
 download:'Comprimir y descargar logs',cancel:'Cancelar',save:'Guardar archivo',scanning:'Preparando archivos…',compressing:'Comprimiendo y enviando',ready:'Archivo listo. Si no empieza la descarga, pulsa Guardar archivo.',cancelled:'Compresión cancelada.',failed:'No se pudieron preparar los logs. Los originales se conservan.',space:'No hay espacio libre suficiente para preparar el archivo.',read:'Un archivo de log cambió o no se pudo leer. Vuelve a intentarlo.',saving:'Iniciando descarga…'}:{
 title:'Logs and captures',help:'Download the entire log folder, including graphics captures, as a compressed .tar.zst file. Keep Jadyx in the library.',
 download:'Compress and download logs',cancel:'Cancel',save:'Save file',scanning:'Preparing files…',compressing:'Compressing and sending',ready:'Archive ready. If downloading does not start, click Save file.',cancelled:'Compression cancelled.',failed:'Could not prepare logs. Original files were preserved.',space:'Not enough free space to prepare the archive.',read:'A log file changed or could not be read. Try again.',saving:'Starting download…'};
$('logs-title').textContent=logWords.title;$('logs-help').textContent=logWords.help;
$('logs-download').textContent=logWords.download;$('logs-cancel').textContent=logWords.cancel;
$('logs-file').textContent=logWords.save;
async function saveLogs(){
 const response=await api('/api/v1/logs/download',{method:'POST'}),data=await response.json();
 if(typeof data.download!=='string'||!/^\/api\/v1\/logs\/archive\?ticket=[0-9a-f]{64}$/.test(data.download)||
    typeof data.filename!=='string'||!/^jadyx-logs-[0-9]{8}-[0-9]{6}\.tar\.zst$/.test(data.filename))throw Error('download');
 const link=$('logs-file');link.href=data.download;link.download=data.filename;link.hidden=false;link.click();
}
function renderLogs(){
 const s=state.logs||{};$('log-export').hidden=!session||!s.supported||systemPane!=='logs';
 if(!session||!s.supported)return;
 $('logs-download').disabled=!!s.busy||logsRequesting;$('logs-cancel').hidden=!s.busy;
 $('logs-progress').hidden=s.phase!=='compressing';
 $('logs-progress').value=s.total?Math.min(1,s.processed/s.total):0;
 $('logs-status').textContent=s.phase==='compressing'?logWords.compressing+' · '+size(s.processed||0)+' / '+size(s.total||0)+' · '+size(s.compressed||0):
  s.phase==='failed'?(logWords[s.error]||logWords.failed):(logWords[s.phase]||'');
 if(!s.busy&&s.phase!=='ready')$('logs-file').hidden=true;
 if(logsWaiting&&s.phase==='ready'){
  logsWaiting=false;logsRequesting=true;
  saveLogs().catch(()=>{$('logs-status').textContent=logWords.failed;}).finally(()=>{logsRequesting=false;$('logs-download').disabled=false;});
 }
 if(s.phase==='failed'||s.phase==='cancelled')logsWaiting=false;
}
$('logs-download').addEventListener('click',async()=>{
 logsRequesting=true;$('logs-download').disabled=true;$('logs-file').hidden=true;
 try{await api('/api/v1/logs/export',{method:'POST'});
  try{await saveLogs();}catch{logsWaiting=true;}
  await refresh();}
 catch(error){$('logs-status').textContent=logWords[error.message]||logWords.failed;}
 finally{logsRequesting=false;$('logs-download').disabled=!!state.logs?.busy;}
});
$('logs-cancel').addEventListener('click',async()=>{
 logsWaiting=false;try{await api('/api/v1/logs/export',{method:'DELETE'});await refresh();}
 catch{$('logs-status').textContent=logWords.failed;}
});

// Guided app update: while Jadyx offers one, the page opens on its steps. The ticket is fetched
// ahead of the click (it lives two minutes and serves once) so the download and the Device Portal
// tab both start inside the click, where browsers still allow opening a tab.
const uw=pt?{kicker:'Atualização do Jadyx',title:'Atualizar Jadyx',preparing:'Preparando o pacote verificado…',verified:'Assinatura verificada',
 s1:'Conecte o navegador',s1b:'Feito: este navegador está conectado ao console.',s2:'Baixe e instale',s2b:'O pacote é baixado e o Xbox Device Portal se abre em outra aba.',
 go:'Baixar e instalar',again:'Baixar de novo',s3:'Conclua no Device Portal',
 s3b:'Se aparecer um aviso de certificado, escolha Avançado → Continuar. Depois Add → escolha o arquivo baixado → Next → Install.',portal:'Abrir Device Portal',
 keep:'Não desinstale o Jadyx: isso apaga seus saves e configurações. Instale por cima.',skip:'Ir para o conteúdo',
 installing:'Instalando a atualização',installingb:'O Jadyx fechou para ser instalado. Quando o Device Portal terminar, abra-o no console: ele confirmará a nova versão.',
 reconnect:'Conectar de novo',banner:'Atualização do Jadyx pronta',bannerb:'Instale-a por cima pelo Xbox Device Portal.',open:'Ver passos',
 busy:'Aguarde o fim das transferências ativas.',verify_in_app:'Abra Atualizações no console e verifique o pacote de novo.',failed:'Não foi possível preparar o pacote. Tente de novo.'}
:es?{kicker:'Actualización de Jadyx',title:'Actualizar Jadyx',preparing:'Preparando el paquete verificado…',verified:'Firma verificada',
 s1:'Conecta el navegador',s1b:'Hecho: este navegador está conectado a la consola.',s2:'Descarga e instala',s2b:'Se descarga el paquete y se abre Xbox Device Portal en otra pestaña.',
 go:'Descargar e instalar',again:'Descargar de nuevo',s3:'Termina en Device Portal',
 s3b:'Si avisa del certificado, elige Avanzado → Continuar. Luego Add → elige el archivo descargado → Next → Install.',portal:'Abrir Device Portal',
 keep:'No desinstales Jadyx: borraría tus partidas y ajustes. Instala encima.',skip:'Ir al contenido',
 installing:'Instalando la actualización',installingb:'Jadyx se cerró para instalarse. Cuando Device Portal termine, ábrelo en la consola: confirmará la versión nueva.',
 reconnect:'Conectar de nuevo',banner:'Actualización de Jadyx lista',bannerb:'Instálala encima desde Xbox Device Portal.',open:'Ver pasos',
 busy:'Espera a que terminen las transferencias activas.',verify_in_app:'Abre Actualizaciones en la consola y vuelve a verificar el paquete.',failed:'No se pudo preparar el paquete. Inténtalo de nuevo.'}
:{kicker:'Jadyx update',title:'Update Jadyx',preparing:'Preparing the verified package…',verified:'Signature verified',
 s1:'Connect the browser',s1b:'Done: this browser is connected to the console.',s2:'Download and install',s2b:'The package downloads and Xbox Device Portal opens in another tab.',
 go:'Download and install',again:'Download again',s3:'Finish in Device Portal',
 s3b:'If it warns about the certificate, choose Advanced → Continue. Then Add → choose the downloaded file → Next → Install.',portal:'Open Device Portal',
 keep:'Do not uninstall Jadyx: that deletes your saves and settings. Install over it.',skip:'Go to content',
 installing:'Installing the update',installingb:'Jadyx closed to be installed. When Device Portal finishes, open it on the console: it confirms the new version.',
 reconnect:'Connect again',banner:'Jadyx update ready',bannerb:'Install it over Jadyx from Xbox Device Portal.',open:'View steps',
 busy:'Wait for active transfers to finish.',verify_in_app:'Open Updates on the console and verify the package again.',failed:'Could not prepare the package. Try again.'};
for(const [id,key]of [['update-kicker','kicker'],['update-title','title'],['update-verified','verified'],['update-s1','s1'],['update-s1b','s1b'],['update-s2','s2'],
 ['update-s2b','s2b'],['update-s3','s3'],['update-s3b','s3b'],['update-portal-label','portal'],['update-keep','keep'],['update-skip','skip'],
 ['update-installing-title','installing'],['update-installing-body','installingb'],['update-reconnect','reconnect'],['update-banner-title','banner'],
 ['update-banner-body','bannerb'],['update-banner-open','open'],['update-go','go']])$(id).textContent=uw[key];
const portalUrl=location.hostname?'https://'+location.hostname+':11443':'';
if(portalUrl)$('update-portal').href=portalUrl;
let ticket=null,ticketAt=0,ticketing=false,ticketError='',release=null,updateDismissed=false,updateStarted=false,updateGone=false,failures=0;
async function fetchTicket(){
 ticketing=true;
 try{
  const data=await (await api('/api/v1/update/download',{method:'POST'})).json();
  // Keep the navigation same-origin even if a future server response is malformed.
  if(typeof data.download!=='string'||!/^\/api\/v1\/update\/package\?ticket=[0-9a-f]{64}$/.test(data.download)||
     typeof data.filename!=='string'||!/^Jadyx_[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+_x64\.appx$/.test(data.filename))throw Error('failed');
  ticket=release=data;ticketError='';
 }catch(error){ticket=null;ticketError=error.message==='lost'?'':(uw[error.message]||uw.failed);}
 finally{ticketAt=Date.now();ticketing=false;renderUpdate();}
}
function renderUpdate(){
 const offered=updateGone||!!(session&&state.update?.offered);
 document.body.classList.toggle('updating',offered&&!updateDismissed);
 $('update-banner').hidden=!offered||!updateDismissed||updateGone;
 if(!offered)return;
 const age=Date.now()-ticketAt;
 if(session&&!updateGone&&!updateDismissed&&!ticketing&&(ticket?age>90000:age>(ticketError?5000:0)))fetchTicket();
 $('update-title').textContent=release?'Jadyx '+release.version:uw.title;
 $('update-meta').textContent=ticketError&&!ticket?ticketError:release?release.filename+' · '+size(release.size):uw.preparing;
 $('update-badge').hidden=!release;
 $('update-go').disabled=!ticket;$('update-go').textContent=updateStarted?uw.again:uw.go;
 $('update-go').classList.toggle('primary',!updateStarted);
 $('update-step-2').className=updateStarted?'done':'now';$('update-step-3').className=updateStarted?'now':'';
 $('update-steps').hidden=$('update-skip').hidden=updateGone;$('update-installing').hidden=!updateGone;
}
$('update-go').addEventListener('click',()=>{
 if(!ticket)return;
 const link=$('update-package');link.href=ticket.download;link.download=ticket.filename;link.click();
 if(portalUrl)window.open(portalUrl,'_blank','noopener');
 updateStarted=true;ticket=null;ticketAt=0;renderUpdate();
});
$('update-skip').addEventListener('click',()=>{updateDismissed=true;renderUpdate();scrollTo(0,0);});
$('update-banner-open').addEventListener('click',()=>{updateDismissed=false;renderUpdate();scrollTo(0,0);});
$('update-reconnect').addEventListener('click',()=>location.reload());
</script></body></html>)HTML";
}
