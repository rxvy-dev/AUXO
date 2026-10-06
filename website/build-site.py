#!/usr/bin/env python3
"""Builds the Auxo site (v4, Void Linux): home, download, docs and the extra pages.

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
# The extra pages (Desktops, Releases, Community, Press kit) are built but not live on
# auxolinux.com, so links point at sections of pages that do exist. Set True once
# /desktops/, /releases/, /community/ and /press/ have been added to WordPress.
EXTRA_PAGES = False
if EXTRA_PAGES:
    DESKP = "desktops.html" if PREVIEW else "/desktops/"
    RELP = "releases.html" if PREVIEW else "/releases/"
    COMP = "community.html" if PREVIEW else "/community/"
    PRESSP = "press.html" if PREVIEW else "/press/"
else:
    DESKP = HOME + "#desktops"
    RELP = DL + "#releases"
    COMP = "https://discord.gg/XbQ66dH5a7"
    PRESSP = None
CHANGELOG_URL = "https://github.com/rxvy-dev/AUXO/blob/main/CHANGELOG.md"

def iso(ident, name):
    return {"url": f"https://archive.org/download/{ident}/{name}",
            "torrent": f"https://archive.org/download/{ident}/{ident}_archive.torrent",
            "details": f"https://archive.org/details/{ident}", "name": name}

# Auxo 4 (Void Linux): one image for PCs and virtual machines
VERSION = "4.0"
VOID = dict(iso("auxo-linux-VOID", "auxo-linux-2026.10.05-x86_64.iso"), size="", date="2026-10-05")
# Auxo 3 (Arch Linux): discontinued, still downloadable
ARCH = dict(iso("auxo-linux-2026.09.27-x86_64", "auxo-linux-2026.09.27-x86_64.iso"), size="2.8 GB", date="2026-09-27")
ARCH_VM = dict(iso("auxo-linux-2026.09.27-x86_64_202609", "auxo-linux-2026.09.27-x86_64.iso"), size="2.8 GB", date="2026-09-27")
ARCH_SRC = "https://github.com/rxvy-dev/AUXO/tree/arch"
STD = VOID
DISCORD = "https://discord.gg/XbQ66dH5a7"
KDE_SHOT = "https://i.ibb.co/B5s78nCV/Screenshot-20260926-233008.png"  # Auxo 3 (Arch) desktop: no longer used on the pages
OG_IMAGE = "https://auxolinux.com/og.png"  # upload website/og.png to the site root

imgs = json.load(open("assets/screenshots.json"))  # Auxo 3 (Arch) Calamares screenshots, no longer used on the pages
import base64 as _b64e
def _webp(name):
    return "data:image/webp;base64," + _b64e.b64encode(open(f"assets/installer/{name}.webp", "rb").read()).decode()
TUI = {n: _webp(n) for n in ("welcome", "desktop", "accent", "kernel", "extras", "review")}

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

/* extras grid (6 cards) + discontinued release block */
.cards5.extras6{grid-template-columns:repeat(3,1fr)}
@media(max-width:860px){.cards5.extras6{grid-template-columns:1fr}}
.cards5.extras6 .pkgs{margin-top:10px}
.legacy{margin-top:28px;border:1px dashed var(--line2);border-radius:8px;padding:28px;background:var(--card)}
.legacy .eyebrow{color:var(--dim)}
.legacy .top p{margin:10px 0 20px;color:var(--mut)}
.legacy .dlrow .btn.primary{background:var(--card2);border-color:var(--line2);color:var(--tx)}
.dtag{display:inline-block;font:600 11px var(--fm);letter-spacing:.06em;text-transform:uppercase;color:var(--dim);border:1px solid var(--line2);border-radius:5px;padding:2px 7px;margin-left:8px;vertical-align:middle}

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
.L i.tty::after{content:"Auxo Linux 4.0 (tty1)\A\A auxo login: _";white-space:pre;inset:8% auto auto 5%;background:none;font:clamp(10px,1.4vw,15px)/1.6 var(--fm);color:#cfcfd8}
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
.alertbar{background:#15130b;border-bottom:1px solid #3a3118}
.alertbar .wrap{display:grid;grid-template-columns:22px 1fr;gap:12px;padding-top:14px;padding-bottom:14px;font-size:14.5px;line-height:1.6;color:#d6d3c4}
.alertbar svg{width:20px;height:20px;margin-top:2px;fill:none;stroke:#fbbf24;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.alertbar b{color:#fde68a}.alertbar code{color:#fde68a}
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
.heroshot.tui .shotwrap{aspect-ratio:1000/640;background:#121212}
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
.qrec{font-size:13.5px;color:var(--mut);padding:10px 12px;border:1px solid color-mix(in srgb,var(--a) 35%,transparent);border-radius:7px;background:color-mix(in srgb,var(--a) 6%,transparent)}
.qrec b{color:var(--tx)}
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

/* v8 pages */
.muted{color:var(--dim);font-size:14.5px}
.subnav{border-bottom:1px solid var(--line);background:var(--bg2);position:sticky;top:72px;z-index:30}
.subnav .wrap{display:flex;gap:4px;overflow-x:auto;padding-top:8px;padding-bottom:8px}
.subnav a{padding:8px 14px;border-radius:6px;font-size:14px;font-weight:600;color:var(--mut);white-space:nowrap}
.subnav a:hover{color:var(--tx);background:var(--card)}
@media(max-width:760px){.subnav{position:static}}
.auxo section.dsec{padding:80px 0;border-bottom:1px solid var(--line);scroll-margin-top:130px}
.dsgrid{display:grid;grid-template-columns:.9fr 1.1fr;gap:56px;align-items:start}
.dsgrid>*{min-width:0}
@media(max-width:960px){.dsgrid{grid-template-columns:1fr;gap:28px}}
.dsec:nth-child(even) .dtext{order:2}
@media(max-width:960px){.dsec:nth-child(even) .dtext{order:0}}
.dtext h2{font-size:clamp(28px,3vw,38px)}
.dtext .dl{color:var(--mut);font-size:16.5px;margin-bottom:20px}
.dtext .code{margin-bottom:16px}
.note2{margin-top:14px;font-size:14px;color:var(--mut)}
.screen.mini{border:1px solid var(--line2);border-radius:8px;aspect-ratio:16/10}
.kh{font:600 12px var(--fb);letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:22px 0 10px}
.keys{display:grid;grid-template-columns:1fr 1fr;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:var(--line);gap:1px}
@media(max-width:560px){.keys{grid-template-columns:1fr}}
.keys>div{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:10px 14px;background:var(--card);font-size:14px}
.keys .kempty{background:var(--card)}
.keys em{font-style:normal;color:var(--mut);text-align:right}
kbd{display:inline-block;font:600 11.5px var(--fm);padding:3px 7px;border-radius:5px;border:1px solid var(--line2);border-bottom-width:2px;background:#12151a;color:var(--tx);margin-right:4px}
.timeline{position:relative;max-width:880px;padding-left:34px}
.timeline::before{content:"";position:absolute;left:9px;top:8px;bottom:8px;width:2px;background:linear-gradient(var(--a),var(--line) 30%)}
.tl{position:relative;margin-bottom:24px;scroll-margin-top:96px}
.tldot{position:absolute;left:-31px;top:26px;width:14px;height:14px;border-radius:50%;background:var(--bg);border:3px solid var(--a)}
.tl:not(:first-child) .tldot{border-color:var(--line2)}
.tlcard{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:24px 26px}
.tl:first-child .tlcard{border-color:color-mix(in srgb,var(--a) 45%,var(--line2))}
.tlcard header{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.tlcard h2{font-size:24px;margin:0}
.rbadge{font:600 12px var(--fm);padding:4px 9px;border-radius:5px;border:1px solid var(--line2);color:var(--mut)}
.tl:first-child .rbadge{background:var(--a);border-color:var(--a);color:#0a0a0d}
.tlt{color:var(--mut);margin:6px 0 4px}
.cgrid{display:grid;grid-template-columns:1.3fr 1fr 1fr;gap:20px}
.cgrid.two-up{grid-template-columns:1fr 1fr}
@media(max-width:960px){.cgrid,.cgrid.two-up{grid-template-columns:1fr}}
.ccard{display:flex;flex-direction:column;gap:18px;background:var(--card);border:1px solid var(--line);border-radius:10px;padding:28px;transition:border-color .2s}
.ccard:hover{border-color:var(--a)}
.ccard.big{background:linear-gradient(160deg,color-mix(in srgb,#5865f2 14%,var(--card)),var(--card))}
.ccard>div:last-child{display:flex;flex-direction:column;flex:1}
.cico{width:48px;height:48px;border-radius:10px;display:grid;place-items:center;background:#12151a;border:1px solid var(--line2);color:var(--tx)}
.cico svg{width:24px;height:24px}
.ccard h2{font-size:22px;margin:0 0 8px}
.ccard p{color:var(--mut);font-size:15px;margin-bottom:18px}
.ccard .btn,.ccard .more{margin-top:auto;align-self:flex-start}
.ccard .more{font-size:14px;font-weight:600;color:var(--a)}
.rules{display:grid;grid-template-columns:repeat(4,1fr);gap:16px}
@media(max-width:900px){.rules{grid-template-columns:1fr 1fr}}
@media(max-width:560px){.rules{grid-template-columns:1fr}}
.rules>div{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:22px}
.rules b{display:inline-grid;place-items:center;width:28px;height:28px;border-radius:6px;background:var(--a);color:#0a0a0d;font:700 14px var(--fh);margin-bottom:12px}
.rules p{color:var(--mut);font-size:15px}
.facts table{width:100%;border-collapse:collapse;font-size:14.5px;margin-top:12px}
.facts th{text-align:left;font-weight:500;color:var(--dim);padding:10px 12px 10px 0;border-top:1px solid var(--line);white-space:nowrap;vertical-align:top;width:40%}
.facts td{padding:10px 0;border-top:1px solid var(--line);color:#d2d6dd}
.facts tr:first-child th,.facts tr:first-child td{border-top:0}
.blurb{margin-top:14px;padding:14px 16px;border:1px solid var(--line);border-radius:8px;background:#12151a}
.blurb small{display:block;font:600 11px var(--fb);letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin-bottom:6px}
.blurb p{color:#d2d6dd;font-size:14.5px;user-select:all}
.logogrid{display:grid;grid-template-columns:repeat(4,1fr);gap:16px}
@media(max-width:1000px){.logogrid{grid-template-columns:1fr 1fr}}
@media(max-width:560px){.logogrid{grid-template-columns:1fr}}
.lg{border:1px solid var(--line);border-radius:10px;overflow:hidden;background:var(--card)}
.lgv{height:170px;display:grid;place-items:center;background:#161a20;border-bottom:1px solid var(--line)}
.lgv.dark{background:#0a0b0e}.lgv.light{background:#f4f5f7}
.pm{width:84px;height:84px}
.wm{display:flex;align-items:center;gap:12px;font:700 40px var(--fh);letter-spacing:-.03em;color:#ececf5}
.wm .pm{width:52px;height:52px}
.tile{display:grid;place-items:center;width:104px;height:104px;border-radius:24px;background:#0b0b10;box-shadow:inset 0 0 0 1px rgba(255,255,255,.08)}
.tile .pm{width:70px;height:70px}
.lgf{display:grid;grid-template-columns:1fr auto;gap:2px 12px;align-items:center;padding:16px}
.lgf b{font-size:15px}
.lgf span{grid-column:1;font-size:13px;color:var(--dim)}
.lgf .btn{grid-row:1/3;grid-column:2}
.subh{font-size:20px;margin:40px 0 16px}
.colors{display:grid;grid-template-columns:repeat(4,1fr);gap:16px}
@media(max-width:800px){.colors{grid-template-columns:1fr 1fr}}
.colors>div{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:16px;display:grid;gap:4px}
.colors i{height:64px;border-radius:6px;margin-bottom:8px}
.colors code{justify-self:start}
.colors span{font-size:13px;color:var(--dim)}
.pshots{display:grid;grid-template-columns:repeat(3,1fr);gap:18px}
@media(max-width:900px){.pshots{grid-template-columns:1fr 1fr}}
@media(max-width:560px){.pshots{grid-template-columns:1fr}}
.pshots figure{border:1px solid var(--line);border-radius:8px;overflow:hidden;background:var(--card)}
.pshots img{width:100%;aspect-ratio:16/10;object-fit:cover;object-position:top;background:#15181e}
.pshots figcaption{padding:12px 14px;font-size:14px;color:var(--mut)}
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
<meta property="og:image" content="{OG_IMAGE}">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="{title}">
<meta name="twitter:description" content="{desc}">
<meta name="twitter:image" content="{OG_IMAGE}">
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
<a class="brand" href="{HOME}">{mark()}auxo<small>4.0</small></a>
<nav class="links" aria-label="Main"><a href="{DL}"{cur('dl')}>Get Auxo</a><a href="{DESKP}"{cur('desktops')}>Desktops</a><a href="{DOCS}"{cur('docs')}>Docs</a><a href="{RELP}"{cur('releases')}>Releases</a><a href="{COMP}"{cur('community')}>Community</a></nav>
<div class="right"><a class="icon-link" href="{DISCORD}" aria-label="Auxo Discord">{I['discord']}</a><a class="btn primary sm" href="{DL}">{I['dl']}Get Auxo</a></div>
</div></header>
"""

