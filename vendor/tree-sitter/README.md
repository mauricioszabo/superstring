# Vendored Tree-sitter

`lib/` is the C library from Pulsar's fork of Tree-sitter,
https://github.com/pulsar-edit/tree-sitter, branch `v0-25-3-modified`: the same
source the web-tree-sitter build in Pulsar's `vendor/web-tree-sitter` comes
from, so both language modes parse and query alike.

It is taken at commit `0b282266`, the parent of the fork's only Pulsar commit
(`9a62ed70`, which adds `isalnum`/`isxdigit` to `stdlib-symbols.txt`). That
commit didn't regenerate `wasm/wasm-stdlib.h`, the stdlib embedded in the
native library, so with it the wasm store fails to start ("missing exported
symbol in wasm stdlib"). No bundled grammar needs those symbols.

It is built with `TREE_SITTER_FEATURE_WASM` against the Wasmtime C API
**29.0.1** (the version Tree-sitter 0.25 pins), which `script/fetch-wasmtime.js`
downloads into `vendor/wasmtime/`.

## Local patches

- `lib/src/wasm_store.c` (`pulsar-wasm-store.patch`): load grammars built with
  older toolchains:
  - legacy `dylink` custom sections, as well as `dylink.0`;
  - `GOT.func.*` / `GOT.mem.*` imports, resolved against the grammar
    module's own exports.
