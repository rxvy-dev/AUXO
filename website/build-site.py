#!/usr/bin/env python3
"""Builds the Auxo site (v3): index.html (home) and download/index.html.

Usage: python3 build-site.py [--preview]
  --preview  links pages as download.html / index.html so they work as flat files.
No JavaScript: every interactive part (accent picker, desktop switcher, tabs, FAQ)
is plain HTML + CSS, so WordPress can't break it.
"""
import json, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)

PREVIEW = "--preview" in sys.argv
OUT = "preview" if PREVIEW else "site"
HOME = "index.html" if PREVIEW else "/"
DL = "download.html" if PREVIEW else "/download/"
DOCS = "docs.html" if PREVIEW else "/docs/"

def iso(ident, name):
    return {"url": f"https://archive.org/download/{ident}/{name}",
            "torrent": f"https://archive.org/download/{ident}/{ident}_archive.torrent",
            "details": f"https://archive.org/details/{ident}", "name": name}

STD = dict(iso("auxo-linux-2026.09.27-x86_64", "auxo-linux-2026.09.27-x86_64.iso"), size="2.8 GB", date="2026-09-27")
VM = dict(iso("auxo-linux-2026.09.27-x86_64_202609", "auxo-linux-2026.09.27-x86_64.iso"), size="2.8 GB", date="2026-09-27")
DISCORD = "https://discord.gg/XbQ66dH5a7"
KDE_SHOT = "https://i.ibb.co/B5s78nCV/Screenshot-20260926-233008.png"

imgs = json.load(open("assets/screenshots.json"))  # live, desktop, accent, software, installing

ACCENTS = [("violet", "#a78bfa", "#22d3ee"), ("cyan", "#22d3ee", "#818cf8"),
           ("emerald", "#34d399", "#a3e635"), ("amber", "#fbbf24", "#fb7185"),
           ("rose", "#fb7185", "#c084fc"), ("blue", "#60a5fa", "#34d399"),
           ("mono", "#e5e7eb", "#9ca3af")]
acc_css = "\n".join(
    f"body:has(#acc-{n}:checked){{--a:{a};--a2:{b}}}"
    f"body:has(#acc-{n}:checked) .accname::after{{content:'{n}'}}" for n, a, b in ACCENTS)

MARK_PATHS = ('<g fill="none" stroke="url(#lg)" stroke-linecap="round" stroke-linejoin="round">'
              '<path d="M16 112 L64 22 L112 112" stroke-width="13"/>'
              '<path d="M40 96 L56 78 L68 90 L86 68" stroke-width="8"/>'
              '<circle cx="64" cy="9" r="6.5" fill="url(#lg)" stroke="none"/></g>')
# hero logo drawn in on load (pathLength lets CSS animate the stroke)
MARK_DRAW = ('<svg class="logo" viewBox="0 0 128 128" aria-hidden="true"><g fill="none" stroke="url(#lg)" stroke-linecap="round" stroke-linejoin="round">'
             '<path class="d1" pathLength="1" d="M16 112 L64 22 L112 112" stroke-width="13"/>'
             '<path class="d2" pathLength="1" d="M40 96 L56 78 L68 90 L86 68" stroke-width="8"/>'
             '<circle class="d3" cx="64" cy="9" r="6.5" fill="url(#lg)" stroke="none"/></g></svg>')
FAVICON = ("data:image/svg+xml," +
           "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 128 128'%3E%3Cdefs%3E%3ClinearGradient id='g' x1='0' y1='1' x2='1' y2='0'%3E%3Cstop offset='0' stop-color='%23fbbf24'/%3E%3Cstop offset='1' stop-color='%23fb7185'/%3E%3C/linearGradient%3E%3C/defs%3E%3Crect width='128' height='128' rx='28' fill='%230b0b10'/%3E%3Cg fill='none' stroke='url(%23g)' stroke-linecap='round' stroke-linejoin='round' transform='translate(14 14) scale(.78)'%3E%3Cpath d='M16 112 L64 22 L112 112' stroke-width='13'/%3E%3Cpath d='M40 96 L56 78 L68 90 L86 68' stroke-width='8'/%3E%3Ccircle cx='64' cy='9' r='6.5' fill='url(%23g)' stroke='none'/%3E%3C/g%3E%3C/svg%3E")

