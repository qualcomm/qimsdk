# GenieX GStreamer VLM

`geniex` accepts one JPEG image per buffer and emits the model's UTF-8 response.
Set `model` to a GenieX model ID; GenieX downloads uncached models. `prompt`
defaults to `Describe this image.`. The `device` property defaults to `auto` and
accepts `auto`, `cpu`, `gpu`, `npu`, `hybrid`, or a concrete GenieX device list
such as `HTP0,HTP1`. The selected mode is resolved for the model's backend
when the VLM is created; `npu` selects HTP for `llama_cpp` and NPU for QAIRT.

## Prerequisites

- CMake, a C/C++ compiler, `pkg-config`, and GStreamer development files for
  `gstreamer-1.0`.
- The GenieX SDK package (`pkg-geniex`) built for the same OS, architecture,
  libc, and ABI as this plugin.
- For inference, GenieX's backend runtime dependencies and the GStreamer
  elements `videotestsrc`, `videorate`, and `jpegenc`.

## 1. Download the prebuilt GenieX SDK

The GenieX release archive contains the installed `pkg-geniex`; cloning and
building GenieX from source is not needed. This downloads the latest stable
Linux ARM64 package and verifies its published SHA-256 checksum. Set
`GENIEX_VERSION` before running the block to pin a specific release.

```sh
GENIEX_BASE_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-geniex"
GENIEX_VERSION="${GENIEX_VERSION:-$(curl -fsSL "$GENIEX_BASE_URL/index.json" | python3 -c 'import json,sys; print(json.load(sys.stdin)["latest_stable"])')}"
GENIEX_ARCHIVE="geniex-sdk-linux-arm64-${GENIEX_VERSION}.zip"
GENIEX_CACHE_DIR="$HOME/.cache/geniex-sdk/$GENIEX_VERSION"

mkdir -p "$GENIEX_CACHE_DIR"
curl -fL "$GENIEX_BASE_URL/$GENIEX_ARCHIVE" \
  -o "$GENIEX_CACHE_DIR/$GENIEX_ARCHIVE"
curl -fL "$GENIEX_BASE_URL/$GENIEX_ARCHIVE.sha256" \
  -o "$GENIEX_CACHE_DIR/$GENIEX_ARCHIVE.sha256"
(cd "$GENIEX_CACHE_DIR" && sha256sum -c "$GENIEX_ARCHIVE.sha256")
unzip -q "$GENIEX_CACHE_DIR/$GENIEX_ARCHIVE" -d "$GENIEX_CACHE_DIR"

export GENIEX_ROOT="$GENIEX_CACHE_DIR/sdk-linux-arm64"
test -f "$GENIEX_ROOT/include/geniex.h"
```

A CPU-only release is also available as
`geniex-sdk-linux-arm64-cpu-<tag>.zip`; use the matching extracted
`sdk-linux-arm64-cpu` directory for that variant. The Linux ARM64 release is a
prebuilt AArch64 package, not an x86_64 host package. For QDC, verify that its
libc/sysroot ABI matches the board's Yocto release; if it does not, use a
package built for that exact target ABI.

## 2. Build the GStreamer plugin

From the QIMSDK repository root, with `GENIEX_ROOT` pointing at `pkg-geniex`:

```sh
cmake -S gstreamer -B build-geniex \
  -DENABLE_GST_PLUGIN_GENIEX=ON \
  -DGENIEX_ROOT="$GENIEX_ROOT"
cmake --build build-geniex --target gstgeniex --parallel
```

The plugin library is `build-geniex/gst-plugin-geniex/libgstgeniex.so`.

## 3. Configure runtime paths

From the QIMSDK repository root, set the GenieX runtime and GStreamer plugin
paths before launching an application:

```sh
export LD_LIBRARY_PATH="$GENIEX_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GST_PLUGIN_PATH="$PWD/build-geniex/gst-plugin-geniex${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
```

These are **runtime** paths, not only CMake settings. Keep them exported in the
same shell when running `gst-launch-1.0` or either reference app; re-export them
if you open a new terminal. If QIMSDK's GStreamer plugins are in a non-standard
location, include that plugin directory in `GST_PLUGIN_PATH` too.

Check that GStreamer finds the plugin:

```sh
gst-inspect-1.0 geniex
```

## 4. Run a one-frame inference

