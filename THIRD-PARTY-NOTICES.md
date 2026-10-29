# Third-party notices

nativeTv is MIT licensed ([LICENSE](LICENSE)). It includes, links to or was
built from the components below. Each keeps its own licence; the text sits at
the path given, or is the licence named by its SPDX id at the address given.

## Source and data kept in this repository

| Component | Used for | Licence | Licence text |
| :-------- | :------- | :------ | :----------- |
| [CivetWeb](https://github.com/civetweb/civetweb) 1.16 | The daemon's HTTP server, built in | MIT | [native-tv/third_party/civetweb/LICENSE.md](native-tv/third_party/civetweb/LICENSE.md); credits in `CREDITS.md` beside it; copied into runtime packages as `LICENSE-civetweb.txt` |
| [NanoSVG](https://github.com/memononen/nanosvg) | Parsing and rasterising SVG icons in the TV client | zlib | [native-tv/third_party/nanosvg/LICENSE.txt](native-tv/third_party/nanosvg/LICENSE.txt) |
| [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) (Project Nayuki) | The QR codes on the TV | MIT | Header of each file in [native-tv/third_party/qrcodegen/](native-tv/third_party/qrcodegen/) |
| [StevenBlack/hosts](https://github.com/StevenBlack/hosts) | The advertising and measurement host entries of the on-TV blocker, reviewed against it | MIT | [native-tv/third_party/adblock-domains/SOURCE.md](native-tv/third_party/adblock-domains/SOURCE.md) |
| [Roboto](https://github.com/googlefonts/roboto-3-classic) | Text of the dashboards and TV pages, cut to Latin as WOFF | Apache-2.0 | [native-tv/assets/fonts/LICENSE-Roboto.txt](native-tv/assets/fonts/LICENSE-Roboto.txt) |
| [Figtree](https://github.com/erikdkennedy/figtree) | Text of the native TV client | OFL-1.1 | [native-tv/assets/fonts/LICENSE-Figtree-OFL.txt](native-tv/assets/fonts/LICENSE-Figtree-OFL.txt) |
| [Material Design Icons](https://pictogrammers.com/library/mdi/) (`@mdi/js`) | The TV client's icons in `native-tv/assets/icons/material/` | Apache-2.0 | [native-tv/assets/icons/material/LICENSE-MaterialDesignIcons.txt](native-tv/assets/icons/material/LICENSE-MaterialDesignIcons.txt) |
| [Material Symbols](https://fonts.google.com/icons) (Google), Rounded | The icon sprite in `native-tv/web/ui.html` | Apache-2.0 | [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0), the text in the two Apache-2.0 files above |

## Linked from the webOSbrew SDK

The TV builds link these from the [webOSbrew native toolchain](https://github.com/webosbrew/native-toolchain)
and ship the notice with the runtime package (`native-tv/tools/runtime_payload.py`;
the sources of the notices are in [native-tv/third_party/sdk-runtime-notices/SOURCE.md](native-tv/third_party/sdk-runtime-notices/SOURCE.md)).

| Component | Used for | Licence |
| :-------- | :------- | :------ |
| [json-c](https://github.com/json-c/json-c) 0.16, linked statically | JSON | MIT; the notice is [native-tv/assets/LICENSE-json-c.txt](native-tv/assets/LICENSE-json-c.txt) |
| [curl](https://curl.se/) 7.87.0 (`libcurl.so.4`) | MQTT over TLS, downloads | curl ([COPYING](https://raw.githubusercontent.com/curl/curl/curl-7_87_0/COPYING)) |
| [OpenSSL](https://www.openssl.org/) 1.1.1q | TLS for libcurl | OpenSSL and SSLeay ([LICENSE](https://raw.githubusercontent.com/openssl/openssl/OpenSSL_1_1_1q/LICENSE)) |
| GCC runtime (`libatomic`) 12.2.0 | Atomic operations on the TV's ARM CPU | GPL-3.0-or-later with the GCC Runtime Library Exception ([COPYING.RUNTIME](https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-12.2.0/COPYING.RUNTIME)) |

## Used from the TV or the build machine, not shipped

SDL2, SDL2_image and SDL2_ttf (zlib), zlib (zlib) and OpenGL ES are system
libraries of the TV or of the computer that builds; nothing of them is copied
into a package. MkDocs, Material for MkDocs and the other tools in
`.github/workflows/` build the documentation and run the checks only.

## Design reference

The TV pages and the TV client take their sizes, colours and type scale from
Google's TV Design Kit. No file of the kit is included; the values were
re-created in `native-tv/design/` and `native-tv/assets/google-tv/`. Google,
Android TV, Google TV and Material Design are trademarks of Google LLC. LG and
webOS are trademarks of LG Electronics. nativeTv is not affiliated with either.

The project the code descends from is credited in the [README](README.md#credits).