CSS = r"""
:root{color-scheme:dark;--bg:#0e1014;--bg2:#12151a;--card:#161a20;--card2:#1b2028;--line:#232932;--line2:#2f3641;--tx:#f3f4f6;--mut:#a3aab5;--dim:#6f7784;
--a:#fbbf24;--a2:#fb7185;--r:8px;--wrap:1320px;
--fh:'Inter',system-ui,sans-serif;--fb:'Inter',system-ui,sans-serif;--fm:'JetBrains Mono',ui-monospace,monospace;
--ease:cubic-bezier(.2,.7,.2,1)}
html{scroll-behavior:smooth;-webkit-text-size-adjust:100%;scroll-padding-top:84px;overflow-x:clip}
body{background:var(--bg);margin:0;overflow-x:clip}
/* full-bleed: escape the WordPress theme's content column */
.auxo{position:relative;box-sizing:border-box;width:100vw!important;max-width:100vw!important;margin:0 calc(50% - 50vw)!important;padding:0!important;background:var(--bg);color:var(--tx);font:16px/1.6 var(--fb);text-align:left;-webkit-font-smoothing:antialiased}
.auxo *,.auxo *::before,.auxo *::after{box-sizing:border-box;margin:0;padding:0}
.auxo a,.auxo a:hover,.auxo a:visited{color:inherit;text-decoration:none;box-shadow:none}
.auxo h1,.auxo h2,.auxo h3,.auxo h4{color:inherit;text-transform:none}
.auxo ul,.auxo ol{list-style:none}
.auxo img{max-width:100%;display:block;height:auto}
.auxo svg{max-width:none}
.auxo table{margin:0}
.auxo code{font:.86em var(--fm);background:#1a1a23;border:1px solid var(--line);border-radius:5px;padding:.1em .4em}
.wrap{max-width:var(--wrap);margin:0 auto;padding:0 24px}
.vh{position:absolute!important;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}
.mk,.logo{display:block}
.skip{position:absolute;left:-999px;top:8px;background:var(--a);color:#000;padding:8px 14px;border-radius:8px;z-index:99}
.skip:focus{left:12px}
:focus-visible{outline:2px solid var(--a);outline-offset:3px;border-radius:6px}
.grad{background:linear-gradient(100deg,var(--a),var(--a2));-webkit-background-clip:text;background-clip:text;color:transparent}

/* nav */
.nav{position:sticky;top:0;z-index:40;background:#050608;backdrop-filter:saturate(1.5) blur(16px);-webkit-backdrop-filter:saturate(1.5) blur(16px);border-bottom:1px solid var(--line)}
.nav .wrap{display:flex;align-items:center;gap:40px;height:72px}
.brand{display:flex;align-items:center;gap:10px;font:700 20px var(--fh);letter-spacing:-.02em}
.brand .mk{width:30px;height:30px}
.brand small{font:600 11px var(--fm);color:var(--dim);border:1px solid var(--line2);border-radius:5px;padding:1px 6px;margin-left:2px}
.links{display:flex;gap:30px;font-size:15px;font-weight:500;color:var(--mut);margin-left:auto}
.links a{position:relative}
.links a:hover,.links a[aria-current]{color:var(--tx)}
.links a[aria-current]::after{content:"";position:absolute;left:0;right:0;bottom:-26px;height:2px;background:var(--a)}
.nav .right{display:flex;gap:10px;align-items:center}
@media(max-width:880px){.nav .right{margin-left:auto}}
.icon-link{display:grid;place-items:center;width:38px;height:38px;border-radius:6px;color:var(--mut);border:1px solid var(--line)}
.icon-link:hover{color:var(--tx);border-color:var(--line2)}
.icon-link svg{width:18px;height:18px}
@media(max-width:880px){.links{display:none}}

/* buttons */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;height:44px;padding:0 20px;border-radius:6px;font-weight:600;font-size:15px;border:1px solid var(--line2);background:var(--card2);color:var(--tx);transition:border-color .2s,background .2s,transform .2s var(--ease),box-shadow .2s;white-space:nowrap}
.btn:hover{border-color:#44445a;background:#1c1c26}
.btn svg{width:17px;height:17px;flex:none}
.btn.primary{background:var(--a);border-color:var(--a);color:#0a0a0d}
.btn.primary:hover{filter:brightness(1.07)}
.btn.sm{height:38px;padding:0 14px;font-size:14px}
.btn.lg{height:52px;padding:0 26px;font-size:16px}
.btn.ghost{background:transparent}
.btn .k{font:500 12px var(--fm);opacity:.7}

/* type */
.eyebrow .n{color:var(--dim);margin-right:2px}
.eyebrow{display:inline-flex;align-items:center;gap:8px;font:600 13px var(--fb);letter-spacing:.08em;text-transform:uppercase;color:var(--a)}
.eyebrow::before{display:none}
.center .eyebrow::before,.phead .eyebrow::before{display:none}
h1,h2,h3{font-family:var(--fh);letter-spacing:-.022em;line-height:1.08;font-weight:700}
.auxo h2{font-size:clamp(28px,3.2vw,40px);margin:12px 0 14px}
.auxo section{padding:96px 0}
.sec-head{display:grid;grid-template-columns:1fr 1fr;gap:14px 64px;align-items:end;margin-bottom:44px;padding-bottom:28px;border-bottom:1px solid var(--line)}
.sec-head h2{margin-bottom:0}
.sec-head>p{max-width:540px}
@media(max-width:860px){.sec-head{grid-template-columns:1fr}}
.sec-head.center{margin-left:0;margin-right:0;text-align:left}
.sec-head p{color:var(--mut);font-size:17.5px}
.alt{background:var(--bg2);border-top:1px solid var(--line);border-bottom:1px solid var(--line)}

/* background grid */
.gridbg{position:absolute;inset:0;pointer-events:none;background-image:linear-gradient(var(--line) 1px,transparent 1px),linear-gradient(90deg,var(--line) 1px,transparent 1px);background-size:56px 56px;-webkit-mask-image:radial-gradient(ellipse 70% 55% at 50% 30%,#000 30%,transparent 75%);mask-image:radial-gradient(ellipse 70% 55% at 50% 30%,#000 30%,transparent 75%);opacity:.28}

/* hero */
.hero{position:relative;padding:96px 0 120px;overflow:hidden;background:linear-gradient(180deg,#0a0c0f 0%,var(--bg) 100%);border-bottom:1px solid var(--line)}
.hero .hridge{position:absolute;left:0;right:0;bottom:0;width:100%;height:45%;pointer-events:none}
.hero .hridge path:nth-child(1){fill:#13171d}.hero .hridge path:nth-child(2){fill:#101318}
.hgrid{position:relative;display:grid;grid-template-columns:1fr 1.2fr;gap:64px;align-items:center}
.hgrid>*{min-width:0}
@media(max-width:980px){.hgrid{grid-template-columns:1fr;gap:48px}}
.hero .badge,.khero .badge{display:inline-flex;align-items:center;gap:10px;padding:6px 12px;border:1px solid var(--line2);border-radius:5px;font-size:13.5px;font-weight:500;color:var(--mut);background:var(--card);margin-bottom:26px}
.khero .badge:hover,.hero .badge:hover{border-color:var(--a);color:var(--tx)}
.khero .badge b,.hero .badge b{width:7px;height:7px;border-radius:50%;background:var(--a);font-size:0}
.hero h1{font-size:clamp(38px,4.8vw,62px);margin:0 0 22px}
.hero .lede{font-size:clamp(17px,1.5vw,19.5px);color:var(--mut);max-width:560px}
.cta{display:flex;flex-wrap:wrap;gap:12px;margin:34px 0 26px}
.meta-line{display:flex;flex-wrap:wrap;gap:6px 22px;font-size:14px;color:var(--dim)}
.meta-line span{display:inline-flex;align-items:center;gap:7px}
.meta-line svg{width:15px;height:15px;stroke:var(--a);fill:none;stroke-width:2.4;stroke-linecap:round;stroke-linejoin:round}
.rise{opacity:0;transform:translateY(10px);animation:rise .6s var(--ease) forwards}
.rise.r1{animation-delay:.08s}.rise.r2{animation-delay:.16s}.rise.r3{animation-delay:.24s}.rise.r4{animation-delay:.32s}.rise.r5{animation-delay:.4s}
@keyframes rise{to{opacity:1;transform:none}}
.product{border:1px solid var(--line2);border-radius:8px;overflow:hidden;background:var(--card);box-shadow:0 40px 80px -30px rgba(0,0,0,.85)}
.product img{width:100%;aspect-ratio:1100/650;object-fit:cover;object-position:top;background:#15181e}
.product figcaption{padding:10px 14px;border-top:1px solid var(--line);font-size:13px;color:var(--dim);background:#12151a}
/* window bar / terminal */
.bar{display:flex;align-items:center;gap:7px;height:36px;padding:0 14px;border-bottom:1px solid var(--line);background:#0f0f15;position:relative}
.bar i{width:10px;height:10px;border-radius:50%;background:#2a2a36}
.bar.mute i{background:#2a2a36!important}
.bar b{font:500 12px var(--fm);color:var(--dim);position:absolute;left:50%;transform:translateX(-50%);white-space:nowrap}
.term{background:#0b0b10;border:1px solid var(--line2);border-radius:var(--r);overflow:hidden}
.term pre{font:13px/1.75 var(--fm);padding:18px 20px;color:#d4d4dc;overflow-x:auto}
.ta{color:var(--a)}.ta2{color:var(--a2)}.tm{color:var(--dim)}.tg{color:#4ade80}
.caret{display:inline-block;width:8px;height:15px;background:var(--a);vertical-align:-2px;animation:blink 1.1s steps(1) infinite}
@keyframes blink{50%{opacity:0}}

/* strip */
.strip{border-top:1px solid var(--line);border-bottom:1px solid var(--line);background:var(--bg2);position:relative;z-index:1}
.strip .wrap{display:grid;grid-template-columns:repeat(4,1fr)}
.strip div{padding:30px 24px;border-left:1px solid var(--line)}
.strip div:first-child{border-left:0}
.strip b{display:block;font:700 34px var(--fh);letter-spacing:-.022em;color:var(--tx)}
.strip span{color:var(--mut);font-size:14.5px}
@media(max-width:760px){.strip .wrap{grid-template-columns:1fr 1fr}.strip div:nth-child(3){border-left:0}.strip div:nth-child(n+3){border-top:1px solid var(--line)}}

/* desktop switcher */
.switcher{display:grid;grid-template-columns:280px 1fr;gap:28px;align-items:start}
.switcher>*{min-width:0}
@media(max-width:960px){.switcher{grid-template-columns:1fr}}
.dtabs{display:flex;flex-direction:column;gap:6px}
@media(max-width:960px){.dtabs{flex-direction:row;overflow-x:auto;padding-bottom:6px;scrollbar-width:thin}}
.dtabs label{display:grid;grid-template-columns:1fr auto;align-items:center;gap:2px 10px;padding:12px 14px;border:1px solid var(--line);border-radius:8px;cursor:pointer;background:var(--card);transition:border-color .2s,background .2s}
.dtabs label:hover{border-color:var(--line2)}
.dtabs b{font:600 15px var(--fh)}
.dtabs small{grid-column:1;color:var(--dim);font-size:12.5px;line-height:1.4}
.dtabs em{grid-row:1/3;grid-column:2;font:600 10px var(--fm);letter-spacing:.06em;text-transform:uppercase;color:var(--a);border:1px solid color-mix(in srgb,var(--a) 35%,transparent);padding:2px 6px;border-radius:5px;font-style:normal}
@media(max-width:960px){.dtabs label{min-width:170px}}
.dview{border:1px solid var(--line2);border-radius:8px;overflow:hidden;background:var(--card)}
.screen{position:relative;aspect-ratio:16/10;overflow:hidden;background:
 radial-gradient(90% 70% at 80% 10%,color-mix(in srgb,var(--a2) 22%,transparent),transparent 60%),
 radial-gradient(80% 70% at 10% 90%,color-mix(in srgb,var(--a) 20%,transparent),transparent 60%),#0c0c12}
.screen .ridge{position:absolute;left:0;right:0;bottom:0;width:100%;height:46%}
.screen .ridge path:nth-child(1){fill:color-mix(in srgb,var(--a) 16%,#0c0c12)}
.screen .ridge path:nth-child(2){fill:color-mix(in srgb,var(--a2) 12%,#09090d)}
.L{position:absolute;inset:0;display:none}
.L i{position:absolute;display:block;border-radius:6px;background:rgba(16,16,22,.9);border:1px solid rgba(255,255,255,.08);box-shadow:0 10px 30px -10px rgba(0,0,0,.7);overflow:hidden}
.L i::before{content:"";position:absolute;left:0;right:0;top:0;height:clamp(9px,8%,16px);background:radial-gradient(circle at 9px 50%,#3a3a4a 2.4px,transparent 3px),radial-gradient(circle at 18px 50%,#3a3a4a 2.4px,transparent 3px),radial-gradient(circle at 27px 50%,#3a3a4a 2.4px,transparent 3px),#1a1a23;border-bottom:1px solid rgba(255,255,255,.06)}
.L i::after{content:"";position:absolute;left:7%;right:12%;top:calc(clamp(9px,8%,16px) + 9%);height:40%;max-height:150px;background:linear-gradient(var(--a),var(--a)) 0 0/34% 6px no-repeat,repeating-linear-gradient(rgba(255,255,255,.11) 0 5px,transparent 5px 15px) 0 20px/100% calc(100% - 20px) no-repeat}
.L i.t::after{background:linear-gradient(var(--a),var(--a)) 0 0/8% 4px no-repeat,linear-gradient(var(--a2),var(--a2)) 0 26px/5% 4px no-repeat,repeating-linear-gradient(rgba(255,255,255,.22) 0 4px,transparent 4px 13px) 10% 0/70% 100% no-repeat,repeating-linear-gradient(rgba(255,255,255,.1) 0 4px,transparent 4px 13px) 0 0/100% 100% no-repeat;right:20%}
.L i.nt::before{display:none}
.L i.nt::after{top:9%}
.L i.t::after{height:55%;max-height:190px}
.L i:nth-child(2)::after{right:28%}.L i:nth-child(3)::after{right:18%}
.L i.act{border-color:var(--a);box-shadow:0 0 0 1px var(--a),0 10px 30px -10px rgba(0,0,0,.7)}
.L i.pn{border-radius:0;background:rgba(10,10,14,.88);border:0;box-shadow:none}
.L i.pn::before,.L i.dock::before{display:none}
.L i.pn::after{right:auto;bottom:auto;background:none}
.L i.pn.fl{border-radius:8px;border:1px solid rgba(255,255,255,.08)}
.L i.dock{border-radius:8px;background:rgba(20,20,28,.85)}
.L i.dock::before{display:none}
.L i.dock::after{content:"";position:absolute;inset:22% 8%;background:radial-gradient(circle,var(--a) 40%,transparent 42%) 0 50%/20% 100% repeat-x;opacity:.8}
.L i.pn::after{content:"";position:absolute;top:30%;bottom:30%;left:1.2%;width:14%;background:radial-gradient(circle,var(--a) 42%,transparent 45%) 0 50%/25% 100% repeat-x;opacity:.85}
.L i.tty{inset:0;border:0;border-radius:0;background:#07070a;display:flex}
.L i.tty::before{display:none}
.L i.tty::after{content:"Auxo Linux 3.0 (tty1)\A\A auxo login: _";white-space:pre;inset:8% auto auto 5%;background:none;font:clamp(10px,1.4vw,15px)/1.6 var(--fm);color:#cfcfd8}
.dcmd{display:flex;align-items:center;gap:12px;flex-wrap:wrap;padding:14px 18px;border-top:1px solid var(--line);font:13.5px var(--fm);color:#d4d4dc;background:#0b0b10}
.dcmd .c{display:none}
.dcmd .ok{margin-left:auto;color:#4ade80;font-size:12.5px}
.dcap{display:none;padding:0 18px 16px;background:#0b0b10;color:var(--mut);font-size:14px}
DESKRULES

/* feature rows */
.row{display:grid;grid-template-columns:1fr 1.15fr;gap:72px;align-items:center}
.row>*{min-width:0}
.row+.row{margin-top:120px}
.row.flip .txt{order:2}
@media(max-width:900px){.row{grid-template-columns:1fr;gap:30px}.row.flip .txt{order:0}.row+.row{margin-top:80px}}
.row h3{font-size:clamp(26px,2.8vw,36px);margin:14px 0 14px}
.row p{color:var(--mut);font-size:16.5px}
.checks{list-style:none;margin-top:24px;display:grid;gap:11px}
.checks li{display:flex;gap:11px;color:#d6d6de;font-size:15.5px}
.checks svg{width:18px;height:18px;flex:none;margin-top:3px;stroke:var(--a);fill:none;stroke-width:2.4;stroke-linecap:round;stroke-linejoin:round}
.shot{border:1px solid var(--line2);border-radius:8px;overflow:hidden;background:var(--card);box-shadow:0 40px 80px -40px rgba(0,0,0,.9),0 0 0 6px rgba(255,255,255,.015)}
.shot img{width:100%;aspect-ratio:1100/650;object-fit:cover;object-position:top}
.swatches{display:flex;gap:10px;margin-top:24px;flex-wrap:wrap}
.sw{width:32px;height:32px;border-radius:50%;cursor:pointer;border:2px solid transparent;box-shadow:0 0 0 1px var(--line2);transition:transform .2s var(--ease)}
.sw:hover{transform:scale(1.12)}
.swatches input:checked+.sw{border-color:var(--tx);transform:scale(1.08)}
.swatches input:focus-visible+.sw{outline:2px solid var(--a);outline-offset:3px}
.hint{font:13px var(--fm);color:var(--dim);margin-top:12px}
.hint .accname{color:var(--a)}
.picker-row{display:flex;align-items:center;gap:14px;flex-wrap:wrap}.picker-row .swatches{margin:0}.picker-row .hint{margin:0}

/* bento features */
.bento{display:grid;grid-template-columns:repeat(6,1fr);gap:16px}
.bento>div{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:28px;position:relative;overflow:hidden;transition:border-color .25s}
.bento>div:hover{border-color:var(--line2)}
.bento .s3{grid-column:span 3}.bento .s2{grid-column:span 2}.bento .s4{grid-column:span 4}
@media(max-width:960px){.bento{grid-template-columns:1fr 1fr}.bento>div{grid-column:span 1!important}.bento .wide{grid-column:span 2!important}}
@media(max-width:620px){.bento{grid-template-columns:1fr}.bento>div,.bento .wide{grid-column:span 1!important}}
.bento .ico{width:44px;height:44px;border-radius:8px;display:grid;place-items:center;background:color-mix(in srgb,var(--a) 10%,transparent);border:1px solid color-mix(in srgb,var(--a) 25%,transparent)}
.bento svg.i{width:22px;height:22px;fill:none;stroke:var(--a);stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}
.bento h3{font-size:18px;margin:18px 0 8px;letter-spacing:-.015em}
.bento p{color:var(--mut);font-size:15px}
.bento .mini{margin-top:18px;font:12.5px/1.8 var(--fm);background:#0b0b10;border:1px solid var(--line);border-radius:8px;padding:12px 14px;color:#cfcfd8;overflow-x:auto;white-space:pre}
.snaps{margin-top:18px;display:grid;gap:6px}
.snaps span{display:flex;justify-content:space-between;font:12.5px var(--fm);padding:8px 12px;border:1px solid var(--line);border-radius:8px;background:#0b0b10;color:var(--mut)}
.snaps span:first-child{border-color:color-mix(in srgb,var(--a) 45%,transparent);color:var(--tx)}
.snaps span b{color:var(--a);font-weight:600}
.gpus{display:flex;gap:8px;flex-wrap:wrap;margin-top:18px}
.gpus span{font:600 12px var(--fm);padding:6px 10px;border-radius:7px;border:1px solid var(--line2);background:#0b0b10}

/* compare */
.compare{border:1px solid var(--line2);border-radius:8px;overflow:hidden;background:var(--card)}
.compare table{width:100%;border-collapse:collapse;font-size:15px}
.compare th,.compare td{padding:16px 22px;text-align:left;border-top:1px solid var(--line)}
.compare thead th{border-top:0;font:600 13px var(--fm);letter-spacing:.06em;text-transform:uppercase;color:var(--dim);background:#0e0e14}
.compare thead th.us{color:var(--a)}
.compare td:first-child{color:#dcdce3;font-weight:500}
.compare td{color:var(--mut)}
.compare td.us{color:var(--tx);background:color-mix(in srgb,var(--a) 5%,transparent)}
.compare td.us svg{width:17px;height:17px;stroke:#4ade80;fill:none;stroke-width:2.6;stroke-linecap:round;stroke-linejoin:round;vertical-align:-3px;margin-right:8px}
@media(max-width:700px){.compare{overflow-x:auto}.compare table{min-width:560px}.compare th,.compare td{padding:13px 14px}}

/* faq */
.faq{max-width:880px;margin:0;display:grid;gap:10px}
.faq details{border:1px solid var(--line);border-radius:8px;background:var(--card);transition:border-color .2s}
.faq details[open]{border-color:var(--line2)}
.faq summary{list-style:none;cursor:pointer;padding:18px 22px;font:600 16.5px var(--fh);display:flex;justify-content:space-between;gap:16px;align-items:center}
.faq summary::-webkit-details-marker{display:none}
.faq summary::after{content:"+";font:400 22px var(--fm);color:var(--a);transition:transform .25s var(--ease)}
.faq details[open] summary::after{transform:rotate(45deg)}
.faq details p{padding:0 22px 20px;color:var(--mut)}

/* cta band */
.band{padding:0 0 112px}
.band .inner{border-left:3px solid var(--a)!important;position:relative;overflow:hidden;border:1px solid var(--line2);border-radius:8px;padding:56px 48px;display:grid;grid-template-columns:auto 1fr auto;gap:40px;align-items:center;background:var(--card)}
.band .mk{width:56px;height:56px}
.band h2{margin:0 0 6px;font-size:clamp(26px,3vw,36px)}
.band p{color:var(--mut)}
.band .cta{margin:0;justify-content:flex-end}
@media(max-width:860px){.band .inner{border-left:3px solid var(--a)!important;grid-template-columns:1fr;text-align:center;padding:44px 24px}.band .mk{margin:0 auto}.band .cta{justify-content:center}}

/* download page */
.phead{padding:80px 0 60px;text-align:left;background:linear-gradient(180deg,#0a0c0f,var(--bg));border-bottom:1px solid var(--line);position:relative;overflow:hidden}
.phead::before{display:none;content:"";position:absolute;left:50%;top:-280px;width:1000px;height:600px;transform:translateX(-50%);background:radial-gradient(closest-side,color-mix(in srgb,var(--a) 13%,transparent),transparent);pointer-events:none}
.phead .wrap{position:relative}
.phead .logo{width:60px;height:60px;margin:0 auto 24px}
.phead h1{font-size:clamp(38px,5.4vw,64px);margin:14px 0 16px}
.phead p{color:var(--mut);font-size:18px;max-width:620px}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:26px}
.chips span{font:500 12.5px var(--fm);padding:6px 12px;border-radius:5px;border:1px solid var(--line2);background:rgba(17,17,23,.7);color:var(--mut)}
.chips span b{color:var(--tx);font-weight:600}
.platforms{display:grid;grid-template-columns:repeat(3,1fr);gap:20px}
@media(max-width:980px){.platforms{grid-template-columns:1fr}}
.plat{background:var(--card);border:1px solid var(--line);border-radius:8px;overflow:hidden;display:flex;flex-direction:column;transition:border-color .25s,transform .25s var(--ease)}
.plat:hover{border-color:var(--line2)}
.plat.rec{border-color:color-mix(in srgb,var(--a) 55%,var(--line2))}
.plat .art{height:180px;display:grid;place-items:center;background:radial-gradient(60% 90% at 50% 100%,color-mix(in srgb,var(--a) 14%,transparent),transparent),#0c0c11;border-bottom:1px solid var(--line);position:relative}
.plat .art::after{content:"";position:absolute;inset:0;background-image:linear-gradient(var(--line) 1px,transparent 1px),linear-gradient(90deg,var(--line) 1px,transparent 1px);background-size:24px 24px;-webkit-mask-image:radial-gradient(ellipse at center,#000,transparent 70%);mask-image:radial-gradient(ellipse at center,#000,transparent 70%);opacity:.5}
.plat .art svg{height:124px;width:auto;position:relative;z-index:1}
.plat .rtag{position:absolute;z-index:2;top:14px;left:14px;font:600 11px var(--fm);letter-spacing:.08em;text-transform:uppercase;background:var(--a);color:#0a0a0d;padding:4px 9px;border-radius:5px}
.plat .body{padding:26px;display:flex;flex-direction:column;flex:1}
.plat h2{font-size:23px;margin:0 0 6px}
.plat .sub{color:var(--mut);font-size:15px;margin-bottom:18px}
.pc{list-style:none;display:grid;gap:8px;font-size:14.5px;margin-bottom:26px}
.pc li{display:grid;grid-template-columns:18px 1fr;gap:8px;color:#d0d0d8}
.pc li::before{font:700 15px/1.5 var(--fm)}
.pc .p::before{content:"+";color:#4ade80}
.pc .m::before{content:"−";color:#f87171}
.plat .go{margin-top:auto}
.plat .go .btn{width:100%}

.dlsec+.dlsec{margin-top:56px}
.dlsec .top{display:flex;align-items:flex-end;justify-content:space-between;gap:20px;flex-wrap:wrap;margin-bottom:18px}
.dlsec h2{font-size:28px;margin:10px 0 4px}
.dlsec .top p{color:var(--mut)}
.auxo .doclink{font-size:14.5px;color:var(--a)}
.doclink:hover{text-decoration:underline}
.dltable{border:1px solid var(--line2);border-radius:8px;overflow:hidden;background:var(--card)}
.dlrow{display:grid;grid-template-columns:auto 1fr auto;gap:24px;align-items:center;padding:24px}
.dlrow>*{min-width:0}
.arch{font:600 13px var(--fm);border:1px solid var(--line2);border-radius:7px;padding:7px 11px;background:#0c0c11}
.dlinfo b{display:block;font-size:16.5px}
.dlinfo em{font-style:normal;color:var(--mut);font-size:14px;display:block;margin-top:2px}
.dlinfo span{display:block;margin-top:6px;font:12.5px var(--fm);color:var(--dim);word-break:break-all}
.dlacts{display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-end}
@media(max-width:860px){.dlrow{grid-template-columns:1fr}.dlacts{justify-content:flex-start}.arch{justify-self:start}}

/* tabs (css radios) */
.tabs{border:1px solid var(--line2);border-radius:8px;background:var(--card);overflow:hidden}
.tabs>*{min-width:0}
.tablist{display:flex;gap:4px;padding:8px;border-bottom:1px solid var(--line);background:#0e0e14;overflow-x:auto}
.tablist label{padding:9px 16px;border-radius:6px;cursor:pointer;font-weight:600;font-size:14.5px;color:var(--mut);white-space:nowrap;display:flex;align-items:center;gap:8px}
.tablist label:hover{color:var(--tx)}
.tablist svg{width:16px;height:16px}
.panel{display:none;padding:28px}
TABRULES
.steps{list-style:none;counter-reset:s;display:grid;gap:18px}
.steps li{counter-increment:s;display:grid;grid-template-columns:32px 1fr;gap:14px;color:#d2d2da;font-size:15.5px}
.steps li>span{min-width:0}
.steps li::before{content:counter(s);display:grid;place-items:center;width:30px;height:30px;border-radius:8px;background:#1a1a23;border:1px solid var(--line2);font:600 13px var(--fm);color:var(--a)}
.code{font:12.5px/1.7 var(--fm);background:#0a0a0e;border:1px solid var(--line);border-radius:6px;padding:12px 14px;margin-top:10px;overflow-x:auto;color:#d4d4dc;white-space:pre}
.code .tm{user-select:none}
.note{margin-top:22px;padding:14px 16px;border-radius:8px;border:1px solid color-mix(in srgb,var(--a) 40%,transparent);background:color-mix(in srgb,var(--a) 7%,transparent);font-size:14.5px;display:grid;grid-template-columns:20px 1fr;gap:10px}
.note svg{width:20px;height:20px;fill:none;stroke:var(--a);stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.two{display:grid;grid-template-columns:1fr 1fr;gap:20px}
.two>*{min-width:0}
@media(max-width:900px){.two{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:28px}
.card h3{font-size:20px;margin-bottom:8px}
.card>p{color:var(--mut);font-size:15px}
.rel{list-style:none;display:grid;gap:12px;margin-top:18px}
.rel li{display:grid;grid-template-columns:auto 1fr;gap:12px;font-size:15px;color:#d2d2da}
.rel li b{font:600 11px var(--fm);letter-spacing:.06em;text-transform:uppercase;padding:3px 7px;border-radius:5px;height:max-content;margin-top:2px}
.rel .new{color:#0a0a0d;background:var(--a)}
.rel .fix{color:#4ade80;border:1px solid rgba(74,222,128,.35)}
.reqs{display:grid;grid-template-columns:repeat(4,1fr);border:1px solid var(--line2);border-radius:8px;overflow:hidden;background:var(--card)}
.reqs div{padding:24px;border-left:1px solid var(--line)}
.reqs div:first-child{border-left:0}
.reqs small{display:block;font:600 11.5px var(--fm);letter-spacing:.1em;text-transform:uppercase;color:var(--dim);margin-bottom:8px}
.reqs b{font:600 17px var(--fh)}
.reqs span{display:block;color:var(--mut);font-size:14px}
@media(max-width:820px){.reqs{grid-template-columns:1fr 1fr}.reqs div:nth-child(3){border-left:0}.reqs div:nth-child(n+3){border-top:1px solid var(--line)}}
.subtle{font-size:14px;color:var(--dim);margin-top:16px}
.subtle a{color:var(--mut);text-decoration:underline;text-decoration-color:var(--line2)}

/* footer */
.auxo footer{border-top:1px solid var(--line);background:var(--bg2);padding:64px 0 32px;font-size:14.5px;color:var(--mut)}
.fgrid{display:grid;grid-template-columns:1.6fr 1fr 1fr 1fr 1fr;gap:32px}
@media(max-width:760px){.fgrid{grid-template-columns:1fr 1fr}.fgrid>div:first-child{grid-column:span 2}}
.fgrid .brand{color:var(--tx);margin-bottom:14px}
.fgrid p{max-width:320px}
.fgrid h4{font:600 12px var(--fm);letter-spacing:.1em;text-transform:uppercase;color:var(--dim);margin-bottom:16px}
.fgrid ul{list-style:none;display:grid;gap:10px}
.fgrid a:hover{color:var(--tx)}
.fbot{margin-top:48px;padding-top:24px;border-top:1px solid var(--line);display:flex;flex-wrap:wrap;gap:10px 24px;justify-content:space-between;font-size:13.5px;color:var(--dim)}

/* scroll reveal (browsers with scroll-driven animations only) */
@supports (animation-timeline:view()){
 @media(prefers-reduced-motion:no-preference){
  .rv{animation:rv linear both;animation-timeline:view();animation-range:entry 0% cover 22%}
  @keyframes rv{from{opacity:0;transform:translateY(28px)}to{opacity:1;transform:none}}
 }
}
@media(prefers-reduced-motion:reduce){*,*::before,*::after{animation:none!important;transition:none!important}.rise,.logo .d3{opacity:1!important;transform:none!important}.logo .d1,.logo .d2{stroke-dashoffset:0!important}}

/* steps bar (download) */
.stepsbar{border-bottom:1px solid var(--line);background:var(--bg2);position:sticky;top:72px;z-index:30}
.stepsbar .wrap{display:grid;grid-template-columns:repeat(4,1fr)}
.stepsbar a{display:flex;align-items:center;gap:12px;padding:16px 18px;border-left:1px solid var(--line);font-size:14.5px;font-weight:600;color:var(--mut);transition:color .2s,background .2s}
.stepsbar a:first-child{border-left:0}
.stepsbar a:hover{color:var(--tx);background:var(--card)}
.stepsbar b{display:grid;place-items:center;width:26px;height:26px;border-radius:6px;border:1px solid var(--line2);font:600 12.5px var(--fm);color:var(--a);flex:none}
@media(max-width:760px){.stepsbar{position:static}.stepsbar .wrap{grid-template-columns:1fr 1fr}.stepsbar a:nth-child(3){border-left:0}.stepsbar a:nth-child(n+3){border-top:1px solid var(--line)}}
.reqs.compact{margin-top:20px}
/* kali-style home */
.khero{position:relative;padding:88px 0 96px;overflow:hidden;background:linear-gradient(180deg,#0a0c0f 0%,var(--bg) 100%);border-bottom:1px solid var(--line)}
.khero .hgrid{grid-template-columns:1.05fr .95fr}
@media(max-width:980px){.khero .hgrid{grid-template-columns:1fr}.khero .topo{max-width:280px}}
.khero .hridge{position:absolute;left:0;right:0;bottom:0;width:100%;height:45%;pointer-events:none}
.khero .hridge path:nth-child(1){fill:#13171d}.khero .hridge path:nth-child(2){fill:#101318}
.khero .lbl{font:600 14px var(--fb);color:var(--a);margin-bottom:16px;display:block}
.khero h1{font-size:clamp(38px,4.6vw,60px);margin:0 0 22px}
.khero h1 span{color:var(--mut);font-weight:600}
.topo{width:100%;max-width:560px;margin-left:auto;display:block}
.topo .c{fill:none;stroke:var(--a);stroke-linejoin:round;stroke-linecap:round}
@media(max-width:980px){.topo{max-width:360px;margin:0 auto}}
.cards5{display:grid;grid-template-columns:repeat(4,1fr);gap:16px}
@media(max-width:1100px){.cards5{grid-template-columns:repeat(2,1fr)}}
@media(max-width:700px){.cards5{grid-template-columns:1fr}}
.cards5>div{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:26px 22px;border-top:2px solid var(--a)}
.cards5 svg{width:30px;height:30px;fill:none;stroke:var(--a);stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}
.cards5 h3{font-size:17px;margin:16px 0 8px}
.cards5 p{color:var(--mut);font-size:14.5px}
.logos{display:grid;grid-template-columns:repeat(6,1fr);border:1px solid var(--line);border-radius:8px;overflow:hidden;background:var(--line);gap:1px}
@media(max-width:900px){.logos{grid-template-columns:repeat(3,1fr)}}
.logos div{background:var(--card);display:flex;flex-direction:column;align-items:center;justify-content:center;gap:12px;padding:28px 10px;color:var(--mut);font-size:13.5px;font-weight:500;transition:background .2s,color .2s}
.logos svg{width:38px;height:38px;fill:#8b93a0;transition:fill .2s}
.logos div:hover{background:var(--card2);color:var(--tx)}
.logos div:hover svg{fill:var(--h)}
.every{display:grid;grid-template-columns:repeat(4,1fr);gap:16px}
@media(max-width:900px){.every{grid-template-columns:1fr 1fr}}
.every a{display:flex;flex-direction:column;background:var(--card);border:1px solid var(--line);border-radius:8px;padding:28px 24px;transition:border-color .2s,background .2s}
.every a:hover{border-color:var(--a);background:var(--card2)}
.every svg{width:40px;height:40px;fill:none;stroke:var(--tx);stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round}
.every h3{font-size:18px;margin:18px 0 6px}
.every p{color:var(--mut);font-size:14.5px}
.every .more{display:inline-block;margin-top:auto;padding-top:14px;font-size:14px;font-weight:600;color:var(--a)}
.shots3{display:grid;grid-template-columns:repeat(3,1fr);gap:20px}
@media(max-width:900px){.shots3{grid-template-columns:1fr}}
.shots3 figure{background:var(--card);border:1px solid var(--line);border-radius:8px;overflow:hidden}
.shots3 img{width:100%;aspect-ratio:1100/650;object-fit:cover;object-position:top;border-bottom:1px solid var(--line)}
.shots3 figcaption{padding:18px 20px}
.shots3 b{display:block;font-size:16px;margin-bottom:4px}
.shots3 span{color:var(--mut);font-size:14.5px}
.news{display:grid;grid-template-columns:repeat(3,1fr);gap:20px}
@media(max-width:900px){.news{grid-template-columns:1fr}}
.news a{display:flex;flex-direction:column;background:var(--card);border:1px solid var(--line);border-radius:8px;padding:26px;transition:border-color .2s}
.news a:hover{border-color:var(--line2)}
.news time{font:500 13px var(--fm);color:var(--dim)}
.news h3{font-size:19px;margin:10px 0 8px}
.news p{color:var(--mut);font-size:14.5px;flex:1}
.news .more{margin-top:16px;font-size:14px;font-weight:600;color:var(--a)}
.L-plasma img{position:absolute;inset:0;width:100%;height:100%;object-fit:cover;object-position:top}
.sec-foot{margin-top:28px}

/* v7 */
.khero.v7 .hgrid{grid-template-columns:.95fr 1.05fr}
@media(max-width:980px){.khero.v7 .hgrid{grid-template-columns:1fr}}
.khero.v7 h1{font-size:clamp(38px,4.8vw,62px)}
.heroshot{border:1px solid var(--line2);border-radius:10px;overflow:hidden;background:#15181e;box-shadow:0 50px 90px -40px rgba(0,0,0,.9),0 0 0 8px rgba(255,255,255,.02);background-size:0}
.heroshot .shotwrap{background-size:cover;background-position:top;aspect-ratio:1100/650}
.heroshot img{width:100%;height:100%;object-fit:cover;object-position:top}
.auxo a.inl{color:var(--a);text-decoration:underline;text-decoration-color:color-mix(in srgb,var(--a) 40%,transparent);text-underline-offset:3px}
.how3{display:grid;grid-template-columns:repeat(3,1fr);gap:20px}
@media(max-width:960px){.how3{grid-template-columns:1fr}}
.how3>div{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:28px;display:flex;flex-direction:column}
.how3 .num{display:inline-grid;place-items:center;width:34px;height:34px;border-radius:7px;font:700 15px var(--fh);color:#0a0a0d;background:var(--a)}
.how3 h3{font-size:19px;margin:16px 0 8px}
.how3 p{color:var(--mut);font-size:15px;margin-bottom:18px}
.how3 .term{margin-top:auto}
.how3 .term pre{font-size:12.5px;padding:14px 16px}
.softtabs{border:1px solid var(--line2);border-radius:8px;background:var(--card);overflow:hidden}
.stl{display:flex;gap:2px;border-bottom:1px solid var(--line);background:#12151a;overflow-x:auto}
.stl label{padding:15px 20px;font-weight:600;font-size:14.5px;color:var(--mut);cursor:pointer;border-bottom:2px solid transparent;white-space:nowrap}
.stl label:hover{color:var(--tx)}
.spanel{display:none;padding:26px}
.sdesc{color:var(--mut);margin-bottom:20px}
.spanel .logos{grid-template-columns:repeat(8,1fr)}
@media(max-width:1000px){.spanel .logos{grid-template-columns:repeat(4,1fr)}}
@media(max-width:560px){.spanel .logos{grid-template-columns:repeat(2,1fr)}}
.pkgs{margin-top:18px;font:12.5px/1.8 var(--fm);color:var(--mut)}
.pkgs span{font:600 11px var(--fb);letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin-right:12px}
SOFTRULES
/* docs */
.docs{display:grid;grid-template-columns:230px 1fr;gap:56px;padding-top:56px;padding-bottom:96px}
.docs>*{min-width:0}
@media(max-width:900px){.docs{grid-template-columns:1fr}.dnav{position:static!important;flex-direction:row!important;flex-wrap:wrap}}
.dnav{position:sticky;top:96px;align-self:start;display:flex;flex-direction:column;gap:2px;border-left:1px solid var(--line);padding-left:4px}
.dnav p{font:600 11.5px var(--fb);letter-spacing:.08em;text-transform:uppercase;color:var(--dim);padding:0 12px 10px}
.dnav a{padding:7px 12px;border-radius:5px;font-size:14.5px;color:var(--mut)}
.dnav a:hover{color:var(--tx);background:var(--card)}
.dbody{max-width:820px}
.dbody h2{font-size:28px;margin:56px 0 14px;padding-top:12px;scroll-margin-top:90px}
.dbody h2:first-child{margin-top:0}
.dbody p{color:#c9ced6;margin:0 0 14px;font-size:16px}
.dbody .code{margin:6px 0 18px}
.dbody ul.dl,.dbody ol.dsteps{margin:0 0 16px;display:grid;gap:8px;color:#c9ced6}
.dbody ul.dl li{padding-left:18px;position:relative}
.dbody ul.dl li::before{content:"";position:absolute;left:2px;top:.65em;width:6px;height:6px;border-radius:2px;background:var(--a)}
.dbody ol.dsteps{counter-reset:d}
.dbody ol.dsteps li{counter-increment:d;padding-left:34px;position:relative}
.dbody ol.dsteps li::before{content:counter(d);position:absolute;left:0;top:1px;width:24px;height:24px;border-radius:6px;display:grid;place-items:center;border:1px solid var(--line2);font:600 12px var(--fm);color:var(--a)}
.dtable{border:1px solid var(--line2);border-radius:8px;overflow:auto;margin:10px 0 8px}
.dtable table{width:100%;border-collapse:collapse;font-size:14.5px;min-width:560px}
.dtable th{text-align:left;font:600 12px var(--fb);letter-spacing:.06em;text-transform:uppercase;color:var(--dim);background:#12151a;padding:12px 16px}
.dtable td{padding:12px 16px;border-top:1px solid var(--line);color:#c9ced6;vertical-align:top}
.dtable td:first-child{white-space:nowrap}
.dtable td:first-child code{color:var(--a);background:transparent;border:0;padding:0}
.accs{display:flex;flex-wrap:wrap;gap:8px;margin:4px 0 16px}
.accchip{display:inline-flex;align-items:center;gap:8px;padding:6px 12px;border:1px solid var(--line2);border-radius:6px;font:500 13px var(--fm);background:var(--card)}
.accchip i{width:14px;height:14px;border-radius:50%}
.dbody .faq{max-width:none}

.relline{margin-top:18px;padding-top:18px;border-top:1px solid var(--line);font-size:14px;color:var(--dim);max-width:560px}
.relline b{color:var(--tx)}
/* download v8 */
.pgrid{display:grid;grid-template-columns:1.2fr .8fr;gap:56px;align-items:center}
.pgrid>*{min-width:0}
@media(max-width:900px){.pgrid{grid-template-columns:1fr;gap:32px}}
.qd{background:var(--card);border:1px solid var(--line2);border-radius:10px;padding:24px;display:grid;gap:10px;box-shadow:0 30px 60px -30px rgba(0,0,0,.8)}
.qtop{display:flex;align-items:center;gap:14px;margin-bottom:8px}
.qtop .mk{width:40px;height:40px}
.qtop b{display:block;font:700 18px var(--fh)}
.qtop span{font:12.5px var(--fm);color:var(--dim)}
.qd .btn{width:100%}
.qlinks{display:flex;flex-wrap:wrap;gap:6px 18px;margin-top:6px;font-size:14px}
.auxo .qlinks a{color:var(--mut);text-decoration:underline;text-decoration-color:var(--line2);text-underline-offset:3px}
.auxo .qlinks a:hover{color:var(--tx)}
.rtable{display:grid;margin-top:16px;border:1px solid var(--line);border-radius:8px;overflow:hidden}
.rtable a{display:grid;grid-template-columns:60px 1fr auto;gap:12px;align-items:center;padding:12px 16px;border-top:1px solid var(--line);font-size:14.5px;transition:background .2s}
.rtable a:first-child{border-top:0}
.rtable a:hover{background:var(--card2)}
.rtable b{font:600 14px var(--fm);color:var(--a)}
.rtable span{font:13px var(--fm);color:var(--mut)}
.rtable em{font-style:normal;font-size:12.5px;color:var(--dim)}
""" + acc_css

