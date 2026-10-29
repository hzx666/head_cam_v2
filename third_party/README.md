# Third-party components

- `cjson/`: cJSON v1.7.19, https://github.com/DaveGamble/cJSON/tree/v1.7.19 (MIT; see `cjson/LICENSE`). Compiled into the service.
- OpenSSL libcrypto: Ubuntu 22.04 ARM64 development package, Apache-2.0. Built-in AES/SHA/RNG only; external provider configuration is disabled.
- LZ4: Ubuntu ARM64 development package, BSD-2-Clause library.
- Zstandard: Ubuntu ARM64 development package, BSD-3-Clause library.
- glibc: cross compiler C runtime, LGPL-2.1-or-later with applicable exceptions.
- ossutil: `ossutil/ossutilarm64`, v1.7.19, https://github.com/aliyun/ossutil. Embedded unchanged; Apache-2.0 and upstream dependency notices apply. This is a build input required to produce the single-file service.
- CA bundle: selected build-time CA_BUNDLE, default Ubuntu ca-certificates bundle.

Package copyright/license files are preserved in `.deps/arm64/usr/share/doc/`. Package versions and SHA256 are recorded in `dist/build-info.txt`. Build-only dependencies and generated credential inputs are not source-controlled.
