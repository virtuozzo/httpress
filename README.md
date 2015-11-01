= httpress

High performance HTTP server stress & benchmark utility.

Inspired by [[http://redmine.lighttpd.net/projects/weighttp/wiki|weighttp]] tool.

Main features:

* event driven (low memory footprint, large number of connections)
* multi-threaded (uses all cores of your CPU)
* SSL support (via GNUTLS library) with cipher suite selection

Compared to weighttp, httpress offers the following improvements:

* evenly distributes load between threads; does not allow one thread to finish much earlier than others
* promptly timeouts stucked connections, forces all hanging connections to close after the main run, does not allow hanging or interrupted connections to affect the measurement
* SSL support

== Usage

{{{
httpress <options> <url>
  -n num   number of requests     (default: 1)
  -t num   number of threads      (default: 1)
  -c num   concurrent connections (default: 1)
  -k       keep alive             (default: no)
  -z pri   GNUTLS cipher priority (default: NORMAL)
  -h       show this help

example: httpress -n 10000 -c 100 -t 4 -k http://localhost:8080/index.html
}}}

== Dependencies

* Depends on [[http://software.schmorp.de/pkg/libev.html|libev]] library
* Depends on [[http://www.gnu.org/software/gnutls/|GnuTLS]] library (if compiled with SSL support)

== Building from source

# Prerequisite: [[http://software.schmorp.de/pkg/libev.html|libev 4 library]] (your distro might have it in repo; otherwise build from source)
# SSL prerequisite: [[http://www.gnu.org/software/gnutls/|GnuTLS 3.0 library]] (better build from source)
# Download [[https://bitbucket.org/yarosla/httpress/src|httpress source]]
# Run {{{make}}} or {{{make -f Makefile.nossl}}}
# Collect executable from bin subdirectory