I = {
 "dl": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v12M6 11l6 6 6-6M5 20h14"/></svg>',
 "mag": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="6" cy="12" r="2.5"/><circle cx="18" cy="6" r="2.5"/><circle cx="18" cy="18" r="2.5"/><path d="M8.2 10.8l7.6-3.6M8.2 13.2l7.6 3.6"/></svg>',
 "shield": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3l8 3v6c0 4.5-3.4 8.2-8 9-4.6-.8-8-4.5-8-9V6z"/><path d="M9 12l2 2 4-4"/></svg>',
 "discord": '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M20.3 4.4A19.6 19.6 0 0 0 15.4 3l-.6 1.3a18.3 18.3 0 0 0-5.6 0L8.6 3a19.5 19.5 0 0 0-4.9 1.4C.6 9 0 13.6.3 18.1a19.8 19.8 0 0 0 6 3l1.3-2.1a12.8 12.8 0 0 1-2-1l.5-.4a14 14 0 0 0 12 0l.5.4c-.6.4-1.3.7-2 1l1.3 2.1a19.7 19.7 0 0 0 6-3c.5-5.2-.8-9.8-3.6-13.7zM8.3 15.4c-1.2 0-2.2-1.1-2.2-2.4s1-2.4 2.2-2.4 2.2 1.1 2.2 2.4-1 2.4-2.2 2.4zm7.4 0c-1.2 0-2.2-1.1-2.2-2.4s1-2.4 2.2-2.4 2.2 1.1 2.2 2.4-1 2.4-2.2 2.4z"/></svg>',
 "check": '<svg viewBox="0 0 24 24"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg>',
 "info": '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>',
 "win": '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M3 5.5l7.5-1v7H3zM11.5 4.3L21 3v8.5h-9.5zM3 12.5h7.5v7L3 18.5zM11.5 12.5H21V21l-9.5-1.3z"/></svg>',
 "apple": '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M16.4 12.6c0-2.4 2-3.5 2-3.6-1.1-1.6-2.8-1.8-3.4-1.8-1.4-.2-2.8.8-3.5.8s-1.8-.8-3-.8c-1.5 0-3 .9-3.8 2.3-1.6 2.8-.4 7 1.2 9.3.8 1.1 1.7 2.4 2.9 2.3 1.2 0 1.6-.7 3-.7s1.8.7 3 .7c1.3 0 2.1-1.1 2.8-2.3.9-1.3 1.3-2.6 1.3-2.6s-2.5-1-2.5-3.6zM14.2 5.6c.6-.8 1.1-1.9 1-3-1 0-2.1.7-2.8 1.5-.6.7-1.2 1.8-1 2.9 1 .1 2.1-.6 2.8-1.4z"/></svg>',
 "tux": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3c-2.2 0-3.5 1.8-3.5 4.2 0 1.2.2 2-.6 3.3C6.6 12.3 5 14.6 5 17c0 1.5.8 2.6 2 3h10c1.2-.4 2-1.5 2-3 0-2.4-1.6-4.7-2.9-6.5-.8-1.3-.6-2.1-.6-3.3C15.5 4.8 14.2 3 12 3z"/><path d="M10 8h.01M14 8h.01M11 10.5h2"/></svg>',
 "vbox": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2l9 5v10l-9 5-9-5V7z"/><path d="M12 22V12M21 7l-9 5-9-5"/></svg>',
}

