CivetWeb 1.16, pinned to upstream tag `v1.16` (release commit `d7ba35b`).
Source: https://github.com/civetweb/civetweb/tree/v1.16

The embedded server source is kept in `src/` with its public header in
`civetweb.h`. This build enables IPv6 and disables TLS, CGI, WebSocket, zlib,
Lua and Duktape features that nativeTv does not use. It is built as a static
library for the daemon. The upstream license and credits are retained beside
the source; `LICENSE.md` is copied to runtime packages.
