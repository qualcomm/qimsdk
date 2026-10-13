# qimsdk-gst-python-examples

Runnable `gst-python` example applications for the Qualcomm IM SDK
GStreamer plugins.

This distribution contains standalone Python scripts. It does not provide
an importable `qimsdk_gst_python_examples` Python package.

## Install from a local wheel

```bash
python3 -m pip install \
    qimsdk_gst_python_examples-0.1.0-py3-none-any.whl
```

## Install from a package index

If the package is published to PyPI or an internal Python package index,
install it with:

```bash
python3 -m pip install qimsdk-gst-python-examples
```

## Run

The scripts are installed into the Python environment's executable
directory under their existing filenames.

Examples:

```bash
gst-ai-object-detection.py
gst-camera-encode.py
gst-ai-video-inference.py
```

## Build from source

From the `gstreamer/gst-python-examples/` directory, create a virtual
environment and install the build frontend:

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install --upgrade build
```

Build the wheel and source distribution:

```bash
python3 -m build
```

The generated artifacts are placed in `dist/`:

```text
dist/
├── qimsdk_gst_python_examples-0.1.0-py3-none-any.whl
└── qimsdk_gst_python_examples-0.1.0.tar.gz
```