def mark(cls="mk"):
    return f'<svg class="{cls}" viewBox="0 0 128 128" aria-hidden="true"><use href="#mark"/></svg>'

def g(x, y, s):
    return f'<g transform="translate({x} {y}) scale({s})">{MARK_PATHS}</g>'

ART_PC = f'<svg viewBox="0 0 200 140" aria-hidden="true"><rect x="22" y="10" width="156" height="98" rx="8" fill="#15151d" stroke="#34344a" stroke-width="2"/><rect x="30" y="18" width="140" height="82" rx="4" fill="#0b0b10"/>{g(78,30,.34)}<path d="M84 108h32l6 18H78z" fill="#1b1b25" stroke="#34344a" stroke-width="2"/><rect x="64" y="126" width="72" height="6" rx="3" fill="#2a2a38"/></svg>'
ART_VM = f'<svg viewBox="0 0 200 140" aria-hidden="true"><rect x="46" y="8" width="132" height="92" rx="8" fill="#121219" stroke="#2c2c3c" stroke-width="2"/><rect x="34" y="20" width="132" height="92" rx="8" fill="#15151d" stroke="#34344a" stroke-width="2"/><rect x="22" y="32" width="132" height="98" rx="8" fill="#17171f" stroke="#3a3a52" stroke-width="2"/><path d="M22 48h132" stroke="#3a3a52" stroke-width="2"/><circle cx="34" cy="40" r="3" fill="#3a3a52"/><circle cx="44" cy="40" r="3" fill="#3a3a52"/><circle cx="54" cy="40" r="3" fill="#3a3a52"/>{g(66,56,.34)}</svg>'
ART_USB = f'<svg viewBox="0 0 200 140" aria-hidden="true"><rect x="118" y="52" width="46" height="36" rx="4" fill="#1b1b25" stroke="#34344a" stroke-width="2"/><rect x="128" y="62" width="8" height="6" rx="1" fill="#34344a"/><rect x="146" y="62" width="8" height="6" rx="1" fill="#34344a"/><rect x="128" y="72" width="8" height="6" rx="1" fill="#34344a"/><rect x="146" y="72" width="8" height="6" rx="1" fill="#34344a"/><rect x="30" y="36" width="92" height="68" rx="12" fill="#15151d" stroke="#3a3a52" stroke-width="2"/>{g(56,46,.32)}</svg>'

# ---------- desktop switcher ----------
def box(x, y, w, h, cls=""):
    return f'<i class="{cls}" style="left:{x}%;top:{y}%;width:{w}%;height:{h}%"></i>'

