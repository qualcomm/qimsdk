# qimsdk-app-builder

The QIM SDK Python App Builder is a high-level Python SDK for building,
running, and managing multimedia and AI pipelines on Qualcomm platforms.

It provides a Python abstraction layer over GStreamer and the QIM SDK
GStreamer plugins.

The Python distribution is named `qimsdk-app-builder`, while the Python
import package remains `qimsdk`:

```python
import qimsdk
from qimsdk import Pipeline
```

## Install from a local wheel

Copy the wheel to the target device and install it with:

```bash
python3 -m pip install \
    qimsdk_app_builder-0.1.0-py3-none-any.whl
```

On target where PyGObject, PyYAML, GStreamer, and the QIM SDK
runtime are already provided by the platform image, dependency resolution
can be disabled:

```bash
python3 -m pip install --no-deps \
    qimsdk_app_builder-0.1.0-py3-none-any.whl
```

Using `--no-deps` means that the target image is responsible for providing
all required runtime dependencies.

If `pip` is not available but the Python installation includes
`ensurepip`, it can be installed with:

```bash
python3 -m ensurepip
```

## Install from a package index

If the package is published to PyPI or an internal Python package index,
install it with:

```bash
python3 -m pip install qimsdk-app-builder
```

On embedded targets, prefer the local-wheel installation procedure when
the platform dependencies are managed by the Yocto image.

## Build from source

From the `python/` directory, create a virtual environment and install the
build frontend:

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
├── qimsdk_app_builder-0.1.0-py3-none-any.whl
└── qimsdk_app_builder-0.1.0.tar.gz
```