def footer():
    return f"""<footer><div class="wrap">
<div class="fgrid">
  <div><a class="brand" href="{HOME}">{mark()}auxo</a><p>A rolling distro built on Void Linux. Pick your desktop, kernel, shell and colour when you install, and change them later without reinstalling.</p></div>
  <div><h4>Get Auxo</h4><ul><li><a href="{DL}#installer">Installer image</a></li><li><a href="{DL}#vmsetup">Virtual machines</a></li><li><a href="{DL}#arch">Auxo 3 (Arch)</a></li><li><a href="{DL}#usb">Make a USB stick</a></li><li><a href="{DL}#verify">Verify a download</a></li></ul></div>
  <div><h4>Explore</h4><ul><li><a href="{DESKP}">Desktops</a></li><li><a href="{HOME}#software">Software</a></li><li><a href="{RELP}">Releases</a></li><li><a href="{CHANGELOG_URL}">Changelog</a></li>{f'<li><a href="{PRESSP}">Press kit</a></li>' if PRESSP else ''}</ul></div>
  <div><h4>Docs</h4><ul><li><a href="{DOCS}#tweak">auxo-tweak</a></li><li><a href="{DOCS}#snapshots">Snapshots</a></li><li><a href="{DOCS}#nvidia">NVIDIA</a></li><li><a href="{DOCS}#gaming">Gaming</a></li><li><a href="{DOCS}#trouble">Troubleshooting</a></li></ul></div>
  <div><h4>Community</h4><ul>{f'<li><a href="{COMP}">Get involved</a></li>' if EXTRA_PAGES else ''}<li><a href="{DISCORD}">Discord</a></li><li><a href="https://www.reddit.com/r/DistroHub/">r/DistroHub</a></li><li><a href="https://github.com/rxvy-dev/AUXO">GitHub</a></li></ul></div>
</div>
<div class="fbot"><span>© 2026 Auxo Linux</span><span>Built on Void Linux. Not affiliated with the Void Linux project.</span></div>
</div></footer>
</div>
</body>
</html>
"""

def checks(items):
    return '<ul class="checks">' + "".join(f"<li>{I['check']}<span>{t}</span></li>" for t in items) + "</ul>"

def shot(src, alt, title="Auxo 4.0"):
    return f'<div class="shot rv"><div class="bar mute"><i></i><i></i><i></i><b>{title}</b></div><img src="{src}" alt="{alt}" loading="lazy"></div>'

ALERT_ICO = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3 2 21h20L12 3z"/><path d="M12 10v5M12 18h.01"/></svg>'
def alertbar():
    return (f'<div class="alertbar" role="note"><div class="wrap">{ALERT_ICO}<span>'
            f'<b>Auxo 4.0 is here, now built on Void Linux.</b> The Arch-based Auxo 3 is discontinued, '
            f'but you can <a class="inl" href="{DL}#arch">still download it</a>.</span></div></div>')

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
 <h3>Every update is a restore point</h3><p>Before each <code>auxo-update</code>, Auxo takes a btrfs snapshot, plus one every hour. If something breaks, pick an older snapshot in the GRUB menu, boot it, and keep it with <code>auxo-rollback</code>. Your /home is never touched.</p>
 <div class="snaps"><span>▸ before update · today <b>booted</b></span><span>▸ before update · yesterday</span><span>▸ before update · last week</span></div></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><rect x="3" y="6" width="18" height="12" rx="2"/><path d="M7 10h4v4H7zM15 10h2M15 14h2"/></svg></div>
 <h3>GPU drivers, detected</h3><p>The installer checks your graphics card, and <code>auxo-tweak nvidia on</code> picks the right NVIDIA driver for your card.</p>
 <div class="gpus"><span>NVIDIA · nvidia</span><span>AMD · Vulkan</span><span>Intel · Vulkan</span></div></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M6 11h4M8 9v4"/><path d="M7 5h10a5 5 0 0 1 5 5v2a5 5 0 0 1-9 3h-2a5 5 0 0 1-9-3v-2a5 5 0 0 1 5-5z"/></svg></div>
 <h3>One-command gaming</h3><p>Steam with its 32-bit libraries, GameMode, MangoHud and gamescope, plus the kernel tweaks games expect.</p>
 <div class="mini"><span class="ta">❯</span> auxo-tweak gaming on</div></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 3v18M3 12h18"/></svg></div>
 <h3>Dual boot</h3><p>Install on partitions you already made, alongside Windows or another Linux, and reuse your EFI partition.</p></div>
<div class="s2 rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1"/></svg></div>
 <h3>Installs offline</h3><p>No internet still gets you a complete Plasma desktop. Other desktops download what they need.</p></div>
<div class="s3 wide rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><path d="M4 17l6-6-6-6M12 19h8"/></svg></div>
 <h3>One command to update</h3><p><code>auxo-update</code> takes a snapshot, updates xbps itself and then every package and Flatpak, and tells you about new config files and whether to reboot.</p>
 <div class="mini"><span class="ta">❯</span> auxo-update
<span class="tm">:: snapshot · before update</span>
<span class="tm">:: xbps · flatpak</span>
<span class="tg">✓</span> 42 packages updated</div></div>
<div class="s3 wide rv"><div class="ico"><svg class="i" viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" rx="3"/><path d="M9 9h6v6H9z"/></svg></div>
 <h3>Sensible defaults</h3><p>zram swap, a git-aware prompt in your accent, and one-line commands for a firewall, private DNS and sched-ext CPU schedulers.</p>
 <div class="mini"><span class="ta">❯</span> auxo-fetch --json | jq .accent