DESKS = [
 ("plasma", "KDE Plasma 6", "Default · installs offline", False, "The default desktop: floating windows, a bottom panel and Auxo theming throughout.",
  box(8, 8, 50, 58, "act") + box(44, 26, 46, 52, "t") + box(0, 93.5, 100, 6.5, "pn")),
 ("hyprland", "Hyprland", "Animated tiling", True, "Auxo rice: waybar, wofi, mako, hyprlock and kitty with gaps and rounded corners.",
  box(1.5, 1.5, 97, 6, "pn fl") + box(1.5, 10, 57, 88, "t nt act") + box(60, 10, 38.5, 43, "nt") + box(60, 55, 38.5, 43, "t nt")),
 ("sway", "Sway", "i3 on Wayland", True, "Auxo rice: waybar, foot and mako with thin gaps and your accent on the focused window.",
  box(0, 0, 100, 5.5, "pn") + box(.8, 7, 49, 92, "t nt act") + box(50.6, 7, 48.6, 45.5, "nt") + box(50.6, 53.5, 48.6, 45.5, "t nt")),
 ("i3", "i3", "X11 tiling", True, "Auxo rice: polybar, rofi and picom. Classic tiling with no wasted space.",
  box(0, 0, 33.4, 94, "t nt act") + box(33.4, 0, 33.3, 94, "nt") + box(66.7, 0, 33.3, 94, "t nt") + box(0, 94, 100, 6, "pn")),
 ("gnome", "GNOME", "Gestures and a dock", False, "Stock GNOME with your accent mapped to the system colour.",
  box(0, 0, 100, 5, "pn") + box(18, 13, 64, 64, "act") + box(33, 85, 34, 10, "dock")),
 ("xfce", "Xfce", "Light and classic", False, "Lightweight and familiar: top panel, bottom dock, stacked windows.",
  box(0, 0, 100, 6, "pn") + box(10, 14, 48, 56, "") + box(36, 26, 50, 54, "t act") + box(34, 88, 32, 9, "dock")),
 ("cinnamon", "Cinnamon", "Familiar layout", False, "A traditional desktop with a full-width bottom panel.",
  box(12, 10, 56, 62, "act") + box(40, 24, 48, 56, "t") + box(0, 93, 100, 7, "pn")),
 ("none", "No desktop", "Text only", False, "Just a login prompt. Add a desktop later with one command.",
  '<i class="tty"></i>'),
]

desk_rules = []
for k, *_ in DESKS:
    desk_rules.append(f"#dk-{k}:checked~.dview .L-{k},#dk-{k}:checked~.dview .c-{k}{{display:block}}"
                      f"#dk-{k}:checked~.dview .dcap-{k}{{display:block}}"
                      f"#dk-{k}:checked~.dtabs label[for=dk-{k}]{{border-color:var(--a);background:color-mix(in srgb,var(--a) 7%,var(--card))}}"
                      f"#dk-{k}:focus-visible~.dtabs label[for=dk-{k}]{{outline:2px solid var(--a);outline-offset:2px}}")
CSS = CSS.replace("DESKRULES", "\n".join(desk_rules))

TABSETS = {"os": ["win", "mac", "lin"], "vm": ["vbox", "qemu", "vmw"]}
tab_rules = []
for grp, keys in TABSETS.items():
    for k in keys:
        tab_rules.append(f"#t-{grp}-{k}:checked~.tabs .p-{grp}-{k}{{display:block}}"
                         f"#t-{grp}-{k}:checked~.tabs label[for=t-{grp}-{k}]{{background:var(--card2);color:var(--tx);box-shadow:inset 0 0 0 1px var(--line2)}}"
                         f"#t-{grp}-{k}:focus-visible~.tabs label[for=t-{grp}-{k}]{{outline:2px solid var(--a)}}")
CSS = CSS.replace("TABRULES", "\n".join(tab_rules))

RIDGE = ('<svg class="ridge" viewBox="0 0 1600 500" preserveAspectRatio="none" aria-hidden="true">'
         '<path d="M0 360 L180 230 L300 300 L470 150 L620 280 L760 190 L900 300 L1080 120 L1240 260 L1380 200 L1600 320 V500 H0Z"/>'
         '<path d="M0 430 L220 330 L380 400 L560 300 L720 390 L900 320 L1100 410 L1300 330 L1460 390 L1600 360 V500 H0Z"/></svg>')

def head(title, desc, canon):
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title>
<meta name="description" content="{desc}">
<link rel="canonical" href="{canon}">
<link rel="icon" href="{FAVICON}">
<meta name="theme-color" content="#08080b">
<meta property="og:type" content="website">
<meta property="og:site_name" content="Auxo Linux">
<meta property="og:title" content="{title}">
<meta property="og:description" content="{desc}">
<meta property="og:url" content="{canon}">
<meta property="og:image" content="{KDE_SHOT}">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="{title}">
<meta name="twitter:description" content="{desc}">
<meta name="twitter:image" content="{KDE_SHOT}">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600&family=Inter+Tight:wght@600;700&family=JetBrains+Mono:wght@400;500;600&display=swap">
<style>{CSS}</style>
</head>
<body>
<a class="skip" href="#main">Skip to content</a>
<svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs>
<linearGradient id="lg" x1="0" y1="1" x2="1" y2="0"><stop offset="0" style="stop-color:var(--a)"/><stop offset="1" style="stop-color:var(--a2)"/></linearGradient>
<symbol id="mark" viewBox="0 0 128 128">{MARK_PATHS}</symbol>
</defs></svg>
<div class="auxo">
"""

def nav(page):
    cur = lambda p: ' aria-current="page"' if p == page else ""
    h = "" if page == "home" else HOME
    return f"""<header class="nav"><div class="wrap">
<a class="brand" href="{HOME}">{mark()}auxo<small>3.0</small></a>
<nav class="links" aria-label="Main"><a href="{DL}"{cur('dl')}>Get Auxo</a><a href="{DOCS}"{cur('docs')}>Docs</a><a href="{h}#desktops">Desktops</a><a href="{h}#software">Software</a><a href="{h}#news">News</a><a href="{DISCORD}">Community</a></nav>
<div class="right"><a class="icon-link" href="{DISCORD}" aria-label="Auxo Discord">{I['discord']}</a><a class="btn primary sm" href="{DL}">{I['dl']}Get Auxo</a></div>
</div></header>
"""

def footer():
    return f"""<footer><div class="wrap">
<div class="fgrid">
  <div><a class="brand" href="{HOME}">{mark()}auxo</a><p>An Arch-based rolling distro. Pick your desktop, kernel, shell and colour when you install, and change them later without reinstalling.</p></div>
  <div><h4>Get Auxo</h4><ul><li><a href="{DL}#installer">Installer image</a></li><li><a href="{DL}#vm">Virtual machines</a></li><li><a href="{DL}#usb">Make a USB stick</a></li><li><a href="{DL}#verify">Verify a download</a></li></ul></div>
  <div><h4>Explore</h4><ul><li><a href="{HOME}#features">Why Auxo</a></li><li><a href="{HOME}#software">Software</a></li><li><a href="{HOME}#desktops">Desktops</a></li><li><a href="{HOME}#news">News</a></li></ul></div>
  <div><h4>Docs</h4><ul><li><a href="{DOCS}#tweak">auxo-tweak</a></li><li><a href="{DOCS}#snapshots">Snapshots</a></li><li><a href="{DOCS}#gaming">Gaming</a></li><li><a href="{DOCS}#trouble">Troubleshooting</a></li></ul></div>
  <div><h4>Community</h4><ul><li><a href="{DISCORD}">Discord</a></li><li><a href="https://www.reddit.com/r/DistroHub/">r/DistroHub</a></li></ul></div>
</div>
<div class="fbot"><span>© 2026 Auxo Linux</span><span>Built on Arch Linux. Not affiliated with the Arch Linux project.</span></div>
</div></footer>
</div>
</body>
</html>
"""

def checks(items):
    return '<ul class="checks">' + "".join(f"<li>{I['check']}<span>{t}</span></li>" for t in items) + "</ul>"

def shot(src, alt, title="Auxo 3.0"):
    return f'<div class="shot rv"><div class="bar mute"><i></i><i></i><i></i><b>{title}</b></div><img src="{src}" alt="{alt}" loading="lazy"></div>'

# ================= HOME =================
swatches = "".join(
    f'<input class="vh" type="radio" name="acc" id="acc-{n}"{" checked" if n == "amber" else ""} aria-label="{n} accent">'
    f'<label class="sw" for="acc-{n}" title="{n}" style="background:linear-gradient(135deg,{a},{b})"></label>'
    for n, a, b in ACCENTS)
acc_dots = "".join(f'<i style="background:linear-gradient(135deg,{a},{b})"></i>' for n, a, b in ACCENTS)

d_radios = "".join(f'<input class="vh" type="radio" name="desk" id="dk-{k}"{" checked" if k == "plasma" else ""} aria-label="Show {n}">' for k, n, *_ in DESKS)
d_tabs = "".join(f'<label for="dk-{k}"><b>{n}</b><small>{s}</small>{"<em>Rice</em>" if r else ""}</label>' for k, n, s, r, *_ in DESKS)
d_layers = "".join(f'<div class="L L-{k}">{lay}</div>' for k, n, s, r, cap, lay in DESKS)
d_cmds = "".join(f'<span class="c c-{k}">auxo-tweak desktop {k}</span>' for k, *_ in DESKS)
d_caps = "".join(f'<p class="dcap dcap-{k}">{cap}</p>' for k, n, s, r, cap, lay in DESKS)

bento = f"""
<div class="s4 wide rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M3 12a9 9 0 1 0 3-6.7"/><path d="M3 4v4h4"/><path d="M12 7v5l3 2"/></svg></div>
 <h3>Every update is a restore point</h3><p>Before each update, Auxo takes a btrfs snapshot. If something breaks, pick an older snapshot in the GRUB menu, boot it, and keep it with <code>auxo-rollback</code>. Your /home is never touched.</p>
 <div class="snaps"><span>▸ before update · today <b>booted</b></span><span>▸ before update · yesterday</span><span>▸ before update · last week</span></div></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><rect x="3" y="6" width="18" height="12" rx="2"/><path d="M7 10h4v4H7zM15 10h2M15 14h2"/></svg></div>
 <h3>GPU drivers, detected</h3><p>The installer checks your graphics card and sets up the right drivers.</p>
 <div class="gpus"><span>NVIDIA · nvidia-open</span><span>AMD · Vulkan</span><span>Intel · Vulkan</span></div></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M6 11h4M8 9v4"/><path d="M7 5h10a5 5 0 0 1 5 5v2a5 5 0 0 1-9 3h-2a5 5 0 0 1-9-3v-2a5 5 0 0 1 5-5z"/></svg></div>
 <h3>One-command gaming</h3><p>Steam, GameMode, MangoHud and Gamescope, plus the kernel tweaks games expect.</p>
 <div class="mini"><span class="ta">❯</span> auxo-tweak gaming on</div></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 3v18M3 12h18"/></svg></div>
 <h3>Safe dual boot</h3><p>Installs alongside Windows or another Linux and reuses your existing EFI partition.</p></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1"/></svg></div>
 <h3>Installs offline</h3><p>No internet still gets you a complete Plasma desktop. You're told what was skipped.</p></div>
<div class="s3 wide rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M4 17l6-6-6-6M12 19h8"/></svg></div>
 <h3>Updates that read the news first</h3><p><code>auxo-update</code> checks Arch news for manual steps, takes a snapshot, then updates packages, AUR and Flatpak together.</p>
 <div class="mini"><span class="ta">❯</span> auxo-update
<span class="tm">:: arch news · nothing needs attention</span>
<span class="tm">:: snapshot · before update</span>
<span class="tg">✓</span> 42 packages updated</div></div>
<div class="s3 wide rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" rx="3"/><path d="M9 9h6v6H9z"/></svg></div>
 <h3>Sensible defaults</h3><p>zram swap, a git-aware prompt in your accent, an animated boot splash and a fast text login screen that works everywhere, including VMs.</p>
 <div class="mini"><span class="ta">❯</span> auxo-fetch --json | jq .accent
