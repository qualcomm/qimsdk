# qimsdk-app-builder-examples

Runnable demo and reference applications for `qimsdk-app-builder`, the
Qualcomm IM SDK Python App Builder.

This distribution contains standalone Python scripts. It does not provide
an importable `qimsdk_app_builder_examples` Python package.

The scripts use the public API provided by:

```text
qimsdk-app-builder
```

## Install from a local wheel

First install the matching core package:

```bash
python3 -m pip install \
    qimsdk_app_builder-0.1.0-py3-none-any.whl
```

Then install the examples package:

```bash
python3 -m pip install \
    qimsdk_app_builder_examples-0.1.0-py3-none-any.whl
```

## Install from a package index

If both packages are published to PyPI or an internal Python package index,
install the examples package with:

```bash
python3 -m pip install qimsdk-app-builder-examples
```

This installs the matching `qimsdk-app-builder` distribution and resolves
the additional Python dependencies declared by the examples package.

## Run

The scripts are installed into the Python environment's executable
directory under their existing filenames.

Examples:

```bash
qimsdk_ref_camera.py
qimsdk_ref_usb_camera.py
qimsdk_demo_car_game.py
qimsdk_demo_smartboard.py
```

The exact executable directory depends on the Python installation. On the
supported target image, the scripts are normally installed under:

```text
/usr/bin/
```

List the installed example files with:

```bash
python3 -m pip show -f qimsdk-app-builder-examples
```

## Build from source

From the `python/examples/` directory, create a virtual environment and
install the build frontend:

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
├── qimsdk_app_builder_examples-0.1.0-py3-none-any.whl
└── qimsdk_app_builder_examples-0.1.0.tar.gz
```