<span class="tm">"</span><span class="accname"></span><span class="tm">"</span></div></div>
"""

compare_rows = [
    ("Installer", "Text installer with a review screen", "void-installer, then set up by hand"),
    ("Desktop at first boot", "Ready, themed, configured", "You install and configure it"),
    ("GPU drivers", "Detected and installed", "You pick and install them"),
    ("Snapshots", "Before every update, bootable from GRUB", "Set up yourself if you want them"),
    ("Change desktop later", "One command", "Install, configure, enable services"),
    ("Packages", "Void repos and Flatpak", "Void repos and Flatpak"),
]
compare_html = "".join(f'<tr><td>{a}</td><td class="us">{I["check"]}{b}</td><td>{c}</td></tr>' for a, b, c in compare_rows)

faqs = [
    ("Is Auxo really Void?", "Yes. Auxo uses the official Void Linux repositories, xbps and runit, and it follows Void's rolling release. Auxo adds an installer, themes and its own tools on top, and the Void docs apply as normal."),
    ("What happened to the Arch version?", "Auxo 3 was based on Arch Linux. It's discontinued: there won't be new Auxo 3 images or tool updates. Its last images are still on the <a class=\"inl\" href=\"" + DL + "#arch\">download page</a>, and systems already installed from it keep getting Arch package updates through pacman."),
    ("Can I switch desktops without breaking things?", "That's the point of <code>auxo-tweak</code>. It installs the new desktop, applies the Auxo setup, keeps your old config files as <code>*.auxo-bak</code> and points the login screen at the new desktop. If anything goes wrong, boot an earlier snapshot from GRUB."),
    ("Does it work in VirtualBox or other VMs?", "Yes, from the same image. Set VirtualBox graphics to VMSVGA with 3D acceleration off, and remove the image from the virtual drive after installing."),
    ("Will it work with my NVIDIA card?", "Run <code>auxo-tweak nvidia on</code>. It picks the right driver for your card from Void's nonfree repository: <code>nvidia</code> for Turing and newer, <code>nvidia580</code> for Maxwell to Volta, <code>nvidia470</code> for Kepler."),
    ("Is there an AUR?", "No, that's an Arch thing. Void's repositories are large, Flatpak is one command away (<code>auxo-tweak flatpak on</code>), and Void's own xbps-src builds anything else."),
    ("Does Secure Boot work?", "Not yet. Turn Secure Boot off in your firmware settings before booting the USB stick."),
    ("Is it free?", "Yes. Auxo is free to download and use."),
]
faq_html = "".join(f"<details><summary>{q}</summary><p>{a}</p></details>" for q, a in faqs)

LOGOS = open("assets/logos.html").read()
TOPO = '<svg class="topo" viewBox="0 0 520 440" aria-hidden="true"><g transform="translate(10.4 -26.8) scale(3.9)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="0.72" opacity="0.1"/></g><g transform="translate(42.4 29.2) scale(3.4)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="0.82" opacity="0.14"/></g><g transform="translate(71.2 79.6) scale(2.95)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="0.95" opacity="0.18"/></g><g transform="translate(100.0 130.0) scale(2.5)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="1.12" opacity="0.24"/></g><g transform="translate(125.6 174.8) scale(2.1)"><path class="c" d="M16 112 L64 22 L112 112" stroke-width="1.33" opacity="0.3"/></g><g transform="translate(157 118) scale(1.6)">{MARK_PATHS}</g></svg>'.replace("{MARK_PATHS}", MARK_PATHS)
import re as _re
d_layers_k = d_layers  # only real Auxo 4 screenshots on the site; no 4.0 desktop screenshot yet

IC = {
 "time": '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>',
 "base": '<svg viewBox="0 0 24 24"><path d="M12 3L4 21h3.5L12 11l4.5 10H20z"/></svg>',
 "swap": '<svg viewBox="0 0 24 24"><path d="M7 7h11l-3-3M17 17H6l3 3"/></svg>',
 "undo": '<svg viewBox="0 0 24 24"><path d="M3 12a9 9 0 1 0 3-6.7"/><path d="M3 4v4h4"/></svg>',
 "people": '<svg viewBox="0 0 24 24"><circle cx="9" cy="8" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0"/><circle cx="17" cy="9" r="2.5"/><path d="M16 14.5a5 5 0 0 1 5.5 5"/></svg>',
 "metal": '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/></svg>',
 "usb": '<svg viewBox="0 0 24 24"><rect x="7" y="9" width="10" height="13" rx="2"/><path d="M9 9V3h6v6M10.5 5.5h.01M13.5 5.5h.01"/></svg>',
 "vm": '<svg viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="14" rx="2"/><rect x="6" y="8" width="8" height="6" rx="1"/><path d="M8 21h8"/></svg>',
 "dual": '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="8" height="16" rx="1.5"/><rect x="13" y="4" width="8" height="16" rx="1.5"/></svg>',
}


EXTRAS_CARDS = [
 ("snap", "Snapshots", "Bootable btrfs snapshots before every update, with snapper and grub-btrfs.", "on by default"),
 ("zram", "zram swap", "Compressed swap in RAM, so the system stays responsive when memory is tight.", "on by default"),
 ("gpu", "GPU drivers", "The right driver for your NVIDIA, AMD or Intel graphics.", "on by default"),
 ("fw", "Firewall", "ufw, set to block incoming connections and allow outgoing.", "optional"),
 ("flat", "Flatpak", "Flatpak with Flathub, for apps that aren't in Void's repositories.", "optional · needs internet"),
 ("game", "Gaming", "Steam, GameMode, MangoHud and gamescope, with Void's nonfree and multilib repos.", "optional · needs internet"),
]
extras_html = "".join(f'<div class="rv"><h3>{t}</h3><p>{d}</p><p class="pkgs"><span>{tag}</span></p></div>' for k, t, d, tag in EXTRAS_CARDS)

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

HERO_SHOT = (f'<figure class="heroshot tui" aria-label="The Auxo Linux 4.0 installer">'
             f'<div class="bar mute"><i></i><i></i><i></i><b>Auxo Linux 4.0 · installer</b></div>'
             f'<div class="shotwrap"><img src="{TUI["welcome"]}" alt="The welcome page of the Auxo Linux 4.0 text installer" loading="eager"></div></figure>')

home = head("Auxo Linux — Void Linux, set up the way you want it",
            "Auxo Linux is a rolling distro built on Void Linux. Pick your desktop, kernel, shell and accent colour in a fast text installer, and change any of them later with one command.",
            "https://auxolinux.com/") + nav("home") + f"""
<main id="main">{alertbar()}
<section class="khero v7">{RIDGE.replace('class="ridge"','class="hridge"')}<div class="wrap hgrid">
  <div>
    <a class="badge rise" href="{DL}#releases"><b>.</b>Auxo Linux 4.0: now built on Void Linux →</a>
    <h1 class="rise r1">Void Linux, set up the way you want it.</h1>
    <p class="lede rise r2">An open-source distribution built on Void Linux, with a fast, keyboard-driven installer. Pick your desktop, kernel, shell and accent colour, then change any of them later with one command. No systemd, no reinstall.</p>
    <div class="cta rise r3">
      <a class="btn primary lg" href="{DL}">{I['dl']}Download Auxo 4.0</a>
      <a class="btn lg" href="{DOCS}">Read the docs</a>
    </div>
    <p class="meta-line rise r4"><span>{I['check']}Free and open source</span><span>{I['check']}runit, no systemd</span><span>{I['check']}xbps and Flatpak</span><span>{I['check']}UEFI and BIOS</span></p>
    <p class="relline rise r4">Latest release <b>4.0</b> · {VOID['date']} · <a class="inl" href="{DL}#releases">Release notes</a> · <a class="inl" href="{DL}#arch">Looking for the Arch version?</a></p>
  </div>
  <div class="rise r2">{HERO_SHOT}</div>
</div></section>

<div class="strip"><div class="wrap">
  <div><b>8</b><span>desktops, three pre-riced</span></div>
  <div><b>7</b><span>accent colours</span></div>
  <div><b>3</b><span>kernels to choose from</span></div>
  <div><b>1</b><span>command to change it all</span></div>
</div></div>

<section id="features"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">01</span> Why Auxo</p><h2>Void, without the evening of setup</h2></div><p>Everything you'd normally configure by hand after installing Void, done during install and still yours to change.</p></div>
  <div class="cards5">
    <div class="rv">{IC['time']}<h3>Ready in minutes</h3><p>A guided text installer sets up your desktop, drivers, snapshots and dual boot in one go.</p></div>
    <div class="rv">{IC['base']}<h3>Still pure Void</h3><p>Official Void repositories, xbps, runit and the Void docs all work as normal.</p></div>
    <div class="rv">{IC['swap']}<h3>Change anything later</h3><p><code>auxo-tweak</code> swaps your desktop, kernel, shell or accent colour without reinstalling.</p></div>
    <div class="rv">{IC['undo']}<h3>Undo bad updates</h3><p>Every <code>auxo-update</code> takes a btrfs snapshot you can boot from GRUB and roll back to.</p></div>
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
    <div class="rv"><span class="num">1</span><h3>Install</h3><p>A keyboard-driven text installer. Pick your desktop, accent, kernel, shell and extras. It shows you a summary and changes nothing until you press Install.</p>
      <div class="term"><pre><span class="tm"># in the live session</span>
<span class="ta">❯</span> sudo auxo-installer
<span class="tg">✓</span> Auxo is installed</pre></div></div>
    <div class="rv"><span class="num">2</span><h3>Change anything</h3><p><code>auxo-tweak</code> runs the same commands as the installer, so a change later gives the same result as a fresh install.</p>
      <div class="term"><pre><span class="ta">❯</span> auxo-tweak desktop hyprland
<span class="ta">❯</span> auxo-tweak kernel linux-lts
<span class="ta">❯</span> auxo-tweak accent <span class="accname"></span></pre></div></div>
    <div class="rv"><span class="num">3</span><h3>Undo mistakes</h3><p>A snapshot is taken before every update. Boot one from GRUB, then make it permanent.</p>
      <div class="term"><pre><span class="ta">❯</span> auxo-update
<span class="tm">:: snapshot · xbps · flatpak</span>
<span class="ta">❯</span> auxo-rollback --list</pre></div></div>
  </div>
</div></section>

<section class="alt" id="software"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">04</span> Extras</p><h2>The extras, one checkbox away</h2></div><p>These are the installer's real Extras. Tick what you want during setup, or turn any of them on later with <code>auxo-tweak</code>. Everything else is an <code>xbps-install</code> away.</p></div>
  <div class="cards5 extras6">{extras_html}</div>
</div></section>

<section id="tour"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">05</span> Installer</p><h2>A fast, keyboard-driven install</h2></div><p>Real screenshots of the Auxo 4.0 installer. Arrow keys, type to search, Enter to pick, Esc to go back.</p></div>
  <div class="shots3">
    <figure class="rv"><img src="{TUI['desktop']}" alt="Choosing a desktop in the Auxo installer" loading="eager"><figcaption><b>Pick your desktop</b><span>Plasma, GNOME, Xfce, Cinnamon, or a ready-made Hyprland, Sway or i3 setup.</span></figcaption></figure>
    <figure class="rv"><img src="{TUI['accent']}" alt="Choosing an accent colour in the Auxo installer" loading="eager"><figcaption><b>Choose an accent</b><span>One colour for the boot menu, login screen, prompt, terminal and bars.</span></figcaption></figure>
    <figure class="rv"><img src="{TUI['review']}" alt="The Auxo installer's review screen" loading="eager"><figcaption><b>Check, then install</b><span>A summary of every choice. Nothing on your disk changes until you press Install.</span></figcaption></figure>
  </div>
  <div class="sec-foot picker-row"><span class="hint">Try an accent on this page:</span><div class="swatches" role="radiogroup" aria-label="Accent colour">{swatches}</div></div>
</div></section>

<section class="alt" id="everywhere"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">06</span> Platforms</p><h2>Auxo everywhere</h2></div><p>Install it on your PC, run it from a USB stick, or try it in a virtual machine. It's all one image.</p></div>
  <div class="every">
    <a class="rv" href="{DL}#installer">{IC['metal']}<h3>Bare metal</h3><p>Install on your PC or laptop with full access to your hardware.</p><span class="more">Get the image →</span></a>
    <a class="rv" href="{DL}#usb">{IC['usb']}<h3>Live USB</h3><p>Boot a full KDE Plasma desktop without touching your drive.</p><span class="more">Make a USB stick →</span></a>
    <a class="rv" href="{DL}#vmsetup">{IC['vm']}<h3>Virtual machines</h3><p>VirtualBox, QEMU/KVM and VMware, from the same image.</p><span class="more">VM setup →</span></a>
    <a class="rv" href="{DL}#installer">{IC['dual']}<h3>Dual boot</h3><p>Install alongside Windows or another Linux and pick at startup.</p><span class="more">Get the image →</span></a>
  </div>
</div></section>