<span class="tm">"</span><span class="accname"></span><span class="tm">"</span></div></div>
"""

compare_rows = [
    ("Installer", "Graphical, guided", "Command line, by hand"),
    ("Desktop at first boot", "Ready, themed, configured", "You install and configure it"),
    ("GPU drivers", "Detected and installed", "You pick and install them"),
    ("Snapshots", "Before every update, bootable from GRUB", "Set up yourself if you want them"),
    ("Change desktop later", "One command", "Install, configure, clean up"),
    ("Packages", "Arch repos, AUR, Flatpak", "Arch repos, AUR, Flatpak"),
]
compare_html = "".join(f'<tr><td>{a}</td><td class="us">{I["check"]}{b}</td><td>{c}</td></tr>' for a, b, c in compare_rows)

faqs = [
    ("Is Auxo really Arch?", "Yes. Auxo uses the official Arch Linux repositories and pacman, and it follows Arch's rolling release. Auxo adds an installer, themes and its own tools on top. You can use the Arch Wiki and the AUR as normal."),
    ("Can I switch desktops without breaking things?", "That's the point of <code>auxo-tweak</code>. It installs the new desktop, applies the Auxo setup, keeps your old config files as <code>*.auxo-bak</code>, and sets the login screen to the new desktop. If anything goes wrong, boot an earlier snapshot from GRUB."),
    ("Does it work in VirtualBox or other VMs?", "Yes. Use the VM image from the download page, set VirtualBox graphics to VMSVGA with 3D acceleration off, and choose <b>Boot existing OS</b> after installing."),
    ("Will it work with my NVIDIA card?", "The installer detects your GPU. Recent NVIDIA cards get the open NVIDIA kernel modules with modesetting on, and AMD and Intel get Mesa with Vulkan and video acceleration."),
    ("Does Secure Boot work?", "Not yet. Turn Secure Boot off in your firmware settings before booting the USB stick."),
    ("Is it free?", "Yes. Auxo is free to download and use."),
]
faq_html = "".join(f"<details><summary>{q}</summary><p>{a}</p></details>" for q, a in faqs)

LOGOS = open("assets/logos.html").read()
TOPO = '<svg class="topo" viewBox="0 0 520 440" aria-hidden="true"><g transform="translate(10.4 -26.8) scale(3.9)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="0.72" opacity="0.1"/></g><g transform="translate(42.4 29.2) scale(3.4)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="0.82" opacity="0.14"/></g><g transform="translate(71.2 79.6) scale(2.95)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="0.95" opacity="0.18"/></g><g transform="translate(100.0 130.0) scale(2.5)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="1.12" opacity="0.24"/></g><g transform="translate(125.6 174.8) scale(2.1)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="1.33" opacity="0.3"/></g><g transform="translate(157 118) scale(1.6)">{MARK_PATHS}</g></svg>'.replace("{MARK_PATHS}", MARK_PATHS)
import re as _re
d_layers_k = _re.sub(r'<div class="L L-plasma">.*?</div>', f'<div class="L L-plasma" role="img" aria-label="Auxo KDE Plasma 6 desktop"><img src="{KDE_SHOT}" alt="" loading="lazy"></div>', d_layers, count=1)

IC = {
 "time": '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>',
 "arch": '<svg viewBox="0 0 24 24"><path d="M12 3L4 21h3.5L12 11l4.5 10H20z"/></svg>',
 "swap": '<svg viewBox="0 0 24 24"><path d="M7 7h11l-3-3M17 17H6l3 3"/></svg>',
 "undo": '<svg viewBox="0 0 24 24"><path d="M3 12a9 9 0 1 0 3-6.7"/><path d="M3 4v4h4"/></svg>',
 "people": '<svg viewBox="0 0 24 24"><circle cx="9" cy="8" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0"/><circle cx="17" cy="9" r="2.5"/><path d="M16 14.5a5 5 0 0 1 5.5 5"/></svg>',
 "metal": '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/></svg>',
 "usb": '<svg viewBox="0 0 24 24"><rect x="7" y="9" width="10" height="13" rx="2"/><path d="M9 9V3h6v6M10.5 5.5h.01M13.5 5.5h.01"/></svg>',
 "vm": '<svg viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="14" rx="2"/><rect x="6" y="8" width="8" height="6" rx="1"/><path d="M8 21h8"/></svg>',
 "dual": '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="8" height="16" rx="1.5"/><rect x="13" y="4" width="8" height="16" rx="1.5"/></svg>',
}

home = None
ICONS2 = json.load(open("assets/icons.json"))
def _hx(h):
    r,g_,b=int(h[:2],16),int(h[2:4],16),int(h[4:],16)
    return '#ffffff' if (0.299*r+0.587*g_+0.114*b)<60 else '#'+h
def logo_cells(items):
    return "".join(f'<div style="--h:{_hx(i["hex"])}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="{i["path"]}"/></svg>{i["name"]}</div>' for i in items)

SOFT = [
 ("desktops", "Desktops", "Chosen in the installer, switchable later with <code>auxo-tweak desktop</code>.",
  "plasma · gnome · xfce · cinnamon · hyprland · sway · i3 · none"),
 ("gaming", "Gaming", "The installer's Gaming bundle. Add the tuning later with <code>auxo-tweak gaming on</code>.",
  "steam · gamemode · mangohud · lutris · wine · winetricks · prismlauncher · discord · lib32 Vulkan drivers"),
 ("dev", "Development", "Compilers, editors and containers, including SDL for game development.",
  "base-devel · git · cmake · ninja · gdb · clang · code · neovim · python-pip · nodejs · npm · rustup · go · docker · docker-compose · sdl2-compat · sdl3"),
 ("creative", "Creative", "Video, audio and image editing.",
  "mpv · vlc · obs-studio · gimp · krita · kdenlive · audacity"),
 ("web", "Web, office & system", "Firefox is always included. Everything else is one checkbox.",
  "firefox · vivaldi · thunderbird · libreoffice-fresh · okular · flatpak · btop · ufw · cups · 7zip · virt-manager · qemu-desktop"),
]
ICONS2["creative"] = [i for i in ICONS2["creative"] if i["name"] != "Blender"]
soft_rules = "\n".join(
    f"#sw-{k}:checked~.softtabs .sp-{k}{{display:block}}#sw-{k}:checked~.softtabs label[for=sw-{k}]{{color:var(--tx);border-color:var(--a)}}"
    f"#sw-{k}:focus-visible~.softtabs label[for=sw-{k}]{{outline:2px solid var(--a)}}" for k, *_ in SOFT)
CSS = CSS.replace("SOFTRULES", soft_rules)
soft_radios = "".join(f'<input class="vh" type="radio" name="soft" id="sw-{k}"{" checked" if i == 0 else ""}>' for i, (k, *_) in enumerate(SOFT))
soft_labels = "".join(f'<label for="sw-{k}">{t}</label>' for k, t, *_ in SOFT)
soft_panels = "".join(
    f'<div class="spanel sp-{k}"><p class="sdesc">{d}</p><div class="logos">{logo_cells(ICONS2[k])}</div>'
    f'<p class="pkgs"><span>Packages</span>{pk}</p></div>' for k, t, d, pk in SOFT)

HERO_SHOT = (f'<figure class="heroshot" aria-label="The Auxo Linux KDE Plasma 6 desktop">'
             f'<div class="bar mute"><i></i><i></i><i></i><b>Auxo Linux 3.0 · KDE Plasma 6</b></div>'
             f'<div class="shotwrap" style="background-image:url({imgs[0]})"><img src="{KDE_SHOT}" alt="" loading="eager"></div></figure>')

home = head("Auxo Linux — Arch, set up the way you want it",
            "Auxo Linux is an Arch-based rolling distro. Pick your desktop, kernel, shell and accent colour in a graphical installer, and change any of them later with one command.",
            "https://auxolinux.com/") + nav("home") + f"""
<main id="main">
<section class="khero v7">{RIDGE.replace('class="ridge"','class="hridge"')}<div class="wrap hgrid">
  <div>
    <a class="badge rise" href="{DL}#releases"><b>.</b>Auxo Linux 3.0 is now available →</a>
    <h1 class="rise r1">Arch Linux, set up the way you want it.</h1>
    <p class="lede rise r2">An open-source, Arch-based distribution with a graphical installer. Pick your desktop, kernel, shell and accent colour, then change any of them later with one command. No reinstall.</p>
    <div class="cta rise r3">
      <a class="btn primary lg" href="{DL}">{I['dl']}Download Auxo 3.0</a>
      <a class="btn lg" href="{DOCS}">Read the docs</a>
    </div>
    <p class="meta-line rise r4"><span>{I['check']}Free and open source</span><span>{I['check']}Arch repos and AUR</span><span>{I['check']}UEFI and BIOS</span><span>{I['check']}VM image</span></p>
    <p class="relline rise r4">Latest release <b>3.0</b> · {STD['date']} · {STD['size']} · <a class="inl" href="{DL}#releases">Release notes</a></p>
  </div>
  <div class="rise r2">{HERO_SHOT}</div>
</div></section>

<div class="strip"><div class="wrap">
  <div><b>8</b><span>desktops, three pre-riced</span></div>
  <div><b>7</b><span>accent colours</span></div>
  <div><b>4</b><span>kernels to choose from</span></div>
  <div><b>1</b><span>command to change it all</span></div>
</div></div>

<section id="features"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">01</span> Why Auxo</p><h2>Arch, without the weekend of setup</h2></div><p>Everything you'd normally configure by hand after installing Arch, done during install and still yours to change.</p></div>
  <div class="cards5">
    <div class="rv">{IC['time']}<h3>Ready in minutes</h3><p>A guided installer sets up your desktop, drivers, snapshots and dual boot in one go.</p></div>
    <div class="rv">{IC['arch']}<h3>Still pure Arch</h3><p>Official Arch repositories, pacman, the AUR and the Arch Wiki all work as normal.</p></div>
    <div class="rv">{IC['swap']}<h3>Change anything later</h3><p><code>auxo-tweak</code> swaps your desktop, kernel, shell or accent colour without reinstalling.</p></div>
    <div class="rv">{IC['undo']}<h3>Undo bad updates</h3><p>Every pacman transaction takes a btrfs snapshot you can boot from GRUB and roll back to.</p></div>
  </div>
</div></section>

<section class="alt" id="desktops"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">02</span> Desktops</p><h2>Choose the desktop you prefer</h2></div><p>Pick one in the installer, or switch whenever you like. Click through them to see the command you'd run.</p></div>
  <div class="switcher">
    {d_radios}
    <div class="dtabs" role="radiogroup" aria-label="Desktop">{d_tabs}</div>
    <div class="dview rv">
      <div class="screen">{RIDGE}{d_layers_k}</div>
      <div class="dcmd"><span class="ta">❯</span>{d_cmds}<span class="ok">✓ no reinstall</span></div>
      {d_caps}
    </div>
  </div>
</div></section>

<section id="how"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">03</span> How it works</p><h2>Install once. Change anything. Undo mistakes.</h2></div><p>Three tools cover the whole life of your system. <a class="inl" href="{DOCS}">Full command reference →</a></p></div>
  <div class="how3">
    <div class="rv"><span class="num">1</span><h3>Install</h3><p>A guided installer. Pick your desktop, accent, kernel, shell and extra software, and Auxo sets up drivers, snapshots and dual boot.</p>
      <div class="term"><pre><span class="tm"># in the live session</span>
<span class="ta">❯</span> auxo-welcome
<span class="tg">✓</span> Install Auxo Linux</pre></div></div>
    <div class="rv"><span class="num">2</span><h3>Change anything</h3><p><code>auxo-tweak</code> runs the same code as the installer, so a change later gives the same result as a fresh install.</p>
      <div class="term"><pre><span class="ta">❯</span> auxo-tweak desktop hyprland
<span class="ta">❯</span> auxo-tweak kernel linux-zen
<span class="ta">❯</span> auxo-tweak accent <span class="accname"></span></pre></div></div>
    <div class="rv"><span class="num">3</span><h3>Undo mistakes</h3><p>Snapshots are taken before and after every pacman run. Boot one from GRUB, then make it permanent.</p>
      <div class="term"><pre><span class="ta">❯</span> auxo-update
<span class="tm">:: news · snapshot · repo · AUR · Flatpak</span>
<span class="ta">❯</span> auxo-rollback --list</pre></div></div>
  </div>
</div></section>

<section class="alt" id="software"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">04</span> Software</p><h2>Everything you need, one checkbox away</h2></div><p>These are the installer's real software bundles. Tick what you want during setup, or install it later with pacman.</p></div>
  {soft_radios}
  <div class="softtabs rv">
    <div class="stl" role="tablist">{soft_labels}</div>
    {soft_panels}
  </div>
</div></section>

<section id="tour"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">05</span> Installer</p><h2>A guided install, start to finish</h2></div><p>Real screenshots from Auxo 3.0.</p></div>
  <div class="shots3">
    <figure class="rv"><img src="{imgs[0]}" alt="Auxo live session with the welcome app" loading="lazy"><figcaption><b>Try it live</b><span>Boot the USB stick and the Auxo welcome app opens.</span></figcaption></figure>
    <figure class="rv"><img src="{imgs[1]}" alt="Choosing a desktop in the Auxo installer" loading="lazy"><figcaption><b>Pick your desktop</b><span>Plasma, GNOME, Xfce, Cinnamon, or a ready-made Hyprland, Sway or i3 setup.</span></figcaption></figure>
    <figure class="rv"><img src="{imgs[2]}" alt="Choosing an accent colour in the Auxo installer" loading="lazy"><figcaption><b>Choose an accent</b><span>One colour for the boot menu, splash, login, prompt, terminal and bars.</span></figcaption></figure>
  </div>
  <div class="sec-foot picker-row"><span class="hint">Try an accent on this page:</span><div class="swatches" role="radiogroup" aria-label="Accent colour">{swatches}</div></div>
</div></section>

<section class="alt" id="everywhere"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">06</span> Platforms</p><h2>Auxo everywhere</h2></div><p>Install it on your PC, run it from a USB stick, or try it in a virtual machine.</p></div>
  <div class="every">
    <a class="rv" href="{DL}#installer">{IC['metal']}<h3>Bare metal</h3><p>Install on your PC or laptop with full access to your hardware.</p><span class="more">Get the installer →</span></a>
    <a class="rv" href="{DL}#usb">{IC['usb']}<h3>Live USB</h3><p>Boot a full KDE Plasma desktop without touching your drive.</p><span class="more">Make a USB stick →</span></a>
    <a class="rv" href="{DL}#vm">{IC['vm']}<h3>Virtual machines</h3><p>VirtualBox, QEMU/KVM and VMware, with a dedicated VM image.</p><span class="more">Get the VM image →</span></a>
    <a class="rv" href="{DL}#installer">{IC['dual']}<h3>Dual boot</h3><p>Install alongside Windows or another Linux and pick at startup.</p><span class="more">Get the installer →</span></a>
  </div>
</div></section>

<section id="news"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">07</span> News</p><h2>Latest news</h2></div><p>Releases and announcements.</p></div>
  <div class="news">
    <a class="rv" href="{DL}"><time>2026-09-26</time><h3>Auxo Linux 3.0 released</h3><p>One-command desktop switching with auxo-tweak, ready-made Hyprland, Sway and i3 setups, and bootable snapshots.</p><span class="more">Download 3.0 →</span></a>
    <a class="rv" href="{DL}#vm"><time>2026-09-27</time><h3>A dedicated VM image</h3><p>A separate image for VirtualBox, QEMU/KVM and VMware, plus step-by-step VM setup instructions.</p><span class="more">VM setup →</span></a>
    <a class="rv" href="{DOCS}"><time>2026-09-27</time><h3>New: the Auxo docs</h3><p>Every auxo-tweak command, snapshots and rollback, gaming setup and troubleshooting, in one place.</p><span class="more">Read the docs →</span></a>
  </div>
</div></section>

<div class="band" style="padding-top:0"><div class="wrap"><div class="inner rv">
  {mark()}
  <div><h2>Get started with Auxo Linux</h2><p>Free download for your PC or a virtual machine. Most installs take about ten minutes.</p></div>
  <div class="cta"><a class="btn primary lg" href="{DL}">{I['dl']}Get Auxo</a><a class="btn lg" href="{DISCORD}">{I['discord']}Discord</a></div>
