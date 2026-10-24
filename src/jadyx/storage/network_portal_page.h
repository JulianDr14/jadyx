// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
namespace Jadyx::PortalWeb {
// The page is laid out like the console frontend: the wordmark and segmented tabs in the header,
// and each tab is a Settings-style page, a list of categories beside the chosen one's pane; the
// library's games open into their updates/DLC. Palette, Inter and capsule buttons follow
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
.view{display:grid;grid-template-columns:288px minmax(0,1fr);gap:32px;align-items:start}
.cats{position:sticky;top:116px;display:flex;flex-direction:column;gap:6px}.cats .kicker{margin:0 0 10px 16px}
button.cat{display:flex;align-items:center;gap:14px;width:100%;height:auto;min-height:64px;padding:10px 16px;border-radius:14px;background:none;border-color:transparent;
 color:var(--hint);text-align:left;font-weight:400}
.cat span{flex:1;min-width:0}.cat b{display:block;font-size:15px;font-weight:600}.cat small{display:block;font-size:13px;margin-top:2px}
.cat .chev{width:16px;height:16px;opacity:.5}button.cat:hover:not(:disabled){background:var(--row)}
button.cat[aria-selected=true]{background:var(--active);border-color:rgba(255,255,255,.14);color:var(--text)}.cat[aria-selected=true] small{color:var(--hint)}
button.cat:disabled{opacity:1;color:var(--muted)}.cat:disabled .ico{opacity:.6}
.tag{font-size:10px;line-height:20px;font-style:normal;font-weight:600;letter-spacing:.08em;text-transform:uppercase;padding:0 8px;border-radius:999px;border:1px solid var(--line);color:var(--muted);white-space:nowrap}
.store{display:flex;align-items:center;gap:14px;margin-top:16px;padding:16px;border-radius:16px;background:var(--surface);border:1px solid var(--line);color:var(--hint)}
#space{font-size:14px;font-weight:600;color:var(--text);font-variant-numeric:tabular-nums}#space:empty::before{content:"—"}
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
footer{display:flex;align-items:center;justify-content:center;gap:8px;padding:24px 16px 32px;font-size:13px;color:var(--muted)}footer .ico{width:16px;height:16px}
body.updating .tabs{visibility:hidden}body.updating #view-content,body.updating #view-system,body.updating #log-export{display:none!important}
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
#update-banner .panel-top{align-items:center;margin:0}#update-banner p{margin-top:4px}
@media(max-width:860px){.bar{grid-template-columns:minmax(0,1fr) auto;padding:12px 16px;row-gap:12px}.tabs{grid-column:1/-1}.tabs button{flex:1}
 .shell{padding:20px 16px 32px}.view{grid-template-columns:minmax(0,1fr);gap:16px}
 .cats{position:static;flex-direction:row;overflow-x:auto;scrollbar-width:none;margin:0 -16px;padding:0 16px}.cats::-webkit-scrollbar{display:none}
 .cats .kicker,.cat .chev,.cat small{display:none}button.cat{flex:none;width:auto;min-height:48px;padding:8px 16px;background:var(--row)}
 .store{flex:none;margin:0;padding:8px 16px}#message{top:132px}
 .grid{grid-template-columns:repeat(auto-fill,minmax(120px,1fr))}.hero{grid-template-columns:72px minmax(0,1fr)}.hero .art{width:72px}.hero .danger{grid-column:1/-1}
 .card{padding:22px}h1{font-size:28px;line-height:34px}.actions button{flex:1 1 calc(50% - 10px)}#code{font-size:28px;letter-spacing:.28em;text-indent:.28em}}
@media(max-width:600px){.update-top{flex-direction:column}.steps li{gap:14px}.steps .actions>*{flex:1 1 100%;justify-content:center}}
@media(max-width:420px){.brand img{width:36px;height:36px}.word{font-size:16px;letter-spacing:.3em}.caps,.link span{display:none}.link{padding:0 11px}}
@media(prefers-reduced-motion:reduce){*{transition:none!important}}
</style>
<body><header class="bar"><div class="brand"><img src="/logo.png" alt="Jadyx" width="44" height="44"><span class="word">JADYX</span><span class="caps">Portal</span></div>
<nav class="tabs" role="tablist"><button id="tab-content" role="tab" aria-selected="true">Content</button><button id="tab-system" role="tab" aria-selected="false">System</button></nav>
<span class="link" role="status"><i></i><span class="status-off" id="status-off">Not connected</span><span class="status-on" id="status-on">Connected</span></span></header>
<div class="shell"><p id="message" role="alert"></p>
<main class="gate"><img src="/logo.png" alt="" width="132" height="132"><p class="kicker" id="kicker">Local network portal</p><h1 id="title">Connect to your console</h1>
<p id="intro">Enter the temporary code shown in Jadyx. Keep the app open in the library.</p>
<form id="pair"><label id="label" for="code">Connection code</label>
<input id="code" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{8}" maxlength="8" placeholder="00000000" required>
<button id="connect" class="primary">Connect</button><p class="hint" id="code-hint">8 digits · it changes every time you open the portal</p></form>
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
<div class="view" id="view-content"><nav class="cats"><p class="kicker" id="content-kicker">Install</p>
<button class="cat" id="cat-games" aria-selected="true"><svg class="ico" viewBox="0 0 24 24"><path d="M6.5 8h11a4 4 0 0 1 3.9 4.9l-1 4.4a2.4 2.4 0 0 1-4.2 1L14.5 16h-5l-1.7 2.3a2.4 2.4 0 0 1-4.2-1l-1-4.4A4 4 0 0 1 6.5 8z"/><path d="M8 11v3M6.5 12.5h3M15.5 12h.01M17.5 14h.01"/></svg>
<span><b id="cat-games-title">Games</b><small id="cat-games-sub">NSP · base game</small></span><svg class="ico chev" viewBox="0 0 24 24"><path d="m9 6 6 6-6 6"/></svg></button>
<button class="cat" id="cat-library" aria-selected="false" hidden><svg class="ico" viewBox="0 0 24 24"><rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/></svg>
<span><b id="cat-library-title">Library</b><small id="cat-library-sub">Updates · DLC · delete</small></span><svg class="ico chev" viewBox="0 0 24 24"><path d="m9 6 6 6-6 6"/></svg></button>
<div class="store"><svg class="ico" viewBox="0 0 24 24"><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M5 13 7.5 5h9L19 13M7 16.5h.01"/></svg><span id="space"></span></div></nav>
<section id="upload" class="card" hidden><div class="head"><h2 id="uploadtitle">Import NSP games</h2><p id="uploadintro">Base games only. Files are copied to your console's internal storage.</p></div>
<label class="drop" id="files-zone"><input id="files" type="file" accept=".nsp" multiple><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="fileslabel">Choose NSP files</strong><span id="files-drop">or drop them here</span><span class="picked" id="files-picked"></span></label>
<div class="field"><label id="namelabel" for="name">Destination name (one file)</label><input id="name" maxlength="100" autocomplete="off"></div>
<div class="actions"><button id="send" class="primary">Upload / resume</button><button id="pause">Pause</button><button id="rename">Rename pending file</button>
<button class="danger" id="discard">Discard partial file</button></div>
<div class="panel"><div class="panel-top"><p id="job" role="status" aria-live="polite"></p><span class="pct" id="percent"></span></div>
<progress id="progress" max="1" value="0"></progress><div class="stats"><p id="confirmed"></p><p id="rate"></p><p id="eta"></p><p id="queue"></p></div></div></section>
<section id="library" class="card" hidden><p id="library-note" role="status" aria-live="polite"></p>
<div id="library-list"><div class="head"><h2 id="library-title">Your library</h2><p id="library-intro">Open a game to manage its updates and DLC, or to delete it.</p></div>
<div class="grid" id="games"></div><p id="library-empty" hidden>No games yet.</p></div>
<div id="game" hidden><button class="back" id="game-back">← Library</button>
<div class="hero"><div class="art" id="game-art"></div><div><h2 id="game-name"></h2><p id="game-meta"></p></div><button class="danger" id="game-remove">Delete game</button></div>
<p id="game-warning"></p>
<nav class="seg" role="tablist"><button id="kind-update" role="tab" aria-selected="true">Updates</button><button id="kind-dlc" role="tab" aria-selected="false">DLC</button></nav>
<ul class="items" id="items"></ul>
<label class="drop"><input id="content-files" type="file" accept=".nsp" multiple><svg class="ico" viewBox="0 0 24 24"><path d="M12 15V4m0 0L8 8m4-4 4 4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg>
<strong id="content-pick">Choose update NSP files</strong><span id="content-drop">or drop them here</span><span class="picked" id="content-picked"></span></label>
<div class="actions"><button id="content-send" class="primary">Upload and install</button><button id="content-pause">Pause</button><button class="danger" id="content-discard">Discard partial file</button></div>
<div class="panel"><div class="panel-top"><p id="content-job" role="status" aria-live="polite"></p><span class="pct" id="content-percent"></span></div>
<progress id="content-progress" max="1" value="0"></progress><div class="stats"><p id="content-rate"></p><p id="content-eta"></p></div></div></div></section></div>
<div class="view" id="view-system" hidden><nav class="cats"><p class="kicker" id="system-kicker">System files</p>
<button class="cat" id="cat-keys" aria-selected="true"><svg class="ico" viewBox="0 0 24 24"><circle cx="8" cy="15" r="4"/><path d="M11 12l9-9M17 6l3 3M14 9l2 2"/></svg>
<span><b id="cat-keys-title">Keys</b><small>prod.keys · title.keys · ZIP</small></span><svg class="ico chev" viewBox="0 0 24 24"><path d="m9 6 6 6-6 6"/></svg></button>
<button class="cat" id="cat-firmware" aria-selected="false"><svg class="ico" viewBox="0 0 24 24"><rect x="7" y="7" width="10" height="10" rx="2"/><path d="M10 3v3M14 3v3M10 18v3M14 18v3M3 10h3M3 14h3M18 10h3M18 14h3"/></svg>
<span><b>Firmware</b><small>ZIP · NCA</small></span><svg class="ico chev" viewBox="0 0 24 24"><path d="m9 6 6 6-6 6"/></svg></button></nav>
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
<div class="panel"><div class="panel-top"><p id="system-job" role="status" aria-live="polite"></p><span class="pct" id="system-percent"></span></div><progress id="system-progress" max="1" value="0"></progress>
<div id="system-review" class="review" hidden><p id="system-versions"></p><p id="system-warnings"></p>
<label id="system-warning-row"><input id="system-warning-accept" type="checkbox"><span id="system-warning-label">I understand and accept these warnings.</span></label>
<button id="system-confirm" class="primary">Confirm installation</button></div></div><button id="system-cancel" class="danger">Cancel and discard system import</button></section></div>
</div><section class="panel" id="log-export" hidden>
<h2 id="logs-title"></h2><p id="logs-help"></p>
<div class="actions"><button id="logs-download" class="primary"></button><button id="logs-cancel" hidden></button>
<a id="logs-file" hidden></a></div><progress id="logs-progress" max="1" value="0" hidden></progress>
<p id="logs-status" role="status" aria-live="polite"></p>
</section><section class="panel" id="update-banner" hidden><div class="panel-top"><div><h3 id="update-banner-title"></h3><p id="update-banner-body"></p></div>
<button class="primary" id="update-banner-open"></button></div></section>
</div>
<footer><svg class="ico" viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 7.5-2"/></svg><span id="privacy">Local network · HTTP connection without encryption.</span></footer>
<script>
'use strict';
const $=id=>document.getElementById(id),lang=navigator.language.toLowerCase(),es=lang.startsWith('es'),pt=lang.startsWith('pt');
const words=pt?{storage_full:'Xbox sem espaço: {0} GB livres, precisa de {1} GB. Libere espaço e tente novamente.',update_route:'Atualização detectada. Abra Biblioteca > seu jogo > Atualizações e envie o NSP ali.',dlc_route:'DLC detectado. Abra Biblioteca > seu jogo > DLC e envie o NSP ali.',stalled:'Sem progresso há 60 s. Arquivo: ',keys_older_version:'Suas prod.keys são anteriores ao firmware {0}; extraia chaves de um console com {0} ou mais recente.',firmware_keys_old:'Suas prod.keys são anteriores ao firmware selecionado; extraia chaves de um console com esse firmware ou mais recente. A versão não pôde ser lida.',title_keys:'title.keys contém chaves por título para suas cópias criptografadas com tickets; não substitui prod.keys.',
 title:'Conecte-se ao seu console',intro:'Digite o código temporário que o Jadyx mostra. Mantenha o app aberto na biblioteca.',label:'Código de conexão',connect:'Conectar',
 uploadtitle:'Importar jogos NSP',uploadintro:'Somente jogos base. Os arquivos são copiados para o armazenamento interno do console.',fileslabel:'Selecionar arquivos NSP',
 namelabel:'Nome de destino (um único arquivo)',send:'Enviar / retomar',rename:'Renomear arquivo pendente',pause:'Pausar',discard:'Apagar arquivo parcial',
 kicker:'Portal da rede local','status-off':'Desconectado','status-on':'Conectado','code-hint':'8 dígitos · muda cada vez que você abre o portal','files-drop':'ou arraste-os para cá',files:'arquivos',
 privacy:'Rede local · Conexão HTTP sem criptografia.',ok:'Conexão estabelecida.',bad:'Código incorreto ou expirado. Gere outro no Jadyx.',
 lost:'A conexão foi encerrada. Conecte-se de novo pelo Jadyx; o progresso confirmado é mantido.',free:'Espaço livre',confirmed:'Confirmado',
 speed:'Velocidade',eta:'Tempo restante',queue:'Na fila',choose:'Selecione um ou mais arquivos NSP.',same:'Há outro arquivo pendente. Selecione o mesmo arquivo para retomá-lo ou apague a transferência parcial.',
 receiving:'Recebendo',paused:'Pausado. Selecione o mesmo arquivo para continuar.',ready:'Aguardando validação',validating:'Validando NSP',publishing:'Adicionando à biblioteca',complete:'Instalado na biblioteca',
 busy:'Aguarde o fim da operação no Jadyx.',exists:'Esse nome já existe. Altere o nome de destino e tente de novo.',space:'Não há espaço suficiente.',
 file:'Nome ou arquivo inválido. Selecione um NSP.',base:'Somente jogos base são aceitos; não atualizações, DLC nem pacotes mistos.',
 nsp:'A estrutura do NSP é inválida.',incomplete:'O NSP está incompleto.',keys:'Não foi possível ler os metadados. Importe prod.keys do seu console com o firmware exigido pelo jogo ou mais recente e confira o NSP.',
 disk:'Não foi possível gravar ou publicar o arquivo.',interrupted:'Transferência interrompida. Você pode retomar a partir do progresso confirmado.',
 partial:'Há uma transferência pendente. Retome ou apague.',recovery:'Não foi possível recuperar o arquivo parcial. Apague-o antes de iniciar outro.',unavailable:'A importação não está disponível no momento.',
 attempts:'Tentativas demais. Aguarde um minuto.',version:'Versão de transferência incompatível. Recarregue a página.',idle:'Pronto para importar.'
}:es?{storage_full:'Almacenamiento Xbox lleno: {0} GB libres, se necesitan {1} GB. Libera espacio y reintenta.',update_route:'Actualización detectada. Abre Biblioteca > tu juego > Actualizaciones y sube allí el NSP.',dlc_route:'DLC detectado. Abre Biblioteca > tu juego > DLC y sube allí el NSP.',stalled:'Sin progreso durante 60 s. Archivo: ',keys_older_version:'Tus prod.keys son anteriores al firmware {0}; extrae claves de una consola con {0} o posterior.',firmware_keys_old:'Tus prod.keys son anteriores al firmware seleccionado; extrae claves de una consola con ese firmware o posterior. No se pudo leer la versión.',title_keys:'title.keys contiene claves por título para tus copias cifradas con tickets; no sustituye a prod.keys.',
 title:'Conecta con tu consola',intro:'Introduce el código temporal que muestra Jadyx. Mantén la app abierta en la biblioteca.',label:'Código de conexión',connect:'Conectar',
 uploadtitle:'Importar juegos NSP',uploadintro:'Solo juegos base. Los archivos se copian al almacenamiento interno de la consola.',fileslabel:'Seleccionar archivos NSP',
 namelabel:'Nombre de destino (un solo archivo)',send:'Subir / reanudar',rename:'Renombrar archivo pendiente',pause:'Pausar',discard:'Eliminar archivo parcial',
 kicker:'Portal de red local','status-off':'Sin conectar','status-on':'Conectado','code-hint':'8 dígitos · cambia cada vez que abres el portal','files-drop':'o arrástralos aquí',files:'archivos',
 privacy:'Red local · Conexión HTTP sin cifrado.',ok:'Conexión establecida.',bad:'Código incorrecto o caducado. Genera otro en Jadyx.',
 lost:'La conexión se ha cerrado. Vuelve a conectarte desde Jadyx; el progreso confirmado se conserva.',free:'Espacio libre',confirmed:'Confirmado',
 speed:'Velocidad',eta:'Tiempo restante',queue:'En cola',choose:'Selecciona uno o varios archivos NSP.',same:'Hay un archivo pendiente distinto. Selecciona el mismo archivo para reanudarlo o elimina la transferencia parcial.',
 receiving:'Recibiendo',paused:'En pausa. Selecciona el mismo archivo para continuar.',ready:'Esperando a validar',validating:'Validando NSP',publishing:'Añadiendo a la biblioteca',complete:'Instalado en la biblioteca',
 busy:'Espera a que termine la operación en Jadyx.',exists:'Ese nombre ya existe. Cambia el nombre de destino y vuelve a intentarlo.',space:'No hay espacio suficiente.',
 file:'Nombre o archivo no válido. Selecciona un NSP.',base:'Solo se admiten juegos base; no actualizaciones, DLC ni paquetes mezclados.',
 nsp:'La estructura del NSP no es válida.',incomplete:'El NSP está incompleto.',keys:'No se pueden leer sus metadatos. Importa prod.keys de tu consola con el firmware requerido por el juego o posterior y comprueba el NSP.',
 disk:'No se pudo escribir o publicar el archivo.',interrupted:'Transferencia interrumpida. Puedes reanudar desde el progreso confirmado.',
 partial:'Hay una transferencia pendiente. Reanúdala o elimínala.',recovery:'No se pudo recuperar el parcial. Elimínalo antes de iniciar otro.',unavailable:'La importación no está disponible en este momento.',
 attempts:'Demasiados intentos. Espera un minuto.',version:'Versión de transferencia incompatible. Recarga la página.',idle:'Listo para importar.'
}:{storage_full:'Xbox storage full: {0} GB free, need {1} GB. Free space and retry.',update_route:'Update detected. Open Library > your game > Updates and upload the NSP there.',dlc_route:'DLC detected. Open Library > your game > DLC and upload the NSP there.',stalled:'No progress for 60 s. File: ',keys_older_version:'Your prod.keys are older than firmware {0}; dump keys from a console on {0} or newer.',firmware_keys_old:'Your prod.keys are older than the selected firmware; dump keys from a console on that firmware or newer. Its version could not be read.',title_keys:'title.keys contains per-title decryption keys for your own ticket-encrypted dumps; it does not replace prod.keys.',ok:'Connected.',bad:'Incorrect or expired code. Generate another in Jadyx.',lost:'Connection closed. Reconnect in Jadyx; confirmed progress is retained.',
 free:'Free space',confirmed:'Confirmed',speed:'Speed',eta:'Time left',queue:'Queued',choose:'Choose one or more NSP files.',files:'files',
 same:'A different file is pending. Select the same file to resume, or discard the partial upload.',receiving:'Receiving',paused:'Paused. Select the same file to continue.',
 ready:'Waiting for validation',validating:'Validating NSP',publishing:'Adding to library',complete:'Installed in your library',busy:'Wait for the operation in Jadyx to finish.',
 exists:'This destination already exists. Change its name and try again.',space:'Not enough free space.',file:'Invalid filename or file. Choose an NSP.',
 base:'Base games only; updates, DLC and mixed packages are not supported.',nsp:'Invalid NSP structure.',incomplete:'The NSP is incomplete.',
 keys:'Cannot read its metadata. Import prod.keys from your console on the firmware required by this game or newer, and check the NSP.',disk:'Could not write or publish this file.',
 interrupted:'Transfer interrupted. You can resume from confirmed progress.',partial:'An upload is pending. Resume or discard it.',
 recovery:'Cannot recover the partial file. Discard it before starting another.',unavailable:'Import is currently unavailable.',
 attempts:'Too many attempts. Wait one minute.',version:'Incompatible upload version. Reload this page.',idle:'Ready to import.'};
if(es||pt){document.documentElement.lang=pt?'pt-BR':'es';for(const id of ['title','intro','label','connect','uploadtitle','uploadintro','fileslabel','namelabel','send','rename','pause','discard','privacy',
 'kicker','status-off','status-on','code-hint','files-drop'])$(id).textContent=words[id];}
const labels=pt?{'tab-content':'Conteúdo','tab-system':'Sistema','content-kicker':'Instalar','cat-games-title':'Jogos','cat-games-sub':'NSP · jogo base',
 'cat-library-title':'Biblioteca','cat-library-sub':'Atualizações · DLC · apagar','system-kicker':'Arquivos do sistema','cat-keys-title':'Chaves'}
:es?{'tab-content':'Contenido','tab-system':'Sistema','content-kicker':'Instalar','cat-games-title':'Juegos','cat-games-sub':'NSP · juego base',
 'cat-library-title':'Biblioteca','cat-library-sub':'Actualizaciones · DLC · eliminar','system-kicker':'Archivos del sistema','cat-keys-title':'Claves'}:{};
for(const [id,text]of Object.entries(labels))$(id).textContent=text;
if(es||pt)document.title='Portal Jadyx';
// The header's tabs and each page's categories, as in the console: one of each group is shown.
const tabs=[['tab-content','view-content'],['tab-system','view-system']],categories=[['cat-keys','keys-pane'],['cat-firmware','firmware-pane']];
function choose(group,chosen){for(const [button,view]of group){$(button).ariaSelected=String(button===chosen);$(view).hidden=button!==chosen;}}
for(const group of [tabs,categories])for(const [button]of group)$(button).addEventListener('click',()=>choose(group,button));
const selectedChunk=Number(new URLSearchParams(location.search).get('chunk'));
const chunk=(selectedChunk===4||selectedChunk===16||selectedChunk===64?selectedChunk:16)*1048576;
let session='',timer,state={},running=false,stopped=false,currentXHR=null,currentId='',rates=[],pane='games',contentRunning=false;
let systemRunning=false,systemReviewId='';
const delay=ms=>new Promise(r=>setTimeout(r,ms)),mib=n=>(n/1048576).toFixed(1),reason=e=>friendlyReason(e)||words[e.message]||words.interrupted;
function friendlyReason(error,record=state.transfer||{}){
 if(error.message!=='space')return '';
 const free=error.free_bytes??record.free_bytes,need=error.required_bytes??record.required_bytes;
 if(!Number.isFinite(free)||!Number.isFinite(need))return words.space;
 return words.storage_full.replace('{0}',(free/1e9).toFixed(2)).replace('{1}',(need/1e9).toFixed(2));
}
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
 if(!response.ok){let detail={error:'interrupted'};try{detail=await response.json();}catch{}throw Object.assign(Error(detail.error||'interrupted'),detail);}
 return response;
}
function render(){
 renderLogs();
 renderUpdate();
 const s=state.transfer||{};
 $('upload').hidden=!session||!state.uploads||pane!=='games';
 $('space').textContent=words.free+': '+((s.free_bytes||0)/1073741824).toFixed(2)+' GiB';
 $('job').textContent=(s.name?s.name+'\n':'')+(words[s.phase]||words.idle)+(s.length?' · '+size(s.received||0)+' / '+size(s.length):'');
 $('progress').value=s.length?Math.min(1,(s.received||0)/s.length):0;
 $('percent').textContent=percent(s.received||0,s.length);
 $('confirmed').textContent=words.confirmed+': '+mib(s.offset||0)+' / '+mib(s.length||0)+' MiB';
 $('files').disabled=running;$('name').disabled=running||s.phase==='validating'||s.phase==='publishing';
 $('send').disabled=running||contentRunning||systemRunning||state.system?.busy||!session||!state.uploads||s.phase==='ready'||s.phase==='validating'||s.phase==='publishing';
 $('pause').disabled=systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('discard').disabled=systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 $('rename').disabled=running||systemRunning||state.system?.busy||!s.id||s.phase==='complete'||s.phase==='validating'||s.phase==='publishing';
 if(s.error)$('message').textContent=reason(Error(s.error));
 renderSystem();
 renderLibrary();
}
async function refresh(){const response=await api('/api/v1/status');state=await response.json();render();followLibrary();return state;}
$('pair').addEventListener('submit',async event=>{
 event.preventDefault();$('connect').disabled=true;$('message').textContent='';
 try{
 const response=await fetch('/api/v1/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:$('code').value})});
 if(!response.ok){$('connection').textContent=response.status===429?words.attempts:words.bad;return;}
 session=(await response.json()).session;$('code').value='';$('pair').hidden=true;$('connection').textContent=words.ok;
 await refresh();clearInterval(timer);let polling=false;
 timer=setInterval(async()=>{if(polling)return;polling=true;try{await refresh();failures=0;$('connection').textContent=words.ok;}catch(error){
 // After the package went to Device Portal, the console going away is the install closing Jadyx.
 if(updateStarted&&(error.message==='lost'||++failures>=3)){clearInterval(timer);session='';updateGone=true;renderUpdate();}
 else if(error.message==='lost'){clearInterval(timer);stopped=true;if(currentXHR)currentXHR.abort();session='';$('pair').hidden=false;$('upload').hidden=true;$('connection').textContent=words.lost;}
 else $('connection').textContent=words.interrupted;
 }finally{polling=false;}},1000);
 }catch{$('connection').textContent=words.lost;}finally{$('connect').disabled=false;}
});
// Only digits; the eighth one connects without reaching for the button.
$('code').addEventListener('input',()=>{const code=$('code').value.replace(/\D/g,'').slice(0,8);$('code').value=code;
 if(code.length===8&&!$('connect').disabled)$('pair').requestSubmit?.();});
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
 xhr.onload=()=>{currentXHR=null;if(xhr.status===204)resolve(Number(xhr.getResponseHeader('Upload-Offset')));else{let detail={error:'interrupted'};try{detail=JSON.parse(xhr.responseText);}catch{}reject(Object.assign(Error(xhr.status===401?'lost':detail.error),detail));}};
 xhr.onerror=()=>{currentXHR=null;reject(Error('interrupted'));};xhr.onabort=()=>{currentXHR=null;reject(Error('paused'));};xhr.send(blob);
});}
// Games and updates/DLC share this resumable client. A target names the status record, its
// route, the extra metadata (an update/DLC names its kind and game) and where speed is shown.
const gameTarget={key:'transfer',base:'/api/v1/uploads',tag:'',metadata:{},rate:'rate',eta:'eta'};
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
$('files').addEventListener('change',()=>{if($('files').files.length)$('name').value=$('files').files[0].name;picked('files-picked','files');});
// The file inputs cover their drop zones, so a drop lands on the input itself; this only lights the zone.
for(const id of ['files','content-files','keys-files','firmware-files']){
 for(const event of ['dragenter','dragover'])$(id).addEventListener(event,e=>e.target.parentElement.classList.add('over'));
 for(const event of ['dragleave','drop'])$(id).addEventListener(event,e=>e.target.parentElement.classList.remove('over'));
}
$('send').addEventListener('click',async()=>{
 if(running||systemRunning||state.system?.busy)return;const files=Array.from($('files').files);if(!files.length){$('message').textContent=words.choose;return;}
 running=true;stopped=false;rates=[];$('message').textContent='';render();
 try{for(let i=0;i<files.length;i++){if(stopped)break;$('queue').textContent=words.queue+': '+(files.length-i-1);await sendFile(files[i],files.length===1?$('name').value:files[i].name);}}
 catch(error){$('message').textContent=reason(error);}finally{running=false;currentXHR=null;try{await refresh();}catch{}render();}
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
 title:'Sua biblioteca',intro:'Abra um jogo para gerenciar suas atualizações e DLC ou para apagá-lo.',empty:'Ainda não há jogos.',back:'← Biblioteca',
 update:'Atualizações',dlc:'DLC',none_update:'Nenhuma atualização instalada.',none_dlc:'Nenhum DLC instalado.',pick_update:'Selecionar NSP de atualização',pick_dlc:'Selecionar NSP de DLC',
 send:'Enviar e instalar',remove_game:'Apagar jogo',remove:'Apagar',confirm:'Clique de novo para confirmar',external_item:'Pasta externa',
 warning:'Apagar o jogo remove também seus saves, atualizações e DLC deste console. Mods e cheats são mantidos. Não pode ser desfeito.',
 external:'Este jogo está em uma pasta externa; o Jadyx não apaga arquivos de lá.',removing:'Apagando…',removed_game:'Jogo apagado.',removed:'Conteúdo apagado.',
 ready:'Aguardando instalação',validating:'Instalando',complete:'Instalado',idle:'Escolha arquivos para instalar.',failed:'Não foi possível concluir a operação.',
 game:'O jogo não está mais na biblioteca ou não pôde ser apagado.',content:'Não foi possível apagar esse conteúdo.',saves:'O jogo foi apagado, mas alguns saves não puderam ser removidos.',
 wrong_game:'Esse pacote pertence a outro jogo.',kind:'O pacote não é do tipo escolhido (atualização ou DLC).',conflict:'Já existe uma versão conflitante desse conteúdo.',
 hash:'O pacote está danificado.',requirements:'O pacote exige uma versão mais nova do jogo ou do firmware.',unsupported:'Esse pacote não é compatível.',invalid:'O pacote não é válido.',metadata:'Recarregue a página e tente de novo.'
}:es?{
 title:'Tu biblioteca',intro:'Abre un juego para gestionar sus actualizaciones y DLC, o para eliminarlo.',empty:'Todavía no hay juegos.',back:'← Biblioteca',
 update:'Actualizaciones',dlc:'DLC',none_update:'No hay actualizaciones instaladas.',none_dlc:'No hay DLC instalados.',pick_update:'Seleccionar NSP de actualización',pick_dlc:'Seleccionar NSP de DLC',
 send:'Subir e instalar',remove_game:'Eliminar juego',remove:'Eliminar',confirm:'Pulsa otra vez para confirmar',external_item:'Carpeta externa',
 warning:'Eliminar el juego borra también sus partidas, actualizaciones y DLC de esta consola. Los mods y trucos se conservan. No se puede deshacer.',
 external:'Este juego está en una carpeta externa; Jadyx no borra archivos de ahí.',removing:'Eliminando…',removed_game:'Juego eliminado.',removed:'Contenido eliminado.',
 ready:'Esperando a instalar',validating:'Instalando',complete:'Instalado',idle:'Selecciona archivos para instalar.',failed:'No se pudo completar la operación.',
 game:'El juego ya no está en la biblioteca o no se pudo eliminar.',content:'No se pudo eliminar ese contenido.',saves:'El juego se eliminó, pero algunas partidas no se pudieron borrar.',
 wrong_game:'Ese paquete es de otro juego.',kind:'El paquete no es del tipo elegido (actualización o DLC).',conflict:'Ya hay una versión de ese contenido que choca con esta.',
 hash:'El paquete está dañado.',requirements:'El paquete necesita una versión más nueva del juego o del firmware.',unsupported:'Ese paquete no es compatible.',invalid:'El paquete no es válido.',metadata:'Recarga la página y vuelve a intentarlo.'
}:{
 title:'Your library',intro:'Open a game to manage its updates and DLC, or to delete it.',empty:'No games yet.',back:'← Library',
 update:'Updates',dlc:'DLC',none_update:'No updates installed.',none_dlc:'No DLC installed.',pick_update:'Choose update NSP files',pick_dlc:'Choose DLC NSP files',
 send:'Upload and install',remove_game:'Delete game',remove:'Delete',confirm:'Click again to confirm',external_item:'External folder',
 warning:'Deleting the game also deletes its saves, updates and DLC from this console. Mods and cheats are kept. This cannot be undone.',
 external:'This game is in an external folder; Jadyx does not delete files there.',removing:'Deleting…',removed_game:'Game deleted.',removed:'Content deleted.',
 ready:'Waiting to install',validating:'Installing',complete:'Installed',idle:'Choose files to install.',failed:'The operation could not be completed.',
 game:'The game is no longer in the library or could not be deleted.',content:'That content could not be deleted.',saves:'The game was deleted, but some saves could not be removed.',
 wrong_game:'That package belongs to another game.',kind:'The package is not the chosen kind (update or DLC).',conflict:'A conflicting version of this content is already installed.',
 hash:'The package is damaged.',requirements:'The package needs a newer game or firmware version.',unsupported:'That package is not supported.',invalid:'The package is not valid.',metadata:'Reload this page and try again.'};
for(const [id,key]of [['library-title','title'],['library-intro','intro'],['library-empty','empty'],['game-back','back'],['game-warning','warning'],
 ['kind-update','update'],['kind-dlc','dlc'],['content-send','send'],['content-pick','pick_update']])$(id).textContent=libraryWords[key];
for(const [id,key]of [['content-drop','files-drop'],['content-pause','pause'],['content-discard','discard']])if(words[key])$(id).textContent=words[key];
const libraryReason=error=>friendlyReason(error,state.content||{})||libraryWords[error.message]||words[error.message]||libraryWords.failed;
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
 if(fresh){gameId=id;contents=null;armed='';chooseKind('update');$('library-note').textContent='';$('content-files').value='';$('content-picked').textContent='';}
 $('game-name').textContent=game.name;$('game-meta').textContent=[game.developer,game.version].filter(Boolean).join(' · ');
 showCover($('game-art'),id);loadContents();render();
}
function chooseKind(next){
 kind=next;armed='';
 for(const k of ['update','dlc'])$('kind-'+k).ariaSelected=String(k===kind);
 $('content-pick').textContent=libraryWords['pick_'+kind];renderItems();
}
// Nothing deletes or installs while another storage job runs; the server refuses it as well.
function libraryBusy(){
 const r=state.removal||{};
 return running||contentRunning||systemRunning||!!state.system?.busy||!!state.transfer?.busy||!!state.content?.busy||r.phase==='pending'||r.phase==='running';
}
// The first press arms a deletion and the second, within six seconds, sends it.
function arm(key){
 if(armed===key){armed='';clearTimeout(armedTimer);return true;}
 armed=key;clearTimeout(armedTimer);armedTimer=setTimeout(()=>{armed='';renderItems();render();},6000);renderItems();render();return false;
}
async function remove(path,key){
 if(!arm(key))return;
 try{const data=await(await api('/api/v1/library/'+path,{method:'DELETE'})).json();removalSerial=data.removal.serial;$('message').textContent='';await refresh();}
 catch(error){$('message').textContent=libraryReason(error);}
 renderItems();
}
function renderItems(){
 if(!gameId)return;
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
 $('cat-library').hidden=!on;if(!on&&pane==='library')pane='games';
 $('cat-games').ariaSelected=String(pane==='games');$('cat-library').ariaSelected=String(pane==='library');
 $('library').hidden=!on||pane!=='library';if($('library').hidden)return;
 const game=gameId&&games.find(g=>g.id===gameId);
 $('library-list').hidden=!!game;$('game').hidden=!game;
 const r=state.removal||{};
 if(r.phase==='pending'||r.phase==='running')$('library-note').textContent=libraryWords.removing;
 if(!game)return;
 const busy=libraryBusy(),s=state.content||{},install=state.install||{},installing=s.phase==='validating';
 if(busy!==lastBusy){lastBusy=busy;renderItems();}
 $('game-remove').hidden=!game.removable;$('game-remove').disabled=busy;
 $('game-remove').textContent=armed==='game'?libraryWords.confirm:libraryWords.remove_game;
 $('game-warning').textContent=game.removable?libraryWords.warning:libraryWords.external;
 const active=s.id&&!['complete','validating','publishing'].includes(s.phase);
 $('content-files').disabled=contentRunning;
 $('content-send').disabled=busy||s.available===false||['ready','validating','publishing'].includes(s.phase);
 $('content-pause').disabled=!active||systemRunning;$('content-discard').disabled=!active||systemRunning;
 $('content-job').textContent=(s.name?s.name+'\n':'')+(installing&&install.total?libraryWords.validating+' · '+install.done+' / '+install.total+' MiB':
  libraryWords[s.phase]||words[s.phase]||libraryWords.idle);
 $('content-progress').value=installing?(install.total?Math.min(1,install.done/install.total):0):s.length?Math.min(1,(s.received||0)/s.length):0;
 $('content-percent').textContent=installing?percent(install.done||0,install.total):percent(s.received||0,s.length);
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
$('cat-games').addEventListener('click',()=>choosePane('games'));$('cat-library').addEventListener('click',()=>choosePane('library'));
$('game-back').addEventListener('click',()=>{gameId='';armed='';render();});
$('kind-update').addEventListener('click',()=>chooseKind('update'));$('kind-dlc').addEventListener('click',()=>chooseKind('dlc'));
$('game-remove').addEventListener('click',()=>remove(gameId,'game'));
$('content-files').addEventListener('change',()=>picked('content-picked','content-files'));
$('content-send').addEventListener('click',async()=>{
 if(libraryBusy()||!gameId)return;const files=Array.from($('content-files').files);if(!files.length){$('message').textContent=words.choose;return;}
 contentRunning=true;stopped=false;rates=[];$('message').textContent='';$('library-note').textContent='';const target=contentTarget();render();
 try{for(const file of files){if(stopped)break;await sendFile(file,file.name,target);}await loadContents();}
 catch(error){$('message').textContent=libraryReason(error);}finally{contentRunning=false;currentXHR=null;try{await refresh();}catch{}render();}
});
$('content-pause').addEventListener('click',()=>control('pause',contentTarget()));$('content-discard').addEventListener('click',()=>control('discard',contentTarget()));
const systemWords=pt?{storage_full:'Xbox sem espaço: {0} GB livres, precisa de {1} GB. Libere espaço e tente novamente.',update_route:'Atualização detectada. Abra Biblioteca > seu jogo > Atualizações e envie o NSP ali.',dlc_route:'DLC detectado. Abra Biblioteca > seu jogo > DLC e envie o NSP ali.',stalled:'Sem progresso há 60 s. Arquivo: ',keys_older_version:'Suas prod.keys são anteriores ao firmware {0}; extraia chaves de um console com {0} ou mais recente.',firmware_keys_old:'Suas prod.keys são anteriores ao firmware selecionado; extraia chaves de um console com esse firmware ou mais recente. A versão não pôde ser lida.',title_keys:'title.keys contém chaves por título para suas cópias criptografadas com tickets; não substitui prod.keys.',
 keysTitle:'Importar chaves',firmware:'Importar firmware',keysHelp:'ZIP ou prod.keys / title.keys. Máximo de 4 MiB por arquivo de chaves.',
 firmwareHelp:'ZIP ou arquivos NCA extraídos. A versão será verificada antes de instalar.',folder:'Ou escolha uma pasta extraída',send:'Enviar e revisar',confirm:'Confirmar instalação',cancel:'Cancelar e apagar importação',accept:'Entendo e aceito os avisos.',
 systemTitle:'Chaves e firmware',systemIntro:'Tudo é verificado antes de instalar. Os arquivos atuais ficam até você confirmar.',pick:'Selecionar arquivos',drop:'ou arraste-os para cá',
 installed:'Instalado',selected:'Selecionado',generation:'Geração das chaves (índice)',required:'Geração exigida pelo firmware',ready:'Arquivos validados. Revise as versões e confirme.',review:'Revise as versões e confirme.',
 firmware_downgrade:'O firmware selecionado é anterior ao instalado.',keys_downgrade:'As novas chaves perdem gerações disponíveis nas chaves instaladas.',keys_firmware_old:'As chaves podem não abrir o firmware instalado. Continuar pode deixá-lo inutilizável.',keys_unknown:'Não foi possível verificar a geração das chaves.',installed_unknown:'Não foi possível verificar o firmware instalado.',
 receiving:'Recebendo arquivos',extracting:'Extraindo ZIP',validating:'Validando arquivos',prepare_pending:'Aguardando validação',commit_pending:'Aguardando instalação',publishing:'Instalando',complete:'Instalação concluída',failed:'A importação falhou; os dados anteriores foram preservados.',idle:'Escolha arquivos para importar.',
 archive:'O ZIP está danificado ou não é compatível.',archive_unsafe:'O ZIP contém caminhos perigosos, links ou arquivos criptografados.',archive_limit:'O arquivo ultrapassa os limites de tamanho ou quantidade.',file_limit:'Quantidade de arquivos inválida.',file:'Use somente ZIP ou os arquivos correspondentes, sem misturá-los.',keys:'Arquivo de chaves inválido.',keys_header:'Falta uma header_key válida em prod.keys.',keys_limit:'Cada arquivo de chaves deve ter no máximo 4 MiB.',firmware_keys:'As chaves instaladas não conseguem ler o firmware. Importe chaves compatíveis primeiro.',firmware_incomplete:'Firmware incompleto: falta SystemVersion ou MiiEdit.',firmware_game:'Este pacote contém arquivos de jogo.',firmware_duplicate:'O pacote contém nomes ou versões duplicados.',firmware_version:'Não foi possível verificar uma versão de firmware NX válida.',confirm_warnings:'Aceite os avisos antes de instalar.',recovery:'A importação interrompida não pôde ser recuperada. Apague-a e selecione os arquivos novamente.'
}:es?{storage_full:'Almacenamiento Xbox lleno: {0} GB libres, se necesitan {1} GB. Libera espacio y reintenta.',update_route:'Actualización detectada. Abre Biblioteca > tu juego > Actualizaciones y sube allí el NSP.',dlc_route:'DLC detectado. Abre Biblioteca > tu juego > DLC y sube allí el NSP.',stalled:'Sin progreso durante 60 s. Archivo: ',keys_older_version:'Tus prod.keys son anteriores al firmware {0}; extrae claves de una consola con {0} o posterior.',firmware_keys_old:'Tus prod.keys son anteriores al firmware seleccionado; extrae claves de una consola con ese firmware o posterior. No se pudo leer la versión.',title_keys:'title.keys contiene claves por título para tus copias cifradas con tickets; no sustituye a prod.keys.',
 keysTitle:'Importar claves',firmware:'Importar firmware',keysHelp:'ZIP o prod.keys / title.keys. Máximo de 4 MiB por archivo de claves.',
 firmwareHelp:'ZIP o archivos NCA extraídos. La versión se comprobará antes de instalar.',folder:'O elige una carpeta extraída',send:'Subir y revisar',confirm:'Confirmar instalación',cancel:'Cancelar y eliminar importación',accept:'Entiendo y acepto las advertencias.',
 systemTitle:'Claves y firmware',systemIntro:'Todo se comprueba antes de instalar. Tus archivos actuales se conservan hasta que confirmes.',pick:'Seleccionar archivos',drop:'o arrástralos aquí',
 installed:'Instalado',selected:'Seleccionado',generation:'Generación de claves (índice)',required:'Generación requerida por el firmware',ready:'Archivos validados. Revisa las versiones y confirma.',review:'Revisa las versiones y confirma.',
 firmware_downgrade:'El firmware seleccionado es anterior al instalado.',keys_downgrade:'Las nuevas claves pierden generaciones disponibles en las instaladas.',keys_firmware_old:'Las claves podrían no abrir el firmware instalado. Continuar puede dejarlo inutilizable.',keys_unknown:'No se pudo verificar la generación de las claves.',installed_unknown:'No se pudo verificar el firmware instalado.',
 receiving:'Recibiendo archivos',extracting:'Extrayendo ZIP',validating:'Validando archivos',prepare_pending:'Esperando validación',commit_pending:'Esperando instalación',publishing:'Instalando',complete:'Instalación completada',failed:'La importación falló; se conservaron los datos anteriores.',idle:'Selecciona archivos para importar.',
 archive:'El ZIP está dañado o no es compatible.',archive_unsafe:'El ZIP contiene rutas peligrosas, enlaces o archivos cifrados.',archive_limit:'El archivo supera los límites de tamaño o cantidad.',file_limit:'Cantidad de archivos no válida.',file:'Usa solo ZIP o los archivos correspondientes, sin mezclarlos.',keys:'Archivo de claves no válido.',keys_header:'Falta una header_key válida en prod.keys.',keys_limit:'Cada archivo de claves debe ocupar como máximo 4 MiB.',firmware_keys:'Las claves instaladas no pueden leer el firmware. Importa antes claves compatibles.',firmware_incomplete:'Firmware incompleto: falta SystemVersion o MiiEdit.',firmware_game:'El paquete contiene archivos de juego.',firmware_duplicate:'El paquete contiene nombres o versiones duplicados.',firmware_version:'No se pudo verificar una versión de firmware NX válida.',confirm_warnings:'Acepta las advertencias antes de instalar.',recovery:'No se pudo recuperar la importación interrumpida. Elimínala y vuelve a seleccionar los archivos.'
}:{storage_full:'Xbox storage full: {0} GB free, need {1} GB. Free space and retry.',update_route:'Update detected. Open Library > your game > Updates and upload the NSP there.',dlc_route:'DLC detected. Open Library > your game > DLC and upload the NSP there.',stalled:'No progress for 60 s. File: ',keys_older_version:'Your prod.keys are older than firmware {0}; dump keys from a console on {0} or newer.',firmware_keys_old:'Your prod.keys are older than the selected firmware; dump keys from a console on that firmware or newer. Its version could not be read.',title_keys:'title.keys contains per-title decryption keys for your own ticket-encrypted dumps; it does not replace prod.keys.',
 keysTitle:'Import keys',firmware:'Import firmware',keysHelp:'ZIP or prod.keys / title.keys. Maximum 4 MiB per key file.',firmwareHelp:'ZIP or extracted NCA files. The version is checked before installation.',folder:'Or choose an extracted folder',send:'Upload and review',confirm:'Confirm installation',cancel:'Cancel and discard import',accept:'I understand and accept these warnings.',
 systemTitle:'Keys and firmware',systemIntro:'Everything is checked before installing. Your current files stay until you confirm.',pick:'Choose files',drop:'or drop them here',
 installed:'Installed',selected:'Selected',generation:'Key generation (index)',required:'Firmware required generation',ready:'Files validated. Review versions and confirm.',review:'Review versions and confirm.',firmware_downgrade:'The selected firmware is older than the installed firmware.',keys_downgrade:'The new keys lose generations present in the installed keys.',keys_firmware_old:'These keys may not open the installed firmware. Continuing may make it unusable.',keys_unknown:'The key generation could not be verified.',installed_unknown:'The installed firmware could not be verified.',
 receiving:'Receiving files',extracting:'Extracting ZIP',validating:'Validating files',prepare_pending:'Waiting for validation',commit_pending:'Waiting for installation',publishing:'Installing',complete:'Installation complete',failed:'Import failed; previous data was preserved.',idle:'Choose files to import.',archive:'The ZIP is damaged or unsupported.',archive_unsafe:'The ZIP contains unsafe paths, links or encrypted entries.',archive_limit:'The size or file count limit was exceeded.',file_limit:'Invalid file count.',file:'Use only a ZIP or the appropriate loose files, without mixing formats.',keys:'Invalid key file.',keys_header:'A valid header_key is required in prod.keys.',keys_limit:'Each key file must be at most 4 MiB.',firmware_keys:'Installed keys cannot read this firmware. Import compatible keys first.',firmware_incomplete:'Incomplete firmware: SystemVersion or MiiEdit is missing.',firmware_game:'The package contains game files.',firmware_duplicate:'The package contains duplicate names or versions.',firmware_version:'A valid NX firmware version could not be verified.',confirm_warnings:'Accept warnings before installing.',recovery:'The interrupted import could not be recovered. Discard it and select the files again.'};
for(const [id,text]of Object.entries({'keys-title':systemWords.keysTitle,'firmware-title':systemWords.firmware,'keys-help':systemWords.keysHelp+' '+systemWords.title_keys,'firmware-help':systemWords.firmwareHelp,
 'firmware-folder-label':systemWords.folder,'keys-send':systemWords.send,'firmware-send':systemWords.send,'system-confirm':systemWords.confirm,'system-cancel':systemWords.cancel,'system-warning-label':systemWords.accept,
 'system-title':systemWords.systemTitle,'system-intro':systemWords.systemIntro,'keys-pick':systemWords.pick,'firmware-pick':systemWords.pick,'keys-drop':systemWords.drop,'firmware-drop':systemWords.drop}))$(id).textContent=text;
function systemReason(error){return friendlyReason(error,state.system||{})||systemWords[error.message]||words[error.message]||systemWords.failed;}
function renderSystem(){
 const s=state.system||{},r=s.review||{},warnings=r.warnings||[];
 $('system-upload').hidden=!session||!state.system_supported;
 $('tab-system').hidden=!state.system_supported;
 if(!state.system_supported){if(!$('view-system').hidden)choose(tabs,'tab-content');return;}
 for(const kind of ['keys','firmware'])$(kind+'-send').disabled=running||systemRunning||state.transfer?.busy||!s.available||(s.busy&&s.kind!==kind)||!['idle','receiving','complete'].includes(s.phase);
 $('system-job').textContent=(systemWords[s.phase]||systemWords.idle)+(s.count?' · '+(s.completed||0)+' / '+s.count:'')+(s.current_file?' · '+s.current_file:'')+(s.no_progress_seconds>=60?'\n'+systemWords.stalled+(s.current_file||'—'):'')+(s.error?'\n'+systemReason(Error(s.error)):'');
 const f=s.file||{};$('system-job').textContent+='\n'+size(f.received||0)+' / '+size(f.length||0)+' · '+(s.processed||0)+' '+systemWords.validating;$('system-progress').value=f.length?Math.min(1,(f.received||0)/f.length):0;$('system-percent').textContent=percent(f.received||0,f.length);
 $('system-review').hidden=s.phase!=='review';
 if(s.phase!=='review')systemReviewId='';
 if(s.phase==='review'){
  if(systemReviewId!==s.id){systemReviewId=s.id;$('system-warning-accept').checked=false;}
  const gen=value=>value>=0?'0x'+value.toString(16).padStart(2,'0'):'—';
  $('system-versions').textContent=systemWords.installed+': '+(r.installed_firmware||'—')+'\n'+systemWords.selected+': '+(r.incoming_firmware||'—')+
    '\n'+systemWords.generation+': '+gen(r.installed_generation)+' → '+gen(r.incoming_generation)+'\n'+systemWords.required+': '+gen(r.required_generation);
  $('system-warnings').textContent=warnings.map(w=>w==='keys_firmware_old'&&r.incoming_generation>=0&&r.required_generation>r.incoming_generation?systemWords.keys_older_version.replaceAll('{0}',r.incoming_firmware||r.installed_firmware||'—'):systemWords[w]||systemWords.keys_unknown).join('\n');
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
 const s=state.logs||{};$('log-export').hidden=!session||!s.supported;
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