<section id="news"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">07</span> News</p><h2>Latest news</h2></div><p>Releases and announcements.</p></div>
  <div class="news">
    <a class="rv" href="{DL}"><time>{VOID['date']}</time><h3>Auxo Linux 4.0: now on Void Linux</h3><p>A new base with runit and xbps, a fast text installer, NVIDIA and sched-ext commands, and bootable snapshots before every update.</p><span class="more">Download 4.0 →</span></a>
    <a class="rv" href="{DL}#arch"><time>{VOID['date']}</time><h3>Auxo 3 (Arch) is discontinued</h3><p>The Arch-based Auxo 3 won't get new images or tool updates. Its final images stay available for download.</p><span class="more">Auxo 3 downloads →</span></a>
    <a class="rv" href="{DOCS}"><time>{VOID['date']}</time><h3>The docs, updated for 4.0</h3><p>Every auxo-tweak command on Void: desktops, NVIDIA, snapshots and rollback, gaming and troubleshooting.</p><span class="more">Read the docs →</span></a>
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
  <div class="dlinfo"><b>{title}</b><em>{desc}</em><span>{" · ".join(x for x in (f['name'], f['size'], f['date']) if x)}</span></div>
  <div class="dlacts"><a class="btn primary" href="{f['url']}">{I['dl']}Download</a><a class="btn" href="{f['torrent']}">{I['mag']}Torrent</a><a class="btn" href="{f['details']}" title="MD5 and SHA-1 are listed on the Internet Archive page">{I['shield']}Checksums</a></div>
</div>"""

def radios(grp, keys):
    return "".join(f'<input class="vh" type="radio" name="t-{grp}" id="t-{grp}-{k}"{" checked" if i == 0 else ""}>' for i, k in enumerate(keys))

N = VOID["name"]
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

VMN = VOID["name"]
vm_tabs = f"""{radios('vm', TABSETS['vm'])}
<div class="tabs rv">
 <div class="tablist" role="tablist"><label for="t-vm-vbox">{I['vbox']}VirtualBox</label><label for="t-vm-qemu">{I['tux']}QEMU / KVM</label><label for="t-vm-vmw">{I['vbox']}VMware</label></div>
 <div class="panel p-vm-vbox"><ol class="steps">
  <li><span>New VM: type <b>Linux</b>, version <b>Other Linux (64-bit)</b>. Give it at least <b>4 GB</b> RAM, 2 CPUs and a <b>25 GB</b> disk. Tick <b>Enable EFI</b> if you want a UEFI install.</span></li>
  <li><span>Settings → Display: graphics controller <b>VMSVGA</b>, video memory <b>128 MB</b>, <b>3D acceleration off</b>.</span></li>
  <li><span>Attach the Auxo image as the optical drive, start the VM and install as normal.</span></li>
  <li><span>When the installer says <b>Auxo is installed</b>, power off, remove the image from the optical drive, and start the VM again.</span></li>
 </ol></div>
 <div class="panel p-vm-qemu"><ol class="steps">
  <li><span>With <b>virt-manager</b>: create a VM from the image, choose <b>Generic Linux</b> as the OS, 4 GB RAM and a 25 GB disk. Video: <b>Virtio</b>.</span></li>
  <li><span>Or from a terminal:<div class="code"><span class="tm">$ </span>qemu-img create -f qcow2 auxo.qcow2 25G
<span class="tm">$ </span>qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 4G \\
    -drive file=auxo.qcow2,if=virtio -cdrom {VMN} \\
    -boot d -vga virtio</div></span></li>
  <li><span>After installing, start it again without <code>-cdrom</code> and <code>-boot d</code>.</span></li>
 </ol></div>
 <div class="panel p-vm-vmw"><ol class="steps">
  <li><span>New VM: guest OS <b>Linux</b>, version <b>Other Linux 6.x kernel 64-bit</b>. 4 GB RAM, 2 CPUs, 25 GB disk.</span></li>
  <li><span>Point the CD/DVD drive at the Auxo image and install as normal.</span></li>
  <li><span>After installing, disconnect the CD/DVD drive and restart.</span></li>
 </ol></div>
</div>
<div class="note">{I['info']}<span><b>Using a VM?</b> Remove the image from the virtual drive after installing, or the VM boots the live session again.</span></div>"""

dl = head("Get Auxo Linux — Download",
          "Download Auxo Linux 4.0, built on Void Linux: one image for PCs, live USB and virtual machines. The discontinued Arch-based Auxo 3 is still available.",
          "https://auxolinux.com/download/") + nav("dl") + f"""
<main id="main">{alertbar()}
<div class="phead"><div class="wrap pgrid">
  <div>
    <p class="eyebrow rise">Auxo Linux 4.0</p>
    <h1 class="rise r1">Get Auxo Linux</h1>
    <p class="rise r2">Four steps from download to desktop. Most installs take about ten minutes.</p>
    <div class="chips rise r3"><span><b>4.0</b> latest</span><span><b>Void Linux</b> base</span><span><b>x86_64</b></span><span><b>UEFI</b> + BIOS</span><span>Hosted on the <b>Internet Archive</b></span></div>
  </div>
  <div class="qd rise r2">
    <div class="qtop">{mark()}<div><b>Auxo Linux 4.0</b><span>x86_64 · Void Linux · {VOID['date']}</span></div></div>
    <a class="btn primary lg" href="{VOID['url']}">{I['dl']}Download Auxo 4.0</a>
    <p class="qrec">One image for PCs, live USB and virtual machines.</p>
    <p class="qlinks"><a href="{VOID['torrent']}">Torrent</a><a href="{VOID['details']}">Checksums</a><a href="#arch">Auxo 3 (Arch)</a></p>
  </div>
</div></div>

<nav class="stepsbar" aria-label="Download steps"><div class="wrap">
  <a href="#choose"><b>1</b>Choose a platform</a><a href="#get"><b>2</b>Download</a><a href="#usb"><b>3</b>Prepare USB or VM</a><a href="#step4"><b>4</b>Verify and install</a>
</div></nav>

<section id="choose" style="padding-top:72px;padding-bottom:48px"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow"><span class="n">01</span> Step one</p><h2>Choose your platform</h2></div><p>It's the same image everywhere. Pick where you want to run it.</p></div>
  <div class="platforms">
    <a class="plat rec rv" href="#usb">
      <div class="art"><span class="rtag">Recommended</span>{ART_PC}</div>
      <div class="body"><h2>Install on a PC</h2><p class="sub">Install Auxo on your PC or laptop from a USB stick.</p>
        <ul class="pc"><li class="p">Direct access to your hardware and GPU</li><li class="p">Dual boot with Windows or Linux</li><li class="p">Full speed, full desktop</li><li class="m">Needs a USB stick and a reboot</li></ul>
        <div class="go"><span class="btn primary">{I['dl']}Make a USB stick</span></div></div>
    </a>
    <a class="plat rv" href="#vmsetup">
      <div class="art">{ART_VM}</div>
      <div class="body"><h2>Virtual machines</h2><p class="sub">Run Auxo inside VirtualBox, QEMU/KVM or VMware.</p>
        <ul class="pc"><li class="p">Try it without touching your PC</li><li class="p">Works on Windows, macOS and Linux hosts</li><li class="p">Same image as for PCs</li><li class="m">Slower graphics than real hardware</li></ul>
        <div class="go"><span class="btn">VM setup</span></div></div>
    </a>
    <a class="plat rv" href="#usb">
      <div class="art">{ART_USB}</div>
      <div class="body"><h2>Live boot</h2><p class="sub">Run Auxo from a USB stick without installing.</p>
        <ul class="pc"><li class="p">Nothing changes on your drive</li><li class="p">Full KDE Plasma desktop to try</li><li class="p">Install from it whenever you're ready</li><li class="m">Changes are lost when you shut down</li></ul>
        <div class="go"><span class="btn">Make a USB stick</span></div></div>
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
    <div class="top"><div><p class="eyebrow">Auxo Linux 4.0 · Void Linux</p><h2>PCs, live USB and virtual machines</h2><p>Write it to a USB stick or attach it to a VM, boot it, and try Auxo live or install it.</p></div><a class="doclink" href="#usb">Make a USB stick →</a></div>
    <div class="dltable">{dlrow(VOID, "Auxo Linux 4.0", "Live KDE Plasma session with the Auxo text installer")}</div>
  </div>
  <div class="legacy rv" id="arch">
    <div class="top"><div><p class="eyebrow">Discontinued</p><h2>Auxo 3 (Arch Linux) <span class="dtag">No longer updated</span></h2>
      <p>Auxo 3 was based on Arch Linux and used the Calamares installer. It's no longer developed: there won't be new Auxo 3 images or updates to its Auxo tools. Systems you install from it still get Arch package updates through pacman. <b>For new installs, use Auxo 4.</b></p></div></div>
    <div class="dltable">{dlrow(ARCH, "Auxo Linux 3.0 (Arch) — Installer", "Final Arch-based image: live KDE Plasma with the graphical installer")}{dlrow(ARCH_VM, "Auxo Linux 3.0 (Arch) — VM", "The same, built for virtual machines")}</div>
    <p class="subtle" style="margin-top:14px">The Auxo 3 source code is kept on GitHub on the <a class="inl" href="{ARCH_SRC}">arch</a> branch.</p>
  </div>
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
  <div class="sec-head"><div><p class="eyebrow"><span class="n">04</span> Step four</p><h2>Verify and install</h2></div><p>Check the download, boot it, and start the installer from the welcome app.</p></div>
  <div class="two">
    <div class="card rv" id="install"><h3>Install Auxo</h3><p>Boot the USB stick or VM, then:</p>
      <ol class="steps">
        <li><span>Choose <b>Auxo Linux</b> in the boot menu. The live KDE Plasma desktop starts.</span></li>
        <li><span>Click <b>Install Auxo Linux</b> in the welcome app, or run <code>sudo auxo-installer</code> in a terminal. For Wi-Fi, press <b>N</b> on the installer's first page.</span></li>
        <li><span>Use the arrow keys, type to search, <b>Enter</b> to pick and <b>Esc</b> to go back. Choose your keyboard, language, time zone and disk: erase a disk, or use partitions you made to dual boot.</span></li>
        <li><span>Choose your desktop, accent colour, kernel and shell, create your account, and tick any extras.</span></li>
        <li><span>Check the summary and press <b>Install</b>. Nothing on your disk changes before this step.</span></li>
        <li><span>When it says <b>Auxo is installed</b>, restart and remove the USB stick.</span></li>
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
    <div class="card rv"><h3>What's new in 4.0</h3><p>Auxo moves from Arch Linux to Void Linux.</p>
      <ul class="rel">
        <li><b class="new">New</b><span>Built on Void Linux, with runit and xbps instead of systemd and pacman</span></li>
        <li><b class="new">New</b><span>A fast, keyboard-driven text installer with a review screen, for UEFI and BIOS, erase or dual boot</span></li>
        <li><b class="new">New</b><span><code>auxo-tweak nvidia</code> picks the right NVIDIA driver; <code>scheduler</code>, <code>power</code>, <code>firewall</code>, <code>dns</code> and <code>cleanup</code> commands</span></li>
        <li><b class="new">New</b><span>Kernels: Void's <code>linux</code>, <code>linux-lts</code> and <code>linux-mainline</code></span></li>
        <li><b class="new">New</b><span>Snapshots before every <code>auxo-update</code> and every hour, bootable from GRUB</span></li>
        <li><b class="fix">Changed</b><span>No boot splash: Auxo boots straight to the login screen</span></li>
      </ul></div>
    <div class="card rv"><h3>All releases</h3><p>Every image is kept on the Internet Archive.</p>
      <div class="rtable">
        <a href="{VOID['details']}"><b>4.0</b><span>{VOID['date']}</span><em>Latest · Void Linux</em></a>
        <a href="{ARCH['details']}"><b>3.0</b><span>2026-09-27</span><em>Arch · discontinued</em></a>
        <a href="https://archive.org/details/auxo-linux-2026.09.26-x86_64"><b>3.0</b><span>2026-09-26</span><em>Arch · first 3.0 build</em></a>
        <a href="https://archive.org/details/auxo-linux-2026.07.19-0048-x86_64"><b>2.0</b><span>2026-07-19</span><em>Arch</em></a>
        <a href="https://archive.org/details/auxo-linux-2026.07.09-x86_64"><b>1.0</b><span>2026-07-09</span><em>Arch</em></a>
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
 ("auxo-tweak accent [NAME]", "Set the accent colour for GRUB, the login screen, prompt, terminals and bars. With no name, lists the colours."),
 ("auxo-tweak desktop NAME", "Install and switch to <code>plasma</code>, <code>gnome</code>, <code>xfce</code>, <code>cinnamon</code>, <code>hyprland</code>, <code>sway</code>, <code>i3</code> or <code>none</code>. Add <code>--replace</code> to remove the live Plasma desktop."),
 ("auxo-tweak rice NAME", "Re-apply the Auxo dotfiles for <code>hyprland</code>, <code>sway</code> or <code>i3</code> (existing files are kept as <code>*.auxo-bak</code>). <code>kde</code> applies the Auxo global theme and puts the Auxo panel layout back."),
 ("auxo-tweak kernel NAME", "Switch to <code>linux</code>, <code>linux-lts</code> or <code>linux-mainline</code>. It stays the default boot entry after updates."),
 ("auxo-tweak shell NAME", "Change your shell to <code>zsh</code>, <code>fish</code> or <code>bash</code>."),
 ("auxo-tweak drivers", "Detect your GPU and install the right driver. <code>--prune</code> removes drivers and VM guest tools for hardware that isn't there."),
 ("auxo-tweak nvidia status|on|off|prime", "The right NVIDIA driver for your card, back to nouveau with <code>off</code>, and <code>prime-run</code> for laptops with <code>prime</code>."),
 ("auxo-tweak scheduler list|status|off|NAME", "sched-ext CPU schedulers: <code>lavd</code>, <code>bpfland</code>, <code>flash</code>, <code>cosmos</code>, <code>rusty</code>. Add <code>--mode gaming</code>, <code>lowlatency</code> or <code>powersave</code>."),
 ("auxo-tweak power balanced|performance|power-saver", "Set the power profile."),
 ("auxo-tweak firewall on|off|status", "ufw: block incoming connections, allow outgoing."),
 ("auxo-tweak dns cloudflare|quad9|google|auto", "DNS servers for every NetworkManager connection. <code>auto</code> goes back to your network's DNS."),
 ("auxo-tweak snapshots on|off|status", "Bootable btrfs snapshots with snapper and grub-btrfs."),
 ("auxo-tweak gaming on|off|status", "Steam, GameMode, MangoHud, gamescope and gaming tweaks. Options: <code>--no-steam</code>, <code>--user NAME</code>, and <code>--purge</code> with <code>off</code>."),
 ("auxo-tweak flatpak on|status", "Install Flatpak and add Flathub."),
 ("auxo-tweak cleanup", "Clear the package cache, remove orphaned packages and old kernels."),
 ("auxo-tweak mirrors", "Pick a faster Void mirror. <code>--url</code> sets one directly, <code>--default</code> goes back."),
 ("auxo-tweak zram on|off", "Compressed swap in RAM."),
 ("auxo-tweak repo nonfree|multilib", "Enable Void's extra repositories (needed for NVIDIA, Steam and Wine)."),
 ("auxo-tweak service NAME on|off", "Turn <code>bluetooth</code>, <code>cups</code>, <code>sshd</code> or <code>power-profiles</code> on or off."),
]
tweak_table = "".join(cmdrow(c, d) for c, d in TWEAK)
acc_list = "".join(f'<span class="accchip"><i style="background:linear-gradient(135deg,{a},{b})"></i>{n}</span>' for n, a, b in ACCENTS)