</div></div></div>
</main>
""" + footer()


# ================= DOWNLOAD =================
def dlrow(f, title, desc):
    return f"""<div class="dlrow">
  <span class="arch">x86_64</span>
  <div class="dlinfo"><b>{title}</b><em>{desc}</em><span>{f['name']} · {f['size']} · {f['date']}</span></div>
  <div class="dlacts"><a class="btn primary" href="{f['url']}">{I['dl']}Download</a><a class="btn" href="{f['torrent']}">{I['mag']}Torrent</a><a class="btn" href="{f['details']}" title="MD5 and SHA-1 are listed on the Internet Archive page">{I['shield']}Checksums</a></div>
</div>"""

def radios(grp, keys):
    return "".join(f'<input class="vh" type="radio" name="t-{grp}" id="t-{grp}-{k}"{" checked" if i == 0 else ""}>' for i, k in enumerate(keys))

N = STD["name"]
os_tabs = f"""{radios('os', TABSETS['os'])}
<div class="tabs rv">
 <div class="tablist" role="tablist"><label for="t-os-win">{I['win']}Windows</label><label for="t-os-mac">{I['apple']}macOS</label><label for="t-os-lin">{I['tux']}Linux</label></div>
 <div class="panel p-os-win"><ol class="steps">
  <li><span>Download <b>Rufus</b>, <b>Ventoy</b> or <b>balenaEtcher</b> and plug in a USB stick (8 GB or larger). Everything on it will be erased.</span></li>
  <li><span>In Rufus, select the Auxo image and click <b>Start</b>. When asked, choose <b>Write in DD Image mode</b>. With Ventoy, install Ventoy to the stick and copy the image onto it.</span></li>
  <li><span>Restart, open the boot menu (usually F12, F11, F8 or Esc) and choose the USB stick. If Windows is using Fast Startup, shut down fully first.</span></li>
 </ol></div>
 <div class="panel p-os-mac"><ol class="steps">
  <li><span>Download <b>balenaEtcher</b>, choose the Auxo image and your USB stick, then click <b>Flash</b>. Or use the Terminal:</span></li>
  <li><span>Find the stick, unmount it and write the image (replace <code>diskN</code> with your stick):<div class="code"><span class="tm">$ </span>diskutil list
<span class="tm">$ </span>diskutil unmountDisk /dev/diskN
<span class="tm">$ </span>sudo dd if={N} of=/dev/rdiskN bs=4m</div></span></li>
  <li><span>Intel Macs: hold <b>Option</b> at startup and pick the USB stick. Apple Silicon Macs can't boot Auxo, because it's built for x86_64.</span></li>
 </ol></div>
 <div class="panel p-os-lin"><ol class="steps">
  <li><span>Use <b>Fedora Media Writer</b>, <b>GNOME Disks</b> (Restore Disk Image) or <b>Ventoy</b>, or write it with dd:</span></li>
  <li><span>Find your USB stick and write the image (replace <code>sdX</code> with your stick, not a partition):<div class="code"><span class="tm">$ </span>lsblk
<span class="tm">$ </span>sudo dd if={N} of=/dev/sdX bs=4M status=progress oflag=sync</div></span></li>
  <li><span>Reboot, open your boot menu and choose the USB stick.</span></li>
 </ol></div>
</div>"""

VMN = VM["name"]
vm_tabs = f"""{radios('vm', TABSETS['vm'])}
<div class="tabs rv">
 <div class="tablist" role="tablist"><label for="t-vm-vbox">{I['vbox']}VirtualBox</label><label for="t-vm-qemu">{I['tux']}QEMU / KVM</label><label for="t-vm-vmw">{I['vbox']}VMware</label></div>
 <div class="panel p-vm-vbox"><ol class="steps">
  <li><span>New VM: type <b>Linux</b>, version <b>Arch Linux (64-bit)</b> or <b>Other Linux (64-bit)</b>. Give it at least <b>4 GB</b> RAM, 2 CPUs and a <b>25 GB</b> disk.</span></li>
  <li><span>Settings → Display: graphics controller <b>VMSVGA</b>, video memory <b>128 MB</b>, <b>3D acceleration off</b>.</span></li>
  <li><span>Attach the VM image as the optical drive, start the VM and install as normal.</span></li>
  <li><span>After the restart, the boot menu appears again. Choose <code>Boot existing OS</code>, or remove the image from the optical drive.</span></li>
 </ol></div>
 <div class="panel p-vm-qemu"><ol class="steps">
  <li><span>With <b>virt-manager</b>: create a VM from the image, choose <b>Arch Linux</b> as the OS, 4 GB RAM and a 25 GB disk. Video: <b>Virtio</b>.</span></li>
  <li><span>Or from a terminal:<div class="code"><span class="tm">$ </span>qemu-img create -f qcow2 auxo.qcow2 25G
<span class="tm">$ </span>qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 4G \\
    -drive file=auxo.qcow2,if=virtio -cdrom {VMN} \\
    -boot d -vga virtio</div></span></li>
  <li><span>After installing, start it again without <code>-cdrom</code> and <code>-boot d</code>.</span></li>
 </ol></div>
 <div class="panel p-vm-vmw"><ol class="steps">
  <li><span>New VM: guest OS <b>Linux</b>, version <b>Other Linux 6.x kernel 64-bit</b>. 4 GB RAM, 2 CPUs, 25 GB disk.</span></li>
  <li><span>Point the CD/DVD drive at the VM image and install as normal.</span></li>
  <li><span>When it restarts, choose <code>Boot existing OS</code>, or disconnect the CD/DVD drive.</span></li>
 </ol></div>
</div>
<div class="note">{I['info']}<span><b>Using a VM?</b> After installing, choose <b>Boot existing OS</b> in the boot menu, or the installer just starts again.</span></div>"""

dl = head("Get Auxo Linux — Download",
          "Download Auxo Linux 3.0: the installer image for your PC, or the VM image for VirtualBox, QEMU/KVM and VMware.",
          "https://auxolinux.com/download/") + nav("dl") + f"""
<main id="main">
<div class="phead"><div class="wrap pgrid">
  <div>
    <p class="eyebrow rise">Auxo Linux 3.0</p>
    <h1 class="rise r1">Get Auxo Linux</h1>
    <p class="rise r2">Four steps from download to desktop. Most installs take about ten minutes.</p>
    <div class="chips rise r3"><span><b>3.0</b> latest</span><span><b>x86_64</b></span><span><b>UEFI</b> + BIOS</span><span>Hosted on the <b>Internet Archive</b></span></div>
  </div>
  <div class="qd rise r2">
    <div class="qtop">{mark()}<div><b>Auxo Linux 3.0</b><span>x86_64 · {STD['size']} · {STD['date']}</span></div></div>
    <a class="btn primary lg" href="{STD['url']}">{I['dl']}Download for PC</a>
    <a class="btn lg" href="{VM['url']}">{I['dl']}Download for virtual machines</a>
    <p class="qlinks"><a href="{STD['torrent']}">Torrent</a><a href="{STD['details']}">Checksums</a><a href="#choose">Which one do I need?</a></p>
  </div>
</div></div>

<nav class="stepsbar" aria-label="Download steps"><div class="wrap">
  <a href="#choose"><b>1</b>Choose a platform</a><a href="#get"><b>2</b>Download</a><a href="#usb"><b>3</b>Prepare USB or VM</a><a href="#step4"><b>4</b>Verify and install</a>
</div></nav>

<section id="choose" style="padding-top:72px;padding-bottom:48px"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">01</span> Step one</p><h2>Choose your platform</h2></div><p>Every image is the same Auxo. Pick the one that fits where you're installing it.</p></div>
  <div class="platforms">
    <a class="plat rec rv" href="#installer">
      <div class="art"><span class="rtag">Recommended</span>{ART_PC}</div>
      <div class="body"><h2>Installer image</h2><p class="sub">Install Auxo on your PC or laptop from a USB stick.</p>
        <ul class="pc"><li class="p">Direct access to your hardware and GPU</li><li class="p">Dual boot with Windows or Linux</li><li class="p">Full speed, full desktop</li><li class="m">Needs a USB stick and a reboot</li></ul>
        <div class="go"><span class="btn primary">{I['dl']}Get the installer</span></div></div>
    </a>
    <a class="plat rv" href="#vm">
      <div class="art">{ART_VM}</div>
      <div class="body"><h2>Virtual machines</h2><p class="sub">Run Auxo inside VirtualBox, QEMU/KVM or VMware.</p>
        <ul class="pc"><li class="p">Try it without touching your PC</li><li class="p">Works on Windows, macOS and Linux hosts</li><li class="p">Built with the VM boot fixes</li><li class="m">Slower graphics than real hardware</li></ul>
        <div class="go"><span class="btn">{I['dl']}Get the VM image</span></div></div>
    </a>
    <a class="plat rv" href="#installer">
      <div class="art">{ART_USB}</div>
      <div class="body"><h2>Live boot</h2><p class="sub">Run Auxo from a USB stick without installing.</p>
        <ul class="pc"><li class="p">Nothing changes on your drive</li><li class="p">Full KDE Plasma desktop to try</li><li class="p">Install from it whenever you're ready</li><li class="m">Changes are lost when you shut down</li></ul>
        <div class="go"><span class="btn">{I['dl']}Uses the installer image</span></div></div>
    </a>
  </div>
  <div class="reqs compact rv">
    <div><small>Processor</small><b>64-bit x86</b><span>Intel or AMD (x86_64)</span></div>
    <div><small>Memory</small><b>4 GB</b><span>2 GB minimum</span></div>
    <div><small>Storage</small><b>20 GB</b><span>SSD recommended</span></div>
    <div><small>Firmware</small><b>UEFI or BIOS</b><span>Secure Boot off</span></div>
  </div>
</div></section>

<section id="get" class="alt"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">02</span> Step two</p><h2>Download the image</h2></div><p>Images are hosted on the Internet Archive. Use the torrent if the direct download is slow.</p></div>
  <div class="dlsec rv" id="installer">
    <div class="top"><div><p class="eyebrow">Installer image</p><h2>Bare metal and live boot</h2><p>Write it to a USB stick, boot it, and try Auxo live or install it.</p></div><a class="doclink" href="#usb">Make a USB stick →</a></div>
    <div class="dltable">{dlrow(STD, "Auxo Linux 3.0 — Installer", "Live KDE Plasma session with the graphical installer")}</div>
  </div>
  <div class="dlsec rv" id="vm">
    <div class="top"><div><p class="eyebrow">Virtual machines</p><h2>VirtualBox, QEMU/KVM and VMware</h2><p>Attach it to a new VM as an optical drive and install as normal.</p></div><a class="doclink" href="#vmsetup">VM setup →</a></div>
    <div class="dltable">{dlrow(VM, "Auxo Linux 3.0 — VM", "The same installer, built for virtual machines")}</div>
  </div>
  <p class="subtle">Looking for an older version? See <a href="#releases">all releases</a>.</p>
</div></section>

<section id="usb"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">03</span> Step three</p><h2>Make a bootable USB stick</h2></div><p>Pick the computer you're using right now.</p></div>
  {os_tabs}
</div></section>

<section id="vmsetup" style="padding-top:0"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">03</span> Or, for a VM</p><h2>Set up your virtual machine</h2></div><p>Settings that work in each app. Skip this if you're installing on a PC.</p></div>
  {vm_tabs}
</div></section>

<section class="alt" id="step4"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">04</span> Step four</p><h2>Verify and install</h2></div><p>Check the download, boot it, and click <b>Install Auxo Linux</b> in the welcome app.</p></div>
  <div class="two">
    <div class="card rv" id="install"><h3>Install Auxo</h3><p>Boot the USB stick or VM, then:</p>
      <ol class="steps">
        <li><span>Choose <b>Auxo Linux</b> in the boot menu. The live KDE Plasma desktop starts.</span></li>
        <li><span>Connect to Wi-Fi from the welcome app if you want the extra software.</span></li>
        <li><span>Click <b>Install Auxo Linux</b>. Pick your language, keyboard and disk (erase, or install alongside another OS).</span></li>
        <li><span>Choose your desktop, accent, kernel, shell and any extra software, then create your user.</span></li>
        <li><span>Check the summary and click <b>Install</b>. It takes a few minutes.</span></li>
        <li><span>When it says <b>All done</b>, restart and remove the USB stick. In a VM, choose <b>Boot existing OS</b>.</span></li>
      </ol></div>
    <div class="card rv" id="verify"><h3>Verify your download</h3><p>Make sure the image downloaded completely before you write it. Compare the result with the SHA-1 on the file's <b>Checksums</b> page.</p>
      <div class="code"><span class="tm"># Linux</span>
<span class="tm">$ </span>sha1sum {N}
<span class="tm"># macOS</span>
<span class="tm">$ </span>shasum -a 1 {N}
<span class="tm"># Windows (PowerShell)</span>
<span class="tm">&gt; </span>Get-FileHash {N} -Algorithm SHA1</div>
      <p class="subtle" style="margin-top:14px">A mismatch means the download was cut off or corrupted. Download it again, or use the torrent.</p></div>
  </div>
</div></section>



<section id="releases"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow">Releases</p><h2>Release notes and older versions</h2></div><p>Auxo is a rolling release: once installed, <code>auxo-update</code> keeps you current. You don't need to reinstall for new versions.</p></div>
  <div class="two">
    <div class="card rv"><h3>What's new in 3.0</h3><p>The biggest Auxo release so far.</p>
      <ul class="rel">
        <li><b class="new">New</b><span><code>auxo-tweak</code> switches desktop, kernel, shell and accent without reinstalling</span></li>
        <li><b class="new">New</b><span>Ready-made Hyprland, Sway and i3 setups</span></li>
        <li><b class="new">New</b><span>Animated boot splash in your accent colour</span></li>
        <li><b class="new">New</b><span><code>auxo-tweak gaming on</code> for Steam, GameMode, MangoHud and Gamescope</span></li>
        <li><b class="fix">Fix</b><span>A fast text login screen that works on every desktop and in VMs</span></li>
        <li><b class="fix">Fix</b><span>Installed systems boot reliably in VirtualBox</span></li>
      </ul></div>
    <div class="card rv"><h3>All releases</h3><p>Every image is kept on the Internet Archive.</p>
      <div class="rtable">
        <a href="{STD['details']}"><b>3.0</b><span>2026-09-27</span><em>Latest</em></a>
        <a href="https://archive.org/details/auxo-linux-2026.09.26-x86_64"><b>3.0</b><span>2026-09-26</span><em>First 3.0 build</em></a>
        <a href="https://archive.org/details/auxo-linux-2026.07.19-0048-x86_64"><b>2.0</b><span>2026-07-19</span><em>Older</em></a>
        <a href="https://archive.org/details/auxo-linux-2026.07.09-x86_64"><b>1.0</b><span>2026-07-09</span><em>Older</em></a>
      </div></div>
  </div>
