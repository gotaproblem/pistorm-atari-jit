# nseval - NetSurf evaluation for psweb

Can [NetSurf](https://www.netsurf-browser.org) replace WPE WebKit as psweb's
engine? NetSurf is plain C with its own HTML/CSS/DOM libraries and Duktape
(ES5) for JavaScript. It renders on the CPU into a pixel buffer: no GPU, EGL,
Mesa, LLVM or sandbox. Its system libraries are core Debian ones: libcurl,
OpenSSL, libpng, libjpeg, expat, utf8proc and FreeType.

This evaluation builds NetSurf's framebuffer front end with the VNC surface,
so you can browse from your PC with any VNC viewer. Everything is built into
`~/netsurf-eval`; nothing is installed system-wide.

## 1. Packages (on the Pi)

```
sudo apt install --no-install-recommends build-essential pkg-config git gperf \
    flex bison libhtml-parser-perl libcurl4-openssl-dev libssl-dev \
    libexpat1-dev libpng-dev libjpeg-dev libutf8proc-dev libfreetype-dev \
    fonts-dejavu-core libvncserver-dev
```

`libvncserver-dev` is only for this evaluation; a psweb engine would not use it.

## 2. Build (about 5 minutes on a Pi 4)

```
~/pistorm-atari-jit/tools/nseval/nsbuild.sh
```

It clones NetSurf and its libraries from GitHub into `~/netsurf-eval`, prints
each commit, then builds. The last line should list `netsurf/nsfb`.

## 3. Run

```
cd ~/netsurf-eval/netsurf
taskset -c 1 ./nsfb -f vnc -w 1302 -h 863 https://www.exxosforum.co.uk/forum/
```

`taskset -c 1` puts it on psweb's core. 1302x863 is WEBGEM's view size.
Connect a VNC viewer on your PC to `<pi address>:5900`. Type URLs into the
address bar at the top. Ctrl-C in the terminal quits.

Memory, from a second terminal:

```
ps -o rss=,cmd= -C nsfb
```

## What to try

* exxosforum: browse, log in, post.
* https://html.duckduckgo.com/html/ - search.
* https://en.wikipedia.org/wiki/Atari_ST
* https://www.atari-forum.com
* https://www.bbc.co.uk/news
* Anything else you use from WEBGEM.

For each, check: does the page look right? Is scrolling usable? Does what
needs JavaScript (menus, logins, buttons) work? What is the RSS?
