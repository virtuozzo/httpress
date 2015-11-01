# httpress

High performance HTTP server stress & benchmark utility.

Inspired by [weighttp](http://redmine.lighttpd.net/projects/weighttp/wiki) tool.

## Main features:

* event driven (low memory footprint, large number of connections)
* multi-threaded (uses all cores of your CPU)
* SSL support (via GNUTLS library) with cipher suite selection

Compared to weighttp, httpress offers the following improvements:

* evenly distributes load between threads; does not allow one thread to finish much earlier than others
* promptly timeouts stucked connections, forces all hanging connections to close after the main run, does not allow hanging or interrupted connections to affect the measurement
* SSL support

## Usage

```
httpress <options> <url>
  -n num   number of requests     (default: 1)
  -t num   number of threads      (default: 1)
  -c num   concurrent connections (default: 1)
  -k       keep alive             (default: no)
  -z pri   GNUTLS cipher priority (default: NORMAL)
  -h       show this help

example: httpress -n 10000 -c 100 -t 4 -k http://localhost:8080/index.html
```

## Dependencies

* Depends on [libev](http://software.schmorp.de/pkg/libev.html) library
* Depends on [http-parser](https://github.com/nodejs/http-parser) library
* Depends on [LibParserUtils](http://www.netsurf-browser.org/projects/libparserutils/) library
* Depends on [uchardet](https://github.com/BYVoid/uchardet) library
* Depends on [GnuTLS](http://www.gnu.org/software/gnutls/) library (if compiled with SSL support)

## Building from source

1. Prerequisite (your distro might have these in repo; otherwise build from source):
  1. [libev 4 library](http://software.schmorp.de/pkg/libev.html)
  2. [http-parser 2 library](https://github.com/nodejs/http-parser)
  3. [LibParserUtils 0.2.0 library](http://www.netsurf-browser.org/projects/libparserutils/)
  4. [uchardet library](https://github.com/BYVoid/uchardet)
2. SSL prerequisite: [GnuTLS 3.0 library](http://www.gnu.org/software/gnutls/) (better build from source)
3. Download [httpress source](https://github.com/CloudServer/httpress)
4. Run `make` or `make -f Makefile.nossl`
5. Collect executable from bin subdirectory