DOC_NAV = [("start", "Getting started"), ("tweak", "auxo-tweak"), ("desktops", "Desktops and rices"), ("accents", "Accent colours"),
           ("snapshots", "Snapshots and rollback"), ("updating", "Updating"), ("packages", "Packages and services"),
           ("nvidia", "NVIDIA"), ("performance", "Performance"), ("gaming", "Gaming"), ("motd", "auxo-motd"), ("fetch", "auxo-fetch"),
           ("vm", "Virtual machines"), ("trouble", "Troubleshooting"), ("faq", "FAQ")]
doc_nav = "".join(f'<a href="#{k}">{t}</a>' for k, t in DOC_NAV)

docs = head("Auxo Linux documentation",
            "Documentation for Auxo Linux 4.0 on Void Linux: every auxo-tweak command, desktops, accent colours, snapshots and rollback, updating, NVIDIA, gaming and troubleshooting.",
            "https://auxolinux.com/docs/") + nav("docs") + f"""
<main id="main">
<div class="phead"><div class="wrap">
  <p class="eyebrow rise">Documentation</p>
  <h1 class="rise r1">Auxo Linux docs</h1>
  <p class="rise r2">Everything you can do after installing, from switching desktops to rolling back a bad update. Auxo is Void underneath, so the <a class="inl" href="https://docs.voidlinux.org/">Void Linux Handbook</a> applies too. These docs are for Auxo 4. The Arch-based Auxo 3 is <a class="inl" href="{DL}#arch">discontinued</a>.</p>
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
<li><b>KDE Plasma 6</b>: the default desktop and the live session, with the Auxo global theme. It installs even without internet.</li>
<li><b>Hyprland</b> (Auxo rice): waybar, wofi, mako, hyprlock and kitty. Void doesn't package Hyprland, so it comes from the <a class="inl" href="https://github.com/Makrennel/hyprland-void">hyprland-void</a> community repository, which Auxo adds for you.</li>
<li><b>Sway</b> (Auxo rice): waybar, foot and mako.</li>
<li><b>i3</b> (Auxo rice): polybar, rofi and picom.</li>
<li><b>GNOME</b>, <b>Xfce</b> and <b>Cinnamon</b>: the standard desktops with your accent applied.</li>
<li><b>none</b>: a text login only. Add a desktop later.</li>
</ul>
<p>Most desktops start from a fast text login screen (greetd) in your accent colour. GNOME uses its own login screen, GDM. Broke your tiling setup? <code>auxo-tweak rice hyprland</code> (or <code>sway</code>, <code>i3</code>) puts the Auxo dotfiles back.</p>

<h2 id="accents">Accent colours</h2>
<p>One setting colours the GRUB menu, the login screen, your prompt, terminals and bars, and changes the wallpaper straight away.</p>
<div class="accs">{acc_list}</div>
<div class="code"><span class="ta">❯</span> auxo-tweak accent rose</div>

<h2 id="snapshots">Snapshots and rollback</h2>
<p>On btrfs installs with <b>Snapshots</b> ticked, snapper takes a snapshot before every <code>auxo-update</code> and once an hour, and grub-btrfs adds them to the boot menu. The first snapshot is taken at the end of the install.</p>
<p><b>If an update breaks something:</b></p>
<ol class="dsteps">
<li>Restart and pick the snapshots entry in the GRUB menu.</li>
<li>Boot a snapshot from before the problem. It starts read-only.</li>
<li>Run <code>auxo-rollback</code> and choose that snapshot to make it your system again.</li>
</ol>
<div class="code"><span class="ta">❯</span> auxo-rollback --list   <span class="tm"># list snapshots</span>
<span class="ta">❯</span> auxo-rollback 42       <span class="tm"># roll back to snapshot 42</span></div>
<p>Rollback only replaces the system subvolume. <b>Your /home is never touched.</b> Turn snapshots on or off with <code>auxo-tweak snapshots on|off</code>.</p>

<h2 id="updating">Updating</h2>
<p><code>auxo-update</code> is the safe way to run a full update. It:</p>
<ol class="dsteps">
<li>Takes a btrfs snapshot, if snapshots are on.</li>
<li>Updates xbps itself first, then every package.</li>
<li>Updates your Flatpak apps.</li>
<li>Reports new config files, orphaned packages and whether you need to reboot.</li>
</ol>
<div class="code"><span class="ta">❯</span> auxo-update            <span class="tm"># interactive</span>
<span class="ta">❯</span> auxo-update -y         <span class="tm"># don't ask for confirmation</span></div>
<p>Plain <code>sudo xbps-install -Su</code> works too, but only <code>auxo-update</code> takes a snapshot first.</p>

<h2 id="packages">Packages and services</h2>
<p>Auxo uses Void's package manager, xbps, and Void's init system, runit. There's no systemd.</p>
<div class="code"><span class="ta">❯</span> xbps-query -Rs firefox          <span class="tm"># search</span>
<span class="ta">❯</span> sudo xbps-install firefox       <span class="tm"># install</span>
<span class="ta">❯</span> sudo xbps-remove -R firefox     <span class="tm"># remove, with unneeded dependencies</span>
<span class="ta">❯</span> sudo sv status NetworkManager   <span class="tm"># check a service</span></div>
<p>Services live in <code>/etc/sv</code> and are turned on by linking them into <code>/var/service</code>. <code>auxo-tweak service NAME on|off</code> does that for common ones. Apps that aren't in Void's repositories are usually on Flathub: run <code>auxo-tweak flatpak on</code> first.</p>

<h2 id="nvidia">NVIDIA</h2>
<div class="code"><span class="ta">❯</span> auxo-tweak nvidia on</div>
<p>This enables Void's nonfree repository and picks the driver for your card: <code>nvidia</code> for Turing (GTX 16 / RTX 20) and newer, <code>nvidia580</code> for Maxwell to Volta, and <code>nvidia470</code> for Kepler. It's built with DKMS for every installed kernel. On laptops with two GPUs, <code>auxo-tweak nvidia prime</code> adds <code>prime-run</code>. <code>auxo-tweak nvidia off</code> goes back to the open-source nouveau driver.</p>

<h2 id="performance">Performance</h2>
<p>sched-ext CPU schedulers change how the kernel shares CPU time between programs, without rebooting:</p>
<div class="code"><span class="ta">❯</span> auxo-tweak scheduler list
<span class="ta">❯</span> auxo-tweak scheduler lavd --mode gaming
<span class="ta">❯</span> auxo-tweak scheduler off</div>
<p>Pick a power profile with <code>auxo-tweak power balanced|performance|power-saver</code>, and turn on compressed RAM swap with <code>auxo-tweak zram on</code>.</p>

<h2 id="gaming">Gaming</h2>
<div class="code"><span class="ta">❯</span> auxo-tweak gaming on</div>
<p>This enables Void's nonfree and multilib repositories and installs Steam with its 32-bit libraries, GameMode, MangoHud and gamescope, plus the right 32-bit Vulkan driver for your GPU. It also:</p>
<ul class="dl">
<li>sets <code>vm.max_map_count = 2147483642</code>, which some games need to avoid crashing</li>
<li>turns off split-lock mitigation, which can slow some games down</li>
<li>loads the <code>ntsync</code> module at boot, for Wine and Proton builds that support it</li>
</ul>
<p><code>auxo-tweak gaming off</code> removes the tweaks, and adding <code>--purge</code> removes the packages as well. Use <code>--no-steam</code> to skip Steam.</p>

<h2 id="motd">auxo-motd</h2>
<p>Each new terminal window opens with a short greeting: your accent logo, uptime, disk use, when you last ran <code>auxo-update</code> (with a nudge if it's been a while), and a tip. Text-console and SSH logins show the classic <code>/etc/motd</code> instead.</p>
<div class="code"><span class="ta">❯</span> auxo-motd --off   <span class="tm"># stop showing it</span>
<span class="ta">❯</span> auxo-motd --on    <span class="tm"># bring it back</span>
<span class="ta">❯</span> auxo-motd --tip   <span class="tm"># just a tip</span></div>
<p>Keep an eye on the tips. Not all of them are about settings.</p>

<h2 id="fetch">auxo-fetch</h2>
<p>A fast system summary drawn in your accent colour.</p>
<div class="code"><span class="ta">❯</span> auxo-fetch              <span class="tm"># full summary with logo</span>
<span class="ta">❯</span> auxo-fetch --small      <span class="tm"># compact</span>
<span class="ta">❯</span> auxo-fetch --no-logo
<span class="ta">❯</span> auxo-fetch --json       <span class="tm"># for scripts</span></div>

<h2 id="vm">Virtual machines</h2>
<p>Use the same image as for PCs. In VirtualBox, set the graphics controller to <b>VMSVGA</b> with <b>128 MB</b> of video memory and <b>3D acceleration off</b>. After installing, remove the image from the virtual drive, or the VM boots the live session again. See the <a class="inl" href="{DL}#vmsetup">VM setup steps</a>.</p>
<p>Guest tools for other hypervisors can be removed with <code>auxo-tweak drivers --prune</code>.</p>

<h2 id="trouble">Troubleshooting</h2>
<div class="faq">
<details><summary>The live session starts again after installing in a VM</summary><p>The VM is still booting from the image. Power off and remove it from the VM's optical drive.</p></details>
<details><summary>Black screen or a text console instead of the login screen</summary><p>Press <b>Ctrl+Alt+F2</b>, log in, and run <code>sudo sv status greetd elogind dbus</code> (or <code>gdm</code> for GNOME). Each should say <code>run</code>. Post the output in the Discord if one doesn't. With an NVIDIA card, run <code>auxo-tweak nvidia on</code> and reboot.</p></details>
<details><summary>The system won't boot after an update</summary><p>Pick the snapshots entry in the GRUB menu, boot a snapshot from before the update, then run <code>auxo-rollback</code>. See <a class="inl" href="#snapshots">Snapshots and rollback</a>.</p></details>
<details><summary>The USB stick won't boot</summary><p>Turn Secure Boot off in your firmware settings, and write the image in DD mode if you use Rufus.</p></details>
<details><summary>No Wi-Fi in the installer</summary><p>Press <b>N</b> on the installer's first page to connect, or run <code>nmtui</code> in a terminal. Without internet you still get a complete Plasma desktop.</p></details>
</div>

<h2 id="faq">FAQ</h2>
<div class="faq">
{faq_html}
<details><summary>Where can I get help?</summary><p>Ask in the <a class="inl" href="{DISCORD}">Auxo Discord</a>.</p></details>
</div>

  </article>
</div>
</main>
""" + footer()


