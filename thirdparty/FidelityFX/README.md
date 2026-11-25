# FidelityFX FSR 3.1 Upscaler Subset

The FSR 3.1 upscaler's host code and the shaders of the passes Sparkle dispatches, copied from [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) commit `7539ae8b6568ad29ed78a753404351daf3b5cca7` (`Kits/FidelityFX`), with its directory layout. Every AMD file carries AMD's MIT license ([LICENSE.txt](LICENSE.txt)); the files under `sparkle/` and the replaced `ffx_backends.h` are Sparkle's. The SDK is not a submodule because it ships hundreds of megabytes of samples and signed binaries.

Local changes, each marked `sparkle patch` or `sparkle replacement`:

* `api/include/ffx_api.h`: `FFX_API_ENTRY` is `__declspec(dllexport)` only on Windows.
* `upscalers/fsr3/include/ffx_fsr3upscaler.h`: the opaque context is twice the SDK default, because `wchar_t` is 4 bytes outside Windows and the private context holds wide names.
* `api/internal/ffx_backends.h`: replaced by the one declaration the upscaler needs; the SDK's backend registry is not included.
* `sparkle/git_hash_branch.h`: stands in for the header FidelityFX's build generates.
* `sparkle/ffx_compat.h`: force-included into the host sources; supplies `wcscpy_s`, `_countof` and `FFX_UNUSED` outside the MSVC runtime.

To update, copy the same files from a newer SDK commit and reapply the changes. `ffx_fsr3upscaler_shaderblobs.cpp` stays out: Sparkle compiles the shaders itself and defines the blob lookup.