</div></section>

<div class="band" style="padding-top:96px"><div class="wrap"><div class="inner rv">
  {mark()}
  <div><h2>Need help installing?</h2><p>Ask in the Discord for install help, rices and release news.</p></div>
  <div class="cta"><a class="btn primary lg" href="{DISCORD}">{I['discord']}Join the Discord</a><a class="btn lg" href="{HOME}">Back to home</a></div>
</div></div></div>
</main>
""" + footer()

# ================= DOCS =================
def cmdrow(cmd, desc):
    return f'<tr><td><code>{cmd}</code></td><td>{desc}</td></tr>'

TWEAK = [
 ("auxo-tweak", "Open the interactive menu."),
 ("auxo-tweak info", "Show your current Auxo settings."),
 ("auxo-tweak accent [NAME]", "Set the accent colour for GRUB, the boot splash, login screen, prompt, terminals and bars. With no name, lists the colours."),
 ("auxo-tweak desktop NAME", "Install and switch to <code>plasma</code>, <code>gnome</code>, <code>xfce</code>, <code>cinnamon</code>, <code>hyprland</code>, <code>sway</code>, <code>i3</code> or <code>none</code>. Add <code>--replace</code> to remove the live Plasma desktop."),
 ("auxo-tweak rice NAME", "Re-apply the Auxo dotfiles for <code>hyprland</code>, <code>sway</code> or <code>i3</code>. Existing files are kept as <code>*.auxo-bak</code>."),
 ("auxo-tweak kernel NAME", "Switch to <code>linux</code>, <code>linux-lts</code>, <code>linux-zen</code> or <code>linux-hardened</code>."),
 ("auxo-tweak shell NAME", "Change your shell to <code>zsh</code>, <code>fish</code> or <code>bash</code>."),
 ("auxo-tweak snapshots on|off|status", "Bootable btrfs snapshots with snapper, snap-pac and grub-btrfs."),
 ("auxo-tweak drivers", "Detect your GPU and install the right driver. <code>--prune</code> removes drivers and VM guest tools for hardware that isn't there."),
 ("auxo-tweak gaming on|off|status", "Steam, GameMode, MangoHud, Gamescope and gaming tweaks. Options: <code>--no-steam</code>, <code>--user NAME</code>, and <code>--purge</code> with <code>off</code>."),
 ("auxo-tweak splash on|off|status", "The animated Auxo boot splash (Plymouth) in your accent colour."),
 ("auxo-tweak mirrors", "Rank the fastest mirrors with reflector. <code>--country</code> limits the search."),
 ("auxo-tweak zram on|off", "Compressed swap in RAM."),
 ("auxo-tweak multilib on", "Enable the 32-bit repository (needed for Steam and Wine)."),
 ("auxo-tweak aur", "Install the paru AUR helper."),
 ("auxo-tweak service NAME on|off", "Turn <code>bluetooth</code>, <code>cups</code>, <code>sshd</code>, <code>ufw</code> or <code>power-profiles</code> on or off."),
]
tweak_table = "".join(cmdrow(c, d) for c, d in TWEAK)
acc_list = "".join(f'<span class="accchip"><i style="background:linear-gradient(135deg,{a},{b})"></i>{n}</span>' for n, a, b in ACCENTS)

DOC_NAV = [("start", "Getting started"), ("tweak", "auxo-tweak"), ("desktops", "Desktops and rices"), ("accents", "Accent colours"),
           ("snapshots", "Snapshots and rollback"), ("updating", "Updating"), ("gaming", "Gaming"), ("fetch", "auxo-fetch"),
           ("vm", "Virtual machines"), ("trouble", "Troubleshooting"), ("faq", "FAQ")]
doc_nav = "".join(f'<a href="#{k}">{t}</a>' for k, t in DOC_NAV)

docs = head("Auxo Linux documentation",
            "Documentation for Auxo Linux: every auxo-tweak command, desktops, accent colours, snapshots and rollback, updating, gaming and troubleshooting.",
            "https://auxolinux.com/docs/") + nav("docs") + f"""
<main id="main">
<div class="phead"><div class="wrap">
  <p class="eyebrow rise">Documentation</p>
  <h1 class="rise r1">Auxo Linux docs</h1>
  <p class="rise r2">Everything you can do after installing, from switching desktops to rolling back a bad update. Auxo is Arch underneath, so the <a class="inl" href="https://wiki.archlinux.org/">Arch Wiki</a> applies too.</p>
</div></div>

<div class="wrap docs">
  <aside class="dnav"><p>On this page</p>{doc_nav}</aside>
  <article class="dbody">

<h2 id="start">Getting started</h2>
<p>After your first boot, the <b>Auxo welcome</b> app opens. It's a shortcut to the most common changes: accent colour, drivers, snapshots and updates. You can reopen it any time by running <code>auxo-welcome</code>.</p>
<p>Three commands cover almost everything:</p>
<div class="code"><span class="ta">auxo-tweak</span>      <span class="tm"># change desktop, kernel, shell, accent, drivers and more</span>
<span class="ta">auxo-update</span>     <span class="tm"># safe system update with a snapshot first</span>
<span class="ta">auxo-rollback</span>   <span class="tm"># make a snapshot your live system again</span></div>
<p>Commands that change the system ask for your password automatically, so you don't need to type <code>sudo</code>.</p>

<h2 id="tweak">auxo-tweak</h2>
<p>Run <code>auxo-tweak</code> on its own for an interactive menu, or use a command directly. The installer runs these same commands, so changing something later gives the same result as choosing it during install.</p>
<div class="dtable"><table><thead><tr><th>Command</th><th>What it does</th></tr></thead><tbody>{tweak_table}</tbody></table></div>

<h2 id="desktops">Desktops and rices</h2>
<p>Switch desktops at any time:</p>
<div class="code"><span class="ta">❯</span> auxo-tweak desktop hyprland</div>
<p>Auxo installs the desktop, applies its setup, points the login screen at the new desktop and keeps your previous config files as <code>*.auxo-bak</code>. Log out to start using it.</p>
<ul class="dl">
<li><b>KDE Plasma 6</b>: the default desktop and the live session. It installs even without internet.</li>
<li><b>Hyprland</b> (Auxo rice): waybar, wofi, mako, hyprlock and kitty.</li>
<li><b>Sway</b> (Auxo rice): waybar, foot and mako.</li>
<li><b>i3</b> (Auxo rice): polybar, rofi and picom.</li>
<li><b>GNOME</b>, <b>Xfce</b> and <b>Cinnamon</b>: the standard desktops with your accent applied.</li>
<li><b>none</b>: a text login only. Add a desktop later.</li>
</ul>
<p>Broke your tiling setup? <code>auxo-tweak rice hyprland</code> (or <code>sway</code>, <code>i3</code>) puts the Auxo dotfiles back.</p>

<h2 id="accents">Accent colours</h2>
<p>One setting colours the GRUB menu, the boot splash, the login screen, your prompt, terminals and bars.</p>
<div class="accs">{acc_list}</div>
<div class="code"><span class="ta">❯</span> auxo-tweak accent rose</div>

<h2 id="snapshots">Snapshots and rollback</h2>
<p>On btrfs installs, snapshots are turned on during setup. snap-pac takes a snapshot before and after every pacman transaction, and grub-btrfs adds them to the boot menu under <b>Auxo snapshots</b>. The first snapshot is called <i>Fresh Auxo install</i>.</p>
<p><b>If an update breaks something:</b></p>
<ol class="dsteps">
<li>Restart and pick <b>Auxo snapshots</b> in the GRUB menu.</li>
<li>Boot a snapshot from before the problem. It starts read-only.</li>
<li>Run <code>auxo-rollback</code> and choose that snapshot to make it your system again.</li>
</ol>
<div class="code"><span class="ta">❯</span> auxo-rollback --list   <span class="tm"># list snapshots</span>
<span class="ta">❯</span> auxo-rollback 42       <span class="tm"># roll back to snapshot 42</span></div>
<p>Rollback only replaces the system subvolume. <b>Your /home is never touched.</b> Turn snapshots on or off with <code>auxo-tweak snapshots on|off</code>.</p>

<h2 id="updating">Updating</h2>
<p><code>auxo-update</code> is a safer way to run a full update. It:</p>
<ol class="dsteps">
<li>Warns you about Arch news that needs manual steps.</li>
<li>Takes a btrfs snapshot, if snapshots are on.</li>
<li>Updates repo packages, AUR packages (paru or yay) and Flatpaks.</li>
<li>Reports .pacnew files, orphaned packages and whether you need to reboot.</li>
</ol>
<div class="code"><span class="ta">❯</span> auxo-update            <span class="tm"># interactive</span>
<span class="ta">❯</span> auxo-update -y         <span class="tm"># don't ask for confirmation</span>
<span class="ta">❯</span> auxo-update --no-news  <span class="tm"># skip the Arch news check</span></div>
<p>Plain <code>sudo pacman -Syu</code> works too, and snapshots are still taken automatically.</p>

<h2 id="gaming">Gaming</h2>
<div class="code"><span class="ta">❯</span> auxo-tweak gaming on</div>
<p>This enables multilib and installs Steam, GameMode, MangoHud and Gamescope (with their 32-bit versions), plus the right 32-bit Vulkan driver for your GPU. It also:</p>
<ul class="dl">
<li>sets <code>vm.max_map_count = 2147483642</code>, which some games need to avoid crashing</li>
<li>sets <code>kernel.split_lock_mitigate = 0</code> to avoid split-lock slowdowns</li>
<li>loads the <code>ntsync</code> module at boot, for Wine and Proton builds that support it</li>
<li>adds your user to the <code>gamemode</code> group</li>
</ul>
<p><code>auxo-tweak gaming off</code> removes the tweaks, and adding <code>--purge</code> removes the packages as well. Use <code>--no-steam</code> to skip Steam.</p>

<h2 id="fetch">auxo-fetch</h2>
<p>A fast system summary drawn in your accent colour.</p>
<div class="code"><span class="ta">❯</span> auxo-fetch              <span class="tm"># full summary with logo</span>
<span class="ta">❯</span> auxo-fetch --small      <span class="tm"># compact</span>
<span class="ta">❯</span> auxo-fetch --no-logo
<span class="ta">❯</span> auxo-fetch --json       <span class="tm"># for scripts</span></div>

<h2 id="vm">Virtual machines</h2>
<p>Use the <a class="inl" href="{DL}#vm">VM image</a>. In VirtualBox, set the graphics controller to <b>VMSVGA</b> with <b>128 MB</b> of video memory and <b>3D acceleration off</b>. After installing, the image's boot menu appears again. Choose <b>Boot existing OS</b>, or remove the image from the virtual drive.</p>
<p>Guest tools for other hypervisors can be removed with <code>auxo-tweak drivers --prune</code>.</p>

<h2 id="trouble">Troubleshooting</h2>
<div class="faq">
<details><summary>The installer starts again after installing in a VM</summary><p>The VM is still booting from the ISO. Choose <b>Boot existing OS</b> in the boot menu, or remove the ISO from the VM's optical drive.</p></details>
<details><summary>Black screen after boot in VirtualBox</summary><p>Set the graphics controller to VMSVGA and turn 3D acceleration off. Auxo 3.0 uses a text login screen that works without 3D.</p></details>
<details><summary>The system won't boot after an update</summary><p>Pick <b>Auxo snapshots</b> in the GRUB menu, boot a snapshot from before the update, then run <code>auxo-rollback</code>. See <a class="inl" href="#snapshots">Snapshots and rollback</a>.</p></details>
<details><summary>The USB stick won't boot</summary><p>Turn Secure Boot off in your firmware settings, and write the image in DD mode if you use Rufus.</p></details>
<details><summary>My NVIDIA card isn't using the NVIDIA driver</summary><p>Run <code>auxo-tweak drivers</code>. It detects the card and installs the right driver, then rebuilds the initramfs.</p></details>
</div>

<h2 id="faq">FAQ</h2>
<div class="faq">
<details><summary>Is Auxo really Arch?</summary><p>Yes. Auxo uses the official Arch repositories and pacman, follows Arch's rolling release, and works with the AUR and the Arch Wiki. It adds an installer, themes and its own tools on top.</p></details>
<details><summary>Does Secure Boot work?</summary><p>Not yet. Turn Secure Boot off before booting the USB stick.</p></details>
<details><summary>Where can I get help?</summary><p>Ask in the <a class="inl" href="{DISCORD}">Auxo Discord</a>.</p></details>
</div>

  </article>
</div>
</main>
""" + footer()


os.makedirs(OUT, exist_ok=True)
if PREVIEW:
    open(f"{OUT}/index.html", "w").write(home)
    open(f"{OUT}/download.html", "w").write(dl)
    open(f"{OUT}/docs.html", "w").write(docs)
else:
    os.makedirs(f"{OUT}/download", exist_ok=True)
    open(f"{OUT}/index.html", "w").write(home)
    open(f"{OUT}/download/index.html", "w").write(dl)
    os.makedirs(f"{OUT}/docs", exist_ok=True)
    open(f"{OUT}/docs/index.html", "w").write(docs)
print("built", OUT, len(home), len(dl), len(docs))