# ================= EXTRA PAGES (v8) =================
import base64 as _b64
def _datauri(path, mime):
    return f"data:{mime};base64," + _b64.b64encode(open(path, "rb").read()).decode()

def page_head(eyebrow, title, lede, extra=""):
    return f'''<div class="phead"><div class="wrap">
  <p class="eyebrow rise">{eyebrow}</p>
  <h1 class="rise r1">{title}</h1>
  <p class="rise r2">{lede}</p>{extra}
</div></div>'''

def keytable(rows):
    rows = list(rows) + ([("", "")] if len(rows) % 2 else [])
    return '<div class="keys">' + "".join((f'<div><span>{" ".join(f"<kbd>{k}</kbd>" for k in keys.split("+"))}</span><em>{what}</em></div>' if keys else '<div class="kempty"></div>') for keys, what in rows) + '</div>'

def mini_screen(k):
    lay = {d[0]: d[5] for d in DESKS}[k]
    return f'<div class="screen mini" aria-hidden="true">{RIDGE}<div class="L" style="display:block">{lay}</div></div>'

KEYS_HYPR = [("Super+Enter", "Terminal (kitty)"), ("Super+D", "App launcher (wofi)"), ("Super+E", "File manager"), ("Super+T", "Open auxo-tweak"),
             ("Super+Q", "Close window"), ("Super+F", "Fullscreen"), ("Super+V", "Toggle floating"), ("Super+L", "Lock screen"),
             ("Super+1…9", "Switch workspace"), ("Print", "Screenshot a region"), ("Super+Print", "Screenshot to clipboard"), ("Super+Shift+E", "Exit Hyprland")]
KEYS_SWAY = [("Super+Enter", "Terminal (foot)"), ("Super+D", "App launcher (wofi)"), ("Super+Q", "Close window"), ("Super+F", "Fullscreen"),
             ("Super+L", "Lock screen"), ("Print", "Screenshot a region"), ("Super+Shift+E", "Exit Sway")]
KEYS_I3 = [("Super+Enter", "Terminal (alacritty)"), ("Super+D", "App launcher (rofi)"), ("Super+Q", "Close window"), ("Super+F", "Fullscreen"),
           ("Super+L", "Lock screen"), ("Print", "Screenshot a region to the clipboard"), ("Super+Shift+E", "Exit i3")]

DPAGE = [
 ("plasma", "KDE Plasma 6", "Default · Wayland", "The default desktop and the live session. A full, familiar desktop with the Auxo global theme, and the only one that installs without internet.",
  ["kde-plasma", "konsole", "dolphin", "kate", "ark", "spectacle", "kde-gtk-config"], None),
 ("hyprland", "Hyprland", "Auxo rice · Wayland", "Animated tiling with gaps and rounded corners. The bar, launcher, notifications, lock screen, idle and screenshots are all set up and themed in your accent. Comes from the hyprland-void community repository.",
  ["hyprland", "hyprpaper", "hypridle", "hyprlock", "Waybar", "wofi", "mako", "kitty", "grim + slurp + swappy"], KEYS_HYPR),
 ("sway", "Sway", "Auxo rice · Wayland", "i3-style tiling on Wayland. Lightweight and predictable, with waybar and your accent on the focused window.",
  ["sway", "swaybg", "swayidle", "swaylock", "Waybar", "wofi", "mako", "foot"], KEYS_SWAY),
 ("i3", "i3", "Auxo rice · X11", "Classic X11 tiling with no wasted space. polybar, rofi and picom are configured to match your accent.",
  ["i3", "polybar", "rofi", "picom", "dunst", "alacritty", "feh", "maim"], KEYS_I3),
 ("gnome", "GNOME", "Wayland", "Stock GNOME with gestures and the activities overview, started by its own login screen, GDM. Your accent is mapped to the nearest GNOME accent colour.",
  ["gnome-core", "gdm", "gnome-tweaks", "gnome-console", "nautilus"], None),
 ("xfce", "Xfce", "X11", "Light, fast and traditional. A good fit for older hardware.",
  ["xfce4", "xfce4-plugins", "network-manager-applet", "pavucontrol", "papirus-icon-theme"], None),
 ("cinnamon", "Cinnamon", "X11", "A traditional layout with a full-width bottom panel, menu and system tray.",
  ["cinnamon", "nemo", "gnome-terminal", "xed", "papirus-icon-theme"], None),
 ("none", "No desktop", "Text only", "Just the text login and a shell. Useful for servers, or for building your own setup from scratch. Add a desktop at any time.",
  [], None),
]
d_index = "".join(f'<a href="#d-{k}">{n}</a>' for k, n, *_ in DPAGE)
def dsec(k, n, tag, desc, pk, keys):
    rice = "Auxo rice" in tag
    pk_html = ('<p class="pkgs"><span>Installs</span>' + " · ".join(pk) + '</p>') if pk else '<p class="pkgs"><span>Installs</span>nothing extra</p>'
    keys_html = (f'<h4 class="kh">Keybindings</h4>{keytable(keys)}') if keys else ""
    extra = ""
    if rice:
        extra = f'<p class="note2">Changed a config and want the original back? <code>auxo-tweak rice {k}</code></p>'
    if k == "gnome":
        extra = '<p class="note2">Accent mapping: violet → purple, cyan → teal, emerald → green, amber → yellow, rose → pink, blue → blue, mono → slate.</p>'
    return f'''<section class="dsec" id="d-{k}">
  <div class="dsgrid">
    <div class="dtext rv">
      <p class="eyebrow">{tag}</p><h2>{n}</h2><p class="dl">{desc}</p>
      <div class="code"><span class="ta">❯</span> auxo-tweak desktop {k}</div>
      {pk_html}{extra}
    </div>
    <div class="dvis rv">{mini_screen(k)}{keys_html}</div>
  </div>
</section>'''

desk_page = head("Desktops — Auxo Linux",
                 "Every desktop Auxo Linux offers: KDE Plasma, Hyprland, Sway, i3, GNOME, Xfce and Cinnamon. What each one installs, its keybindings, and the command to switch.",
                 "https://auxolinux.com/desktops/") + nav("desktops") + f"""
<main id="main">
{page_head("Desktops", "Eight desktops. One command.", "Pick a desktop in the installer, or switch any time with <code>auxo-tweak desktop</code>. Your previous config files are kept as <code>*.auxo-bak</code>, and the login screen follows your choice.",
  f'<div class="chips rise r3"><span><b>4</b> Wayland</span><span><b>3</b> X11</span><span><b>3</b> Auxo rices</span><span><b>1</b> text-only</span></div>')}
<nav class="subnav" aria-label="Desktops"><div class="wrap">{d_index}</div></nav>
<div class="wrap">{"".join(dsec(*d) for d in DPAGE)}</div>
<div class="band" style="padding-top:40px"><div class="wrap"><div class="inner rv">
  {mark()}
  <div><h2>Can't decide? You don't have to.</h2><p>Try one, switch tomorrow. Every change is one command, and snapshots have your back.</p></div>
  <div class="cta"><a class="btn primary lg" href="{DL}">{I['dl']}Get Auxo</a><a class="btn lg" href="{DOCS}#desktops">Desktop docs</a></div>
</div></div></div>
</main>
""" + footer()

