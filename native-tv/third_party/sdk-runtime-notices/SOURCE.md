# SDK runtime license source records

Some webOSbrew SDK archives omit license text from the target sysroot. The
runtime packager first uses notices installed in the SDK. When those notices
are absent, it accepts only the package versions below, fetches the upstream
license text over HTTPS, verifies the SHA-256 digest, and caches the verified
text under the ignored build directory. Unknown versions or changed source
content fail closed and require a reviewed update here and in
`tools/runtime_payload.py`.

| Runtime package | SDK version | Upstream notice | SHA-256 |
| --- | --- | --- | --- |
| curl | 7.87.0 | <https://raw.githubusercontent.com/curl/curl/curl-7_87_0/COPYING> | `321b1a09ebc30410f2e837c072e5521cf7095b757193af4a7dae1086e36ed31a` |
| OpenSSL | 1.1.1q | <https://raw.githubusercontent.com/openssl/openssl/OpenSSL_1_1_1q/LICENSE> | `c32913b33252e71190af2066f08115c69bc9fddadf3bf29296e20c835389841c` |
| GCC runtime (`libatomic`) | 12.2.0 | <https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-12.2.0/COPYING.RUNTIME> | `9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74` |