This sends one generated test frame to GenieX and writes the UTF-8 response to
`response.txt`:

```sh
gst-launch-1.0 -q \
  videotestsrc num-buffers=1 \
  ! video/x-raw,format=I420,width=640,height=480,framerate=30/1 \
  ! jpegenc \
  ! geniex model=unsloth/Qwen3.5-2B-GGUF:Q4_0 device=npu prompt="Describe this image." \
  ! filesink location=response.txt
cat response.txt
```

The first run downloads the model if it is not already in the GenieX model
cache.

## 5. Run the QIMSDK reference apps

Both examples generate a moving test pattern, sample one frame every 10 seconds,
and print the inference response and video timestamp. A one-frame leaky queue
keeps the newest sample if inference falls behind.

### Python

Use a Python environment with QIMSDK's Python source, PyGObject/GStreamer
introspection, and the required GStreamer plugins available:

```sh
PYTHONPATH="$PWD/python${PYTHONPATH:+:$PYTHONPATH}" \
  python3 python/examples/reference-apps/qimsdk_ref_geniex_vlm.py \
  --model unsloth/Qwen3.5-2B-GGUF:Q4_0 \
  --device npu \
  --prompt "Describe this image."
```

Press Ctrl+C to stop.

### C++

The C++ examples require QIMSDK's Qualcomm GStreamer development packages and
`yaml-cpp` in addition to the regular GStreamer development files. Source the
matching Qualcomm/Yocto SDK environment first; it normally sets the compiler,
sysroot, and pkg-config variables. If the `.pc` files are installed separately,
add their directories (the paths below are examples):

```sh
export PKG_CONFIG_PATH="/path/to/qti/sysroot/usr/lib/pkgconfig:/path/to/qti/sysroot/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --modversion gstreamer-qcom-oss-ml-1.0 \
  gstreamer-qcom-oss-video-1.0 yaml-cpp
cmake -S cpp -B build-cpp
cmake --build build-cpp --target qimsdk_ref_geniex_vlm --parallel
./build-cpp/examples/reference-apps/qimsdk_ref_geniex_vlm/qimsdk_ref_geniex_vlm \
  --model unsloth/Qwen3.5-2B-GGUF:Q4_0 \
  --device npu \
  --prompt "Describe this image."
```

Press Ctrl+C to stop.

### Backend and device examples

Use a cached or downloadable `llama_cpp` VLM such as
`unsloth/Qwen3.5-2B-GGUF:Q4_0` with `--device npu` (all language-model layers
are offloaded to HTP). For a QAIRT VLM bundle, use
`qualcomm/Qwen2.5-VL-7B-Instruct` with `--device npu`; its vision encoder is
part of the bundle and does not require a standalone `mmproj` file. The same
commands accept `--device cpu` for an explicitly CPU-only `llama_cpp` run
(`qairt` remains an NPU-only backend). `auto` uses the SDK's backend default.

## ARM64 / QDC

For QDC, obtain the Qualcomm Yocto SDK for the **specific board and BSP
release**, then source its environment setup script. Build or obtain a GenieX
`pkg-geniex` package using the same target sysroot/ABI; do not reuse the host
CPU-only package. The generic GenieX `arm64-linux-snapdragon` build is not
necessarily ABI-compatible with a Yocto image.

```sh
source /path/to/environment-setup-armv8a-qcom-linux
export GENIEX_ROOT=/path/to/yocto-compatible/pkg-geniex

cmake -S gstreamer -B build-geniex-qdc \
  -DENABLE_GST_PLUGIN_GENIEX=ON \
  -DGENIEX_ROOT="$GENIEX_ROOT"
cmake --build build-geniex-qdc --target gstgeniex --parallel
```

The QIMSDK C++ example additionally needs the QIMSDK/QTI development packages
and `yaml-cpp` in the Yocto SDK sysroot; make their `.pc` files visible to
`pkg-config` if the SDK environment does not do so automatically. Validate the
plugin and model inference on the target device; QEMU does not replace QDC
runtime/ABI validation.

GenieX logs are mapped to the `geniex` GStreamer category. Before GenieX
initialization the plugin reads that category's threshold and sets GenieX's
minimum level to the nearest supported severity. `GST_DEBUG=geniex:6` enables
DEBUG logs; `GST_DEBUG=geniex:7` also enables TRACE and may slow decoding.