# ---------- releases ----------
REL = [
 ("4.0", "2026-10-05", "Now built on Void Linux", [
   ("new", "Auxo moves from Arch Linux to Void Linux: runit and xbps, no systemd. Void's repositories and docs work as normal."),
   ("new", "A new text installer: keyboard-driven, with a review screen. Erase a disk (UEFI or BIOS) or use existing partitions to dual boot; btrfs with subvolumes, ext4 or xfs."),
   ("new", "<code>auxo-tweak nvidia</code> picks <code>nvidia</code>, <code>nvidia580</code> or <code>nvidia470</code> for your card. New <code>scheduler</code> (sched-ext), <code>power</code>, <code>firewall</code>, <code>dns</code>, <code>cleanup</code>, <code>flatpak</code> and <code>repo</code> commands."),
   ("new", "Kernels: Void's <code>linux</code>, <code>linux-lts</code> and <code>linux-mainline</code>. Your pick stays the default boot entry after updates."),
   ("new", "Snapshots before every <code>auxo-update</code> and every hour, bootable from GRUB."),
   ("fix", "No boot splash: Auxo boots straight to the login screen."),
 ]),
]
REL_ARCH = [
 ("3.0.4", "", "GNOME and the Auxo Plasma theme", [
   ("fix", "GNOME starts again after install: it now uses its own login screen, GDM."),
   ("new", "The Auxo global theme for KDE Plasma: Auxo Dark colours with your accent, and a floating dock-style panel."),
   ("fix", "Installs no longer fail at the GRUB step with kernels other than linux-zen or shells other than zsh."),
 ]),
 ("3.0.3", "", "Login fix for every desktop", [
   ("fix", "Fixed a black screen after install when a desktop other than KDE Plasma was chosen, on VMs and real hardware."),
   ("new", "Installed systems now use a text login screen (greetd + tuigreet) in your accent colour. It starts the desktop you picked, and F3 switches session."),
 ]),
 ("3.0.2", "", "Boot splash and gaming", [
   ("new", "Animated boot splash in your accent colour."),
   ("new", "<code>auxo-tweak gaming on|off|status</code>: Steam, GameMode, MangoHud, Gamescope and gaming tweaks."),
 ]),
 ("3.0.1", "", "Virtual machine fixes", [
   ("fix", "Installed systems now boot in VirtualBox and other VMs."),
   ("new", "<code>auxo-tweak drivers --prune</code> keeps only the guest tools for the hypervisor you're on."),
 ]),
 ("3.0", "2026-09-26", "First public release", [
   ("new", "Calamares installer with Desktop, Accent, Kernel, Shell and Software pages."),
   ("new", "<code>auxo-tweak</code>, <code>auxo-update</code>, <code>auxo-rollback</code>, <code>auxo-fetch</code> and the welcome app."),
 ]),
]
def relcard(v, date, title, items):
    lis = "".join(f'<li><b class="{t}">{"New" if t=="new" else "Fix"}</b><span>{x}</span></li>' for t, x in items)
    badge = f'<span class="rbadge">{date}</span>' if date else ""
    return f'<article class="tl rv" id="v{v.replace(".","-")}"><div class="tldot"></div><div class="tlcard"><header><h2>Auxo {v}</h2>{badge}</header><p class="tlt">{title}</p><ul class="rel">{lis}</ul></div></article>'

rel_page = head("Releases — Auxo Linux",
                "Auxo Linux release notes: what changed in each version, and links to every image on the Internet Archive.",
                "https://auxolinux.com/releases/") + nav("releases") + f"""
<main id="main">
{page_head("Releases", "What's new in Auxo", "Auxo is a rolling release. Once it's installed, <code>auxo-update</code> keeps you current, so you never need to reinstall for a new version. Auxo 4 is built on Void Linux; Auxo 1 to 3 were built on Arch Linux and are discontinued.",
  f'<div class="cta rise r3" style="margin-top:28px"><a class="btn primary lg" href="{DL}">{I["dl"]}Download the latest</a><a class="btn lg" href="https://github.com/rxvy-dev/AUXO">Source on GitHub</a></div>')}
<section><div class="wrap">
  <div class="timeline">
    {"".join(relcard(*r) for r in REL)}
    <article class="tl rv" id="arch"><div class="tldot"></div><div class="tlcard"><header><h2>Auxo 3 (Arch Linux)</h2><span class="rbadge">Discontinued</span></header><p class="tlt">The Arch-based line ends here</p><p class="muted">Auxo 3 won't get new images or tool updates. Its final images are still on the <a class="inl" href="{DL}#arch">download page</a>, and the source is on GitHub on the <a class="inl" href="{ARCH_SRC}">arch</a> branch.</p></div></article>
    {"".join(relcard(*r) for r in REL_ARCH)}
    <article class="tl rv"><div class="tldot"></div><div class="tlcard"><header><h2>Auxo 2</h2><span class="rbadge">2026-07-19</span></header><p class="tlt">Earlier release</p><p class="muted">The image is kept on the Internet Archive. <a class="inl" href="https://archive.org/details/auxo-linux-2026.07.19-0048-x86_64">View on archive.org</a></p></div></article>
    <article class="tl rv"><div class="tldot"></div><div class="tlcard"><header><h2>Auxo 1</h2><span class="rbadge">2026-07-09</span></header><p class="tlt">The first Auxo image</p><p class="muted">The image is kept on the Internet Archive. <a class="inl" href="https://archive.org/details/auxo-linux-2026.07.09-x86_64">View on archive.org</a></p></div></article>
  </div>
</div></section>
</main>
""" + footer()

# ---------- community ----------
com_page = head("Community — Auxo Linux",
                "Join the Auxo Linux community: Discord, r/DistroHub and GitHub. Get help, share your setup, report bugs and contribute.",
                "https://auxolinux.com/community/") + nav("community") + f"""
<main id="main">
{page_head("Community", "Built in the open, with you", "Auxo is a small, independent project. Every bug report, screenshot and suggestion shapes where it goes next.")}
<section style="padding-top:64px"><div class="wrap">
  <div class="cgrid">
    <a class="ccard big rv" href="{DISCORD}"><div class="cico">{I['discord']}</div><div><h2>Discord</h2><p>The main place to hang out. Install help, rice showcases, wallpapers, and release news first.</p><span class="btn primary">{I['discord']}Join the Discord</span></div></a>
    <a class="ccard rv" href="https://www.reddit.com/r/DistroHub/"><div class="cico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="13" r="8"/><circle cx="9" cy="13" r="1"/><circle cx="15" cy="13" r="1"/><path d="M9 16.5c1.8 1.2 4.2 1.2 6 0M16 5l-4-1-1 5"/><circle cx="18" cy="5" r="1.5"/></svg></div><div><h2>r/DistroHub</h2><p>A subreddit for discovering new and indie Linux distros, Auxo included.</p><span class="more">Visit the subreddit →</span></div></a>
    <a class="ccard rv" href="https://github.com/rxvy-dev/AUXO"><div class="cico"><svg viewBox="0 0 24 24" fill="currentColor"><path d="M12 2a10 10 0 0 0-3.2 19.5c.5.1.7-.2.7-.5v-1.7c-2.8.6-3.4-1.3-3.4-1.3-.4-1.2-1.1-1.5-1.1-1.5-.9-.6.1-.6.1-.6 1 .1 1.5 1 1.5 1 .9 1.5 2.4 1.1 2.9.8.1-.7.4-1.1.6-1.3-2.2-.3-4.6-1.1-4.6-5 0-1.1.4-2 1-2.7-.1-.3-.4-1.3.1-2.7 0 0 .8-.3 2.8 1a9.6 9.6 0 0 1 5 0c1.9-1.3 2.8-1 2.8-1 .5 1.4.2 2.4.1 2.7.6.7 1 1.6 1 2.7 0 3.9-2.4 4.7-4.6 5 .4.3.7.9.7 1.9V21c0 .3.2.6.7.5A10 10 0 0 0 12 2z"/></svg></div><div><h2>GitHub</h2><p>The source code for the installer, auxo-tweak and every other Auxo tool.</p><span class="more">View the source →</span></div></a>
  </div>
</div></section>

<section class="alt"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow">Get involved</p><h2>Ways to help</h2></div><p>No code required for most of these.</p></div>
  <div class="cards5">
    <div class="rv"><svg viewBox="0 0 24 24"><path d="M12 3l9 16H3z"/><path d="M12 10v4M12 17h.01"/></svg><h3>Report bugs</h3><p>Something broke? Tell us what you did, what happened and your hardware. <a class="inl" href="https://github.com/rxvy-dev/AUXO/issues">Open an issue</a> or post in Discord.</p></div>
    <div class="rv"><svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8M12 17v4"/></svg><h3>Share your setup</h3><p>Post screenshots of your rice in Discord. The best ones get featured.</p></div>
    <div class="rv"><svg viewBox="0 0 24 24"><path d="M4 12a8 8 0 0 1 16 0"/><path d="M12 4v16M8 16l4 4 4-4"/></svg><h3>Test releases</h3><p>Try new images on real hardware and in VMs, and say what worked.</p></div>
    <div class="rv"><svg viewBox="0 0 24 24"><path d="M3 12h4l3-8 4 16 3-8h4"/></svg><h3>Spread the word</h3><p>Tell a friend, write a review, or post about Auxo. It all helps a small project.</p></div>
  </div>
</div></section>

<section><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow">Developers</p><h2>Build Auxo from source</h2></div><p>Everything, including the ISO, builds from the repository.</p></div>
  <div class="two">
    <div class="card rv"><h3>Build the ISO</h3><p>On any Linux distro, no Docker needed. Takes 20–40 minutes and about 15 GB of free space.</p>
      <div class="code"><span class="tm">$ </span>git clone https://github.com/rxvy-dev/AUXO.git
<span class="tm">$ </span>cd AUXO
<span class="tm">$ </span>./build.sh --clean
<span class="tm">$ </span>./scripts/test-vm.sh out/auxo-linux-*.iso</div>
      <p class="muted" style="margin-top:12px">It sets up a small Void system in <code>out/</code> and builds inside it. The Arch-based Auxo 3 is on the <a class="inl" href="{ARCH_SRC}">arch</a> branch.</p></div>
    <div class="card rv"><h3>Run the tests</h3><p>Both suites run anywhere, without root.</p>
      <div class="code"><span class="tm">$ </span>./tests/test-tools.sh
<span class="tm">$ </span>python3 tests/test-installer.py</div>
      <p class="muted" style="margin-top:12px">Please run both before sending a pull request.</p></div>
  </div>
</div></section>

<section class="alt"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow">Guidelines</p><h2>Be excellent to each other</h2></div><p>A few simple rules for every Auxo space.</p></div>
  <div class="rules">
    <div class="rv"><b>1</b><p>Be kind and patient, especially with people new to Linux.</p></div>
    <div class="rv"><b>2</b><p>No distro wars. Every distro has its place.</p></div>
    <div class="rv"><b>3</b><p>Help others the way you'd want to be helped: with detail, not "RTFM".</p></div>
    <div class="rv"><b>4</b><p>No spam, piracy or harassment.</p></div>
  </div>
</div></section>
</main>
""" + footer()

