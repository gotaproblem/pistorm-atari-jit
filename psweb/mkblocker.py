#!/usr/bin/env python3
"""
mkblocker.py - EasyList + EasyPrivacy -> WebKit content-blocker JSON for psweb.

Reads the EasyList section files (the repo splits the lists by kind; the
allowlists come last so their ignore-previous-rules entries win) and writes
the rule array WebKit's UserContentFilterStore compiles. What is kept:

  ||host^ / ||host/path   -> anchored host rule           (the bulk of it)
  |http://...  plain / *  -> url-filter regex, WebKit's own grammar (no |)
  $third-party, ~third-party, domain=, script/image/... resource types
  @@ exceptions           -> ignore-previous-rules
  ##selector (generic)    -> css-display-none, batched 200 selectors a rule

What is dropped, with a count printed: options WebKit cannot express
($csp $redirect $removeparam $rewrite $header $replace $important $badfilter
$genericblock $elemhide $generichide $match-case $popunder $document ...),
rules mixing domain= and ~domain= (WebKit forbids if-domain + unless-domain),
regex rules (/.../), non-ASCII patterns, site-specific hides, and the
procedural / extended-CSS cosmetic rules.

  python3 mkblocker.py --fetch            downloads the section files
  python3 mkblocker.py                    writes adblock.json + adblock-lite.json
  python3 mkblocker.py --lite-only        only the small list (hosts, no CSS)

WebKit compiles the JSON into a DFA once and caches it (psweb keeps the
compiled store under XDG_CACHE_HOME); on a Pi 4 the full list takes a
minute and a few hundred MB the first time - install-full.sh runs
`psweb --compile-filter` for that so the first browser start is instant.
The lite list (hosts only: ad servers + tracking servers) is the fallback
for a 1 GB Pi.
"""
import json
import os
import re
import sys
import urllib.request

RAW = "https://raw.githubusercontent.com/easylist/easylist/master/"

# order matters: blocks first, allowlists (ignore-previous-rules) last
FULL = [
    "easylist/easylist_adservers.txt",
    "easylist/easylist_adservers_popup.txt",
    "easylist/easylist_thirdparty.txt",
    "easylist/easylist_thirdparty_popup.txt",
    "easylist/easylist_general_block.txt",
    "easylist/easylist_general_block_popup.txt",
    "easylist/easylist_specific_block.txt",
    "easylist/easylist_specific_block_popup.txt",
    "easyprivacy/easyprivacy_trackingservers.txt",
    "easyprivacy/easyprivacy_trackingservers_international.txt",
    "easyprivacy/easyprivacy_general.txt",
    "easyprivacy/easyprivacy_thirdparty.txt",
    "easyprivacy/easyprivacy_thirdparty_international.txt",
    "easyprivacy/easyprivacy_specific.txt",
    "easyprivacy/easyprivacy_specific_international.txt",
    "easylist/easylist_general_hide.txt",
    "easylist/easylist_allowlist.txt",
    "easylist/easylist_allowlist_general_hide.txt",
    "easyprivacy/easyprivacy_allowlist.txt",
    "easyprivacy/easyprivacy_allowlist_international.txt",
]
LITE = [
    "easylist/easylist_adservers.txt",
    "easyprivacy/easyprivacy_trackingservers.txt",
    "easyprivacy/easyprivacy_trackingservers_international.txt",
    "easylist/easylist_allowlist.txt",
    "easyprivacy/easyprivacy_allowlist.txt",
]

# ABP option -> WebKit resource-type
RTYPE = {
    "script": "script", "image": "image", "stylesheet": "style-sheet",
    "font": "font", "media": "media", "xmlhttprequest": "raw",
    "xhr": "raw", "websocket": "raw", "other": "raw", "ping": "ping",
    "popup": "popup", "subdocument": "document", "object": "raw",
    "document": "document",
}
ALL_TYPES = ["document", "image", "style-sheet", "script", "font", "raw",
             "svg-document", "media", "popup", "ping"]
UNSUPPORTED = {"csp", "redirect", "redirect-rule", "removeparam", "rewrite",
               "header", "replace", "important", "badfilter", "genericblock",
               "elemhide", "generichide", "match-case", "popunder",
               "inline-script", "inline-font", "empty", "mp4", "cookie",
               "all", "1p", "3p", "strict1p", "strict3p", "denyallow",
               "method", "to", "from", "permissions", "urltransform",
               "object-subrequest", "webrtc", "ipaddress", "reason"}

dropped = {}


def drop(why):
    dropped[why] = dropped.get(why, 0) + 1


def esc(ch):
    return "\\" + ch if ch in ".+?^$(){}[]\\|" else ch


def pattern_to_regex(p):
    """ABP URL pattern -> WebKit url-filter. Returns None if it can't."""
    if not p.isascii():
        drop("non-ascii pattern")
        return None
    if len(p) >= 2 and p[0] == "/" and p[-1] == "/":
        drop("regex rule")
        return None
    anchored_host = p.startswith("||")
    if anchored_host:
        p = p[2:]
    anchored_start = p.startswith("|")
    if anchored_start:
        p = p[1:]
    anchored_end = p.endswith("|")
    if anchored_end:
        p = p[:-1]
    if not p:
        return None
    out = []
    if anchored_host:
        out.append("^https?://([^/]+\\.)?")
    elif anchored_start:
        out.append("^")
    for i, ch in enumerate(p):
        if ch == "*":
            out.append(".*")
        elif ch == "^":
            # separator: anything that is not a URL character, or the end
            if i == len(p) - 1:
                out.append("([^a-zA-Z0-9_.%-].*)?$")
            else:
                out.append("[^a-zA-Z0-9_.%-]")
        else:
            out.append(esc(ch))
    if anchored_end:
        out.append("$")
    return "".join(out)