# ---------- press kit ----------
LOGO_DIR = os.path.join(HERE, "..", "branding", "logo")
lg_svg = _datauri(os.path.join(LOGO_DIR, "auxo-mark.svg"), "image/svg+xml")
lg_word = _datauri(os.path.join(LOGO_DIR, "auxo-logo-wordmark.png"), "image/png")
lg_icon = _datauri(os.path.join(LOGO_DIR, "auxo-logo-icon-512.png"), "image/png")

FACTS = [("Name", "Auxo Linux"), ("Based on", "Void Linux (Auxo 1–3: Arch Linux)"), ("Init system", "runit"), ("Release model", "Rolling"), ("Latest version", "4.0"),
         ("Architecture", "x86_64"), ("Installer", "Auxo's own text installer"), ("Default desktop", "KDE Plasma 6"),
         ("Desktops", "Plasma, GNOME, Xfce, Cinnamon, Hyprland, Sway, i3"), ("Developer", "rxvy"),
         ("Website", '<a class="inl" href="https://auxolinux.com">auxolinux.com</a>'), ("Source", '<a class="inl" href="https://github.com/rxvy-dev/AUXO">github.com/rxvy-dev/AUXO</a>')]
facts_html = "".join(f"<tr><th>{a}</th><td>{b}</td></tr>" for a, b in FACTS)

BLURB_S = "Auxo Linux is a Void-based distro where you can switch your whole desktop with one command, without reinstalling."
BLURB_M = ("Auxo Linux is a rolling distribution built on Void Linux, with runit and no systemd. Users pick their desktop, kernel, shell "
           "and accent colour in a fast text installer, and can change any of them later with a single command, auxo-tweak. Every "
           "update takes a bootable btrfs snapshot, so a bad update can be rolled back from the boot menu.")
BLURB_L = ("Auxo Linux is an independent rolling distribution built on Void Linux around one idea: your setup should be easy to change. "
           "Its keyboard-driven text installer lets users choose between KDE Plasma 6, GNOME, Xfce, Cinnamon, or ready-made Hyprland, "
           "Sway and i3 setups, along with a kernel, shell and one of seven accent colours that theme everything from the boot menu to "
           "the terminal, and it changes nothing until the user confirms a summary. After installing, the auxo-tweak tool changes any of "
           "those choices with one command, using the same code the installer runs, and handles NVIDIA drivers, sched-ext CPU "
           "schedulers, firewall and DNS. Snapper takes a snapshot before every update, and auxo-rollback restores one if something breaks. "
           "Auxo uses the official Void repositories, xbps and runit. Versions 1 to 3 were based on Arch Linux.")

shots_dl = [(TUI[k], t, True) for k, t in
            [("welcome", "Installer: welcome"), ("desktop", "Installer: pick a desktop"), ("accent", "Installer: pick an accent"), ("extras", "Installer: extras"), ("review", "Installer: review and install")]]
shots_html = "".join(
    f'<figure class="rv"><img src="{src}" alt="{t}" loading="lazy"><figcaption><span>{t}</span></figcaption></figure>'
    for n, (src, t, embedded) in enumerate(shots_dl))

press_page = head("Press kit — Auxo Linux",
                  "Auxo Linux press kit: fact sheet, ready-to-use descriptions, logos, brand colours and screenshots for reviews and articles.",
                  "https://auxolinux.com/press/") + nav("press") + f"""
<main id="main">
{page_head("Press kit", "Writing about Auxo?", "Everything you need for a review, article or video: the facts, ready-to-use descriptions, logos and screenshots. Use any of it freely.")}
<section style="padding-top:64px"><div class="wrap">
  <div class="two">
    <div class="card rv"><h3>At a glance</h3><div class="facts"><table>{facts_html}</table></div></div>
    <div class="card rv"><h3>Descriptions</h3><p class="muted">Copy whichever length fits.</p>
      <div class="blurb"><small>One line</small><p>{BLURB_S}</p></div>
      <div class="blurb"><small>Short · about 60 words</small><p>{BLURB_M}</p></div>
      <div class="blurb"><small>Long · about 130 words</small><p>{BLURB_L}</p></div>
    </div>
  </div>
</div></section>

<section class="alt"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow">Brand</p><h2>Logos</h2></div><p>The Auxo mark is a mountain peak with a climbing trail and a summit dot. Please don't stretch, recolour or redraw it.</p></div>
  <div class="logogrid">
    <div class="lg rv"><div class="lgv dark"><svg class="pm" viewBox="0 0 128 128"><use href="#pmark"/></svg></div><div class="lgf"><b>Mark</b><span>SVG · scalable, transparent</span><a class="btn sm" href="{lg_svg}" download="auxo-mark.svg">{I['dl']}SVG</a></div></div>
    <div class="lg rv"><div class="lgv light"><svg class="pm" viewBox="0 0 128 128"><use href="#pmark"/></svg></div><div class="lgf"><b>Mark on light</b><span>Same SVG works on light backgrounds</span><a class="btn sm" href="{lg_svg}" download="auxo-mark.svg">{I['dl']}SVG</a></div></div>
    <div class="lg rv"><div class="lgv dark wide"><span class="wm"><svg class="pm" viewBox="0 0 128 128"><use href="#pmark"/></svg>auxo</span></div><div class="lgf"><b>Wordmark</b><span>PNG · 1840×560, for dark backgrounds</span><a class="btn sm" href="{lg_word}" download="auxo-logo-wordmark.png">{I['dl']}PNG</a></div></div>
    <div class="lg rv"><div class="lgv"><span class="tile"><svg class="pm" viewBox="0 0 128 128"><use href="#pmark"/></svg></span></div><div class="lgf"><b>App icon</b><span>PNG · 512 px, dark tile</span><a class="btn sm" href="{lg_icon}" download="auxo-logo-icon.png">{I['dl']}PNG</a></div></div>
  </div>
  <svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs><linearGradient id="pg" x1="0" y1="1" x2="1" y2="0"><stop offset="0" stop-color="#a78bfa"/><stop offset="1" stop-color="#22d3ee"/></linearGradient><symbol id="pmark" viewBox="0 0 128 128"><g fill="none" stroke="url(#pg)" stroke-linecap="round" stroke-linejoin="round"><path d="M16 112 L64 22 L112 112" stroke-width="13"/><path d="M40 96 L56 78 L68 90 L86 68" stroke-width="8"/><circle cx="64" cy="9" r="6.5" fill="url(#pg)" stroke="none"/></g></symbol></defs></svg>
  <h3 class="subh">Colours</h3>
  <div class="colors">
    <div class="rv"><i style="background:#a78bfa"></i><b>Violet</b><code>#A78BFA</code><span>Logo gradient start</span></div>
    <div class="rv"><i style="background:#22d3ee"></i><b>Cyan</b><code>#22D3EE</code><span>Logo gradient end</span></div>
    <div class="rv"><i style="background:#fbbf24"></i><b>Amber</b><code>#FBBF24</code><span>Website accent</span></div>
    <div class="rv"><i style="background:#0e1014;box-shadow:inset 0 0 0 1px #2f3641"></i><b>Night</b><code>#0E1014</code><span>Background</span></div>
  </div>
</div></section>

<section><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow">Media</p><h2>Screenshots</h2></div><p>Real screenshots from Auxo 4.0, free to use in coverage. Right-click an image and choose <b>Save image as</b>.</p></div>
  <div class="pshots">{shots_html}</div>
</div></section>

<section class="alt"><div class="wrap">
  <div class="sec-head"><div><p class="eyebrow">Contact</p><h2>Get in touch</h2></div><p>Questions, interview requests or anything else? Reach out.</p></div>
  <div class="cgrid two-up">
    <a class="ccard rv" href="{DISCORD}"><div class="cico">{I['discord']}</div><div><h2>Discord</h2><p>The fastest way to reach the developer.</p><span class="more">Join the Discord →</span></div></a>
    <a class="ccard rv" href="https://github.com/rxvy-dev/AUXO"><div class="cico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M9 19c-4 1.5-4-2-6-2.5M15 21v-3.5c0-1 .1-1.4-.5-2 2.8-.3 5.5-1.4 5.5-6a4.6 4.6 0 0 0-1.3-3.2 4.2 4.2 0 0 0-.1-3.2s-1.1-.3-3.5 1.3a12 12 0 0 0-6.2 0C6.5 2.8 5.4 3.1 5.4 3.1a4.2 4.2 0 0 0-.1 3.2A4.6 4.6 0 0 0 4 9.5c0 4.6 2.7 5.7 5.5 6-.6.6-.6 1.2-.5 2V21"/></svg></div><div><h2>GitHub</h2><p>Source code, issues and technical details.</p><span class="more">Open GitHub →</span></div></a>
  </div>
</div></section>
</main>
""" + footer()

os.makedirs(OUT, exist_ok=True)
if PREVIEW:
    open(f"{OUT}/index.html", "w").write(home)
    open(f"{OUT}/download.html", "w").write(dl)
    open(f"{OUT}/docs.html", "w").write(docs)
    for _n, _pg in [("desktops", desk_page), ("releases", rel_page), ("community", com_page), ("press", press_page)]:
        open(f"{OUT}/{_n}.html", "w").write(_pg)
else:
    os.makedirs(f"{OUT}/download", exist_ok=True)
    open(f"{OUT}/index.html", "w").write(home)
    open(f"{OUT}/download/index.html", "w").write(dl)
    os.makedirs(f"{OUT}/docs", exist_ok=True)
    open(f"{OUT}/docs/index.html", "w").write(docs)
    for _n, _pg in [("desktops", desk_page), ("releases", rel_page), ("community", com_page), ("press", press_page)]:
        os.makedirs(f"{OUT}/{_n}", exist_ok=True)
        open(f"{OUT}/{_n}/index.html", "w").write(_pg)
print("built", OUT, len(home), len(dl), len(docs), len(desk_page), len(rel_page), len(com_page), len(press_page))