def parse_options(opts):
    """-> (trigger extras dict) or None when not expressible."""
    t = {}
    types, ntypes = [], []
    for o in opts.split(","):
        o = o.strip()
        if not o:
            continue
        neg = o.startswith("~")
        name, _, val = o[1:].partition("=") if neg else o.partition("=")
        if name == "third-party":
            t["load-type"] = ["first-party"] if neg else ["third-party"]
        elif name == "domain":
            inc, exc = [], []
            for d in val.split("|"):
                if not d:
                    continue
                if d.startswith("~"):
                    exc.append("*" + d[1:])
                else:
                    inc.append("*" + d)
            if inc and exc:
                drop("domain= mixed with ~domain=")
                return None
            if inc:
                t["if-domain"] = inc
            if exc:
                t["unless-domain"] = exc
        elif name in RTYPE:
            (ntypes if neg else types).append(RTYPE[name])
        elif name in UNSUPPORTED:
            drop("$" + name)
            return None
        else:
            drop("unknown option $" + name)
            return None
    if types:
        t["resource-type"] = sorted(set(types))
    elif ntypes:
        t["resource-type"] = [x for x in ALL_TYPES if x not in ntypes]
    return t


def convert_line(line, lite):
    line = line.strip()
    if not line or line[0] in "![":
        return None
    if "#" in line and ("##" in line or "#@#" in line or "#?#" in line or "#$#" in line):
        # cosmetic
        if lite:
            return None
        if "#?#" in line or "#$#" in line or "#@#" in line or "#%#" in line:
            drop("procedural/extended cosmetic")
            return None
        dom, _, sel = line.partition("##")
        if dom:
            drop("site-specific hide")
            return None
        if not sel.isascii() or '"' in sel:
            drop("cosmetic selector not usable")
            return None
        return ("css", sel)
    exception = line.startswith("@@")
    if exception:
        line = line[2:]
    pat, _, opts = line.partition("$")
    extras = parse_options(opts) if opts else {}
    if extras is None:
        return None
    if lite and extras:
        return None
    rx = pattern_to_regex(pat)
    if rx is None:
        return None
    trig = {"url-filter": rx}
    trig.update(extras)
    return ("rule", {"trigger": trig,
                     "action": {"type": "ignore-previous-rules" if exception else "block"}})


def build(files, lite):
    rules, css = [], []
    seen = set()
    for f in files:
        name = os.path.basename(f)
        if not os.path.exists(name):
            print("missing", name, "(run --fetch)", file=sys.stderr)
            sys.exit(1)
        with open(name, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                r = convert_line(line, lite)
                if r is None:
                    continue
                if r[0] == "css":
                    css.append(r[1])
                else:
                    key = json.dumps(r[1], sort_keys=True)
                    if key in seen:
                        continue
                    seen.add(key)
                    rules.append(r[1])
    # the css rules sit before the allowlist so @@ generic-hide exceptions win
    out = []
    ncss = 0
    for r in rules:
        if r["action"]["type"] == "ignore-previous-rules" and css and not ncss:
            for i in range(0, len(css), 200):
                out.append({"trigger": {"url-filter": ".*"},
                            "action": {"type": "css-display-none",
                                       "selector": ", ".join(css[i:i + 200])}})
                ncss += 1
        out.append(r)
    if css and not ncss:
        for i in range(0, len(css), 200):
            out.append({"trigger": {"url-filter": ".*"},
                        "action": {"type": "css-display-none",
                                   "selector": ", ".join(css[i:i + 200])}})
            ncss += 1
    return out, len(css), ncss


def main():
    if "--fetch" in sys.argv:
        for f in FULL:
            name = os.path.basename(f)
            print("fetching", name)
            urllib.request.urlretrieve(RAW + f, name)
        return
    sets = [("adblock-lite.json", LITE, True)]
    if "--lite-only" not in sys.argv:
        sets.insert(0, ("adblock.json", FULL, False))
    for outname, files, lite in sets:
        dropped.clear()
        rules, nsel, ncss = build(files, lite)
        with open(outname, "w") as fh:
            json.dump(rules, fh, separators=(",", ":"))
        nb = sum(1 for r in rules if r["action"]["type"] == "block")
        ni = sum(1 for r in rules if r["action"]["type"] == "ignore-previous-rules")
        print("%s: %d rules (%d block, %d exceptions, %d css rules holding %d selectors), %d KB"
              % (outname, len(rules), nb, ni, ncss, nsel, os.path.getsize(outname) // 1024))
        for k, v in sorted(dropped.items(), key=lambda kv: -kv[1]):
            print("   dropped %6d  %s" % (v, k))


if __name__ == "__main__":
    main()
