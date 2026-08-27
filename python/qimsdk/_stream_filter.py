# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

from typing import Optional, Tuple, List
from enum import Enum, auto

from ._utils import gst


class StreamFilter:
    """Base class for objects that describe GStreamer caps used by capsfilter elements.
    """
    def __init__(self, caps: str) -> None:
        """Initializes the object.

        Args:
            caps: StreamFilter, GstCaps, or caps-compatible value. Type: str.

        Returns:
            None.
        """
        self._Gst = gst()
        self._cached_caps: object | None = self._Gst.Caps.from_string(caps)
        self._upstream_pad_hint = ""

    def _get_caps(self) -> object | None:
        """Returns the cached GstCaps object represented by this filter.

        Returns:
            object | None: Result of the operation.
        """
        return self._cached_caps

    def to_string(self) -> str:
        """Returns this filter as a GStreamer caps string.

        Returns:
            str: Result of the operation.
        """
        caps = self._get_caps()
        return caps.to_string() if caps else ""


class VideoFormat(str, Enum):
    # video/x-raw
    """String constants for video and image formats supported by the stream filters.
    """
    NV12 = "NV12"
    NV21 = "NV21"
    I420 = "I420"
    YV12 = "YV12"

    YUY2 = "YUY2"
    UYVY = "UYVY"
    VYUY = "VYUY"
    YVYU = "YVYU"

    NV16 = "NV16"
    NV61 = "NV61"
    Y42B = "Y42B"
    I422_10LE = "I422_10LE"
    I422_10BE = "I422_10BE"

    I420_10LE = "I420_10LE"
    I420_10BE = "I420_10BE"

    RGB = "RGB"
    BGR = "BGR"
    RGBx = "RGBx"
    xRGB = "xRGB"
    BGRx = "BGRx"
    xBGR = "xBGR"
    RGBA = "RGBA"
    ARGB = "ARGB"
    BGRA = "BGRA"
    ABGR = "ABGR"

    P010_10LE = "P010_10LE"
    P010_10BE = "P010_10BE"
    P016_LE = "P016_LE"
    P016_BE = "P016_BE"

    Y410 = "Y410"
    r210 = "r210"
    RGB10A2_LE = "RGB10A2_LE"
    BGR10A2_LE = "BGR10A2_LE"

    GRAY8 = "GRAY8"
    GRAY16_LE = "GRAY16_LE"
    GRAY16_BE = "GRAY16_BE"
    GRAY10_LE16 = "GRAY10_LE16"
    GRAY10_LE32 = "GRAY10_LE32"

    Y444 = "Y444"
    Y444_10LE = "Y444_10LE"
    Y444_10BE = "Y444_10BE"
    Y444_12LE = "Y444_12LE"
    Y444_12BE = "Y444_12BE"
    Y444_16LE = "Y444_16LE"
    Y444_16BE = "Y444_16BE"

    I422_12LE = "I422_12LE"
    I422_12BE = "I422_12BE"
    I420_12LE = "I420_12LE"
    I420_12BE = "I420_12BE"

    RGBA64_LE = "RGBA64_LE"
    ARGB64_LE = "ARGB64_LE"
    BGRA64_LE = "BGRA64_LE"
    ABGR64_LE = "ABGR64_LE"

    RGBA64_BE = "RGBA64_BE"
    ARGB64_BE = "ARGB64_BE"
    BGRA64_BE = "BGRA64_BE"
    ABGR64_BE = "ABGR64_BE"

    UYVP = "UYVP"
    v210 = "v210"
    v216 = "v216"
    v308 = "v308"
    NV24 = "NV24"

    NV12_10LE32 = "NV12_10LE32"
    NV12_10LE40 = "NV12_10LE40"
    NV16_10LE32 = "NV16_10LE32"
    NV16_10LE40 = "NV16_10LE40"

    # image/jpeg
    JPEG = "JPEG"

    # video/x-bayer
    BGGR = "BGGR"
    RGGB = "RGGB"
    GBRG = "GBRG"
    GRBG = "GRBG"
    MONO = "MONO"

    @property
    def is_jpeg(self) -> bool:
        """Returns whether the format string represents JPEG.

        Returns:
            bool: Result of the operation.
        """
        return self is VideoFormat.JPEG

    @property
    def is_bayer(self) -> bool:
        """Returns whether the format string represents a Bayer format.

        Returns:
            bool: Result of the operation.
        """
        return self in {
            VideoFormat.BGGR,
            VideoFormat.RGGB,
            VideoFormat.GBRG,
            VideoFormat.GRBG,
            VideoFormat.MONO,
        }


class VideoFilter(StreamFilter):
    """Builds video or image caps for raw, Bayer, and JPEG video streams.
    """
    class Kind(Enum):
        """Kind defines an SDK type used by the IMSDK Python package.
        """
        RAW = auto()
        BAYER = auto()
        JPEG = auto()

    def __init__(self) -> None:
        """Initializes the object.

        Returns:
            None.
        """
        super().__init__("")

        self._dirty = True
        self._extra_kv: List[Tuple[str, str]] = []

        self._kind = self.Kind.RAW
        self._format: Optional[str] = "NV12"
        self._width: Optional[int] = None
        self._height: Optional[int] = None
        self._framerate: Optional[Tuple[int, int]] = None
        self._colorimetry: Optional[str] = None
        self._range: Optional[str] = None
        self._interlace: Optional[str] = None
        self._par: Optional[Tuple[int, int]] = None

    def add(self, expr: str) -> "VideoFilter":
        """Adds an element, caps field, or property depending on the receiver type.

        Args:
            expr: Expr value. Type: str.

        Returns:
            "VideoFilter": Result of the operation.
        """
        key, value = (part.strip() for part in expr.split("=", 1))
        if not key:
            raise RuntimeError("VideoFilter.add: empty key")
        self._extra_kv.append((key, value))
        self._dirty = True
        return self

    def format(self, fmt: str | VideoFormat) -> "VideoFilter":
        """Sets the stream format value.

        Args:
            fmt: Format string. Type: str | VideoFormat.

        Returns:
            "VideoFilter": Result of the operation.
        """
        if isinstance(fmt, VideoFormat):
            f = fmt
        elif isinstance(fmt, str):
            try:
                f = VideoFormat(fmt)
            except ValueError:
                f = None
        else:
            f = None

        if isinstance(f, VideoFormat) and f.is_jpeg:
            self._kind = self.Kind.JPEG
            self._format = None

        elif isinstance(f, VideoFormat) and f.is_bayer:
            self._kind = self.Kind.BAYER
            self._format = f.value

        else:
            self._kind = self.Kind.RAW
            self._format = f.value if isinstance(f, VideoFormat) else fmt

        self._dirty = True
        return self

    def resolution(self, w: int, h: int) -> "VideoFilter":
        """Sets width and height constraints.

        Args:
            w: Width in pixels. Type: int.
            h: Height in pixels. Type: int.

        Returns:
            "VideoFilter": Result of the operation.
        """
        self._width = w
        self._height = h
        self._dirty = True
        return self

    def framerate(self, num: int | float, den: int = 1) -> "VideoFilter":
        """Sets framerate as an integer fraction or floating-point value.

        Args:
            num: Framerate numerator or floating-point framerate. Type: int | float.
            den: Framerate denominator. Type: int.

        Returns:
            "VideoFilter": Result of the operation.
        """
        self._framerate = (
            (round(num * 1000), 1000)
            if isinstance(num, float)
            else (num, den or 1)
        )
        self._dirty = True
        return self

    def colorimetry(self, value: str) -> "VideoFilter":
        """Sets the raw-video colorimetry caps field.

        Args:
            value: Value to assign. Type: str.

        Returns:
            "VideoFilter": Result of the operation.
        """
        self._colorimetry = value
        self._dirty = True
        return self

    def range(self, value: str) -> "VideoFilter":
        """Sets the raw-video color range caps field.

        Args:
            value: Value to assign. Type: str.

        Returns:
            "VideoFilter": Result of the operation.
        """
        self._range = value
        self._dirty = True
        return self

    def interlace(self, mode: str) -> "VideoFilter":
        """Sets the raw-video interlace-mode caps field.

        Args:
            mode: Capture mode or logging mode. Type: str.

        Returns:
            "VideoFilter": Result of the operation.
        """
        self._interlace = mode
        self._dirty = True
        return self

    def pixel_aspect_ratio(self, num: int, den: int = 1) -> "VideoFilter":
        """Sets the raw-video pixel-aspect-ratio caps field.

        Args:
            num: Framerate numerator or floating-point framerate. Type: int.
            den: Framerate denominator. Type: int.

        Returns:
            "VideoFilter": Result of the operation.
        """
        self._par = (num, den or 1)
        self._dirty = True
        return self

    def _build_caps(self):
        """Builds the GstCaps object represented by this filter.
        """
        Gst = self._Gst

        if self._kind is self.Kind.RAW:
            caps = Gst.Caps.new_empty_simple("video/x-raw")
        elif self._kind is self.Kind.BAYER:
            caps = Gst.Caps.new_empty_simple("video/x-bayer")
        elif self._kind is self.Kind.JPEG:
            caps = Gst.Caps.new_empty_simple("image/jpeg")
        else:
            return None

        if self._kind is self.Kind.RAW and self._format:
            caps.set_value("format", self._format)
        elif self._kind is self.Kind.BAYER and self._format:
            caps.set_value("format", self._format.lower())

        if self._width is not None and self._height is not None:
            caps.set_value("width", self._width)
            caps.set_value("height", self._height)

        if self._framerate:
            n, d = self._framerate
            caps.set_value("framerate", Gst.Fraction(n, d))

        if self._kind is self.Kind.RAW:
            if self._colorimetry:
                caps.set_value("colorimetry", self._colorimetry)
            if self._range:
                caps.set_value("range", self._range)
            if self._interlace:
                caps.set_value("interlace-mode", self._interlace)
            if self._par:
                n, d = self._par
                caps.set_value("pixel-aspect-ratio", Gst.Fraction(n, d))

        for k, v in self._extra_kv:
            caps.set_value(k, v)

        return caps

    def _get_caps(self):
        """Returns the cached GstCaps object represented by this filter.
        """
        if self._dirty or self._cached_caps is None:
            self._cached_caps = self._build_caps()
            self._dirty = False
        return self._cached_caps


class ImageFilter(StreamFilter):
    """Builds caps for image streams such as JPEG or Bayer frames.
    """
    class Kind(Enum):
        """Kind defines an SDK type used by the IMSDK Python package.
        """
        RAW = auto()
        BAYER = auto()
        JPEG = auto()

    def __init__(self) -> None:
        """Initializes the object.

        Returns:
            None.
        """
        super().__init__("")

        self._upstream_pad_hint = "image_%u"
        self._dirty = True
        self._extra_kv: List[Tuple[str, str]] = []

        self._kind = self.Kind.JPEG
        self._format: Optional[str] = None
        self._width: Optional[int] = None
        self._height: Optional[int] = None
        self._framerate: Optional[Tuple[int, int]] = None

    def add(self, expr: str) -> "ImageFilter":
        """Adds an element, caps field, or property depending on the receiver type.

        Args:
            expr: Expr value. Type: str.

        Returns:
            "ImageFilter": Result of the operation.
        """
        key, value = (part.strip() for part in expr.split("=", 1))
        if not key:
            raise RuntimeError("ImageFilter.add: empty key")
        self._extra_kv.append((key, value))
        self._dirty = True
        return self

    def format(self, fmt: str | VideoFormat) -> "ImageFilter":
        """Sets the stream format value.

        Args:
            fmt: Format string. Type: str | VideoFormat.

        Returns:
            "ImageFilter": Result of the operation.
        """
        if isinstance(fmt, VideoFormat):
            f = fmt
        elif isinstance(fmt, str):
            try:
                f = VideoFormat(fmt)
            except ValueError:
                f = None
        else:
            f = None

        if isinstance(f, VideoFormat) and f.is_jpeg:
            self._kind = self.Kind.JPEG
            self._format = None

        elif isinstance(f, VideoFormat) and f.is_bayer:
            self._kind = self.Kind.BAYER
            self._format = f.value

        else:
            self._kind = self.Kind.RAW
            self._format = f.value if isinstance(f, VideoFormat) else fmt

        self._dirty = True
        return self

    def resolution(self, w: int, h: int) -> "ImageFilter":
        """Sets width and height constraints.

        Args:
            w: Width in pixels. Type: int.
            h: Height in pixels. Type: int.

        Returns:
            "ImageFilter": Result of the operation.
        """
        self._width = w
        self._height = h
        self._dirty = True
        return self

    def framerate(self, num: int | float, den: int = 1) -> "ImageFilter":
        """Sets framerate as an integer fraction or floating-point value.

        Args:
            num: Framerate numerator or floating-point framerate. Type: int | float.
            den: Framerate denominator. Type: int.

        Returns:
            "ImageFilter": Result of the operation.
        """
        self._framerate = (
            (round(num * 1000), 1000)
            if isinstance(num, float)
            else (num, den or 1)
        )
        self._dirty = True
        return self

    def _build_caps(self):
        """Builds the GstCaps object represented by this filter.
        """
        Gst = self._Gst

        if self._kind is self.Kind.JPEG:
            caps = Gst.Caps.new_empty_simple("image/jpeg")
        elif self._kind is self.Kind.BAYER:
            caps = Gst.Caps.new_empty_simple("video/x-bayer")
            if self._format:
                caps.set_value("format", self._format.lower())
        else:
            caps = Gst.Caps.new_empty_simple("video/x-raw")
            if self._format:
                caps.set_value("format", self._format)

        if self._width and self._height:
            caps.set_value("width", self._width)
            caps.set_value("height", self._height)

        if self._framerate:
            n, d = self._framerate
            caps.set_value("framerate", Gst.Fraction(n, d))

        for k, v in self._extra_kv:
            caps.set_value(k, v)

        return caps

    def _get_caps(self):
        """Returns the cached GstCaps object represented by this filter.
        """
        if self._dirty or self._cached_caps is None:
            self._cached_caps = self._build_caps()
            self._dirty = False
        return self._cached_caps


class H264Filter(StreamFilter):
    """Builds H.264 caps including resolution, framerate, profile, stream-format, alignment, and codec data.
    """
    def __init__(self) -> None:
        """Initializes the object.

        Returns:
            None.
        """
        super().__init__("")
        self._cached_caps: object | None = None
        self._dirty = True
        self._extra_kv: List[Tuple[str, str]] = []

        self._profile: Optional[str] = None
        self._level: Optional[str] = None
        self._stream_format: Optional[str] = None
        self._alignment: Optional[str] = None
        self._codec_data: Optional[str] = None
        self._width: Optional[int] = None
        self._height: Optional[int] = None
        self._framerate: Optional[Tuple[int, int]] = None
        self._dirty = True

    def add(self, expr: str) -> H264Filter:
        """Adds an element, caps field, or property depending on the receiver type.

        Args:
            expr: Expr value. Type: str.

        Returns:
            H264Filter: Result of the operation.
        """
        try:
            key, value = (part.strip() for part in expr.split("=", 1))
        except ValueError:
            raise RuntimeError("H264Filter.add expects 'key=value'")

        if not key:
            raise RuntimeError("H264Filter.add: empty key")

        self._extra_kv.append((key, value))
        self._dirty = True
        return self

    def resolution(self, w: int, h: int) -> H264Filter:
        """Sets width and height constraints.

        Args:
            w: Width in pixels. Type: int.
            h: Height in pixels. Type: int.

        Returns:
            H264Filter: Result of the operation.
        """
        self._width = w
        self._height = h
        self._dirty = True
        return self

    def framerate(self, num: int | float, den: int = 1) -> H264Filter:
        """Sets framerate as an integer fraction or floating-point value.

        Args:
            num: Framerate numerator or floating-point framerate. Type: int | float.
            den: Framerate denominator. Type: int.

        Returns:
            H264Filter: Result of the operation.
        """
        if isinstance(num, float):
            self._framerate = (round(num * 1000), 1000)
        else:
            self._framerate = (num, den or 1)
        self._dirty = True
        return self

    def profile(self, profile: str) -> H264Filter:
        """Sets the H.264 profile caps field.

        Args:
            profile: H.264 profile value. Type: str.

        Returns:
            H264Filter: Result of the operation.
        """
        self._profile = profile
        self._dirty = True
        return self

    def level(self, level: str) -> H264Filter:
        """Sets the H.264 level caps field.

        Args:
            level: IMSDK or GStreamer log level. Type: str.

        Returns:
            H264Filter: Result of the operation.
        """
        self._level = level
        self._dirty = True
        return self

    def stream_format(self, fmt: str) -> H264Filter:
        """Sets the H.264 stream-format caps field.

        Args:
            fmt: Format string. Type: str.

        Returns:
            H264Filter: Result of the operation.
        """
        self._stream_format = fmt
        self._dirty = True
        return self

    def alignment(self, value: str) -> H264Filter:
        """Sets the H.264 alignment caps field.

        Args:
            value: Value to assign. Type: str.

        Returns:
            H264Filter: Result of the operation.
        """
        self._alignment = value
        self._dirty = True
        return self

    def codec_data(self, value: str) -> H264Filter:
        """Sets the H.264 codec_data caps field.

        Args:
            value: Value to assign. Type: str.

        Returns:
            H264Filter: Result of the operation.
        """
        self._codec_data = value
        self._dirty = True
        return self

    def set(self, key: str, value: str) -> H264Filter:
        """Sets GObject properties and returns the wrapper for fluent chaining.

        Args:
            key: Configuration or caps field key. Type: str.
            value: Value to assign. Type: str.

        Returns:
            H264Filter: Result of the operation.
        """
        return self.add(f"{key}={value}")

    def _build_caps(self):
        """Builds the GstCaps object represented by this filter.
        """
        Gst = self._Gst

        caps = Gst.Caps.new_empty_simple("video/x-h264")

        if self._width is not None and self._height is not None:
            caps.set_value("width", int(self._width))
            caps.set_value("height", int(self._height))

        if self._framerate:
            n, d = self._framerate
            caps.set_value("framerate", Gst.Fraction(n, d))

        if self._profile:
            caps.set_value("profile", self._profile)

        if self._level:
            caps.set_value("level", self._level)

        if self._stream_format:
            caps.set_value("stream-format", self._stream_format)

        if self._alignment:
            caps.set_value("alignment", self._alignment)

        if self._codec_data:
            caps.set_value("codec_data", self._codec_data)

        for k, v in self._extra_kv:
            caps.set_value(k, v)

        return caps

    def _get_caps(self) -> object | None:
        """Returns the cached GstCaps object represented by this filter.

        Returns:
            object | None: Result of the operation.
        """
        if self._dirty or self._cached_caps is None:
            self._cached_caps = self._build_caps()
            self._dirty = False

        return self._cached_caps


class TensorFilter(StreamFilter):
    """Builds neural-network tensor caps including tensor type and dimensions.
    """
    class Type(Enum):
        """Type defines an SDK type used by the IMSDK Python package.
        """
        UINT8 = auto()
        UINT16 = auto()
        UINT32 = auto()
        INT8 = auto()
        INT16 = auto()
        INT32 = auto()
        FLOAT16 = auto()
        FLOAT32 = auto()

    def __init__(self) -> None:
        """Initializes the object.

        Returns:
            None.
        """
        super().__init__("")
        self._cached_caps = self._Gst.Caps.new_empty_simple(
            "neural-network/tensors"
        )
        self._dirty = False

    def add(self, expr: str) -> "TensorFilter":
        """Adds an element, caps field, or property depending on the receiver type.

        Args:
            expr: Expr value. Type: str.

        Returns:
            "TensorFilter": Result of the operation.
        """
        try:
            key, value = (part.strip() for part in expr.split("=", 1))
        except ValueError:
            raise RuntimeError("TensorFilter.add expects 'key=value'")

        if not key:
            raise RuntimeError("TensorFilter.add: empty key")

        self._cached_caps.set_value(key, value)
        return self

    def type(self, t) -> "TensorFilter":
        """Sets the tensor element type.

        Args:
            t: Tensor type value.

        Returns:
            "TensorFilter": Result of the operation.
        """
        if isinstance(t, self.Type):
            t = t.name
        self._cached_caps.set_value("type", str(t))
        return self

    def dimensions(self, *dims) -> "TensorFilter":
        """Sets tensor dimensions for one or more tensors.

        Args:
            dims: Tensor dimensions.

        Returns:
            "TensorFilter": Result of the operation.
        """
        if len(dims) == 1 and isinstance(dims[0], (list, tuple)):
            value = dims[0]
        else:
            value = list(dims)

        if value and all(isinstance(x, int) for x in value):
            many = [list(map(int, value))]
        elif value and all(isinstance(x, (list, tuple)) for x in value):
            many = [list(map(int, inner)) for inner in value]
        else:
            raise RuntimeError(
                "TensorFilter.dimensions expects ints, a list[int], or list[list[int]]"
            )

        Gst = self._Gst

        # nested ValueArray: [[1, 520, 520, 3], ...]
        outer = Gst.ValueArray(
            [Gst.ValueArray(inner_dims) for inner_dims in many]
        )

        self._cached_caps.set_value("dimensions", outer)
        return self


class TextFilter(StreamFilter):
    """Builds caps for raw text streams.
    """

    def __init__(self) -> None:
        """Initializes the object.

        Returns:
            None.
        """
        super().__init__("")
        self._cached_caps = self._Gst.Caps.new_empty_simple("text/x-raw")
        self._dirty = False

    def add(self, expr: str) -> "TextFilter":
        """Adds an element, caps field, or property depending on the receiver type.

        Args:
            expr: Expr value. Type: str.

        Returns:
            "TextFilter": Result of the operation.
        """
        try:
            key, value = (part.strip() for part in expr.split("=", 1))
        except ValueError:
            raise RuntimeError("TextFilter.add expects 'key=value'")

        if not key:
            raise RuntimeError("TextFilter.add: empty key")

        self._cached_caps.set_value(key, value)
        return self


class AudioFormat(str, Enum):
    """String constants for audio sample formats supported by AudioFilter.
    """
    S8 = "S8"
    U8 = "U8"

    S16LE = "S16LE"
    S16BE = "S16BE"
    U16LE = "U16LE"
    U16BE = "U16BE"

    S24_32LE = "S24_32LE"
    S24_32BE = "S24_32BE"
    U24_32LE = "U24_32LE"
    U24_32BE = "U24_32BE"

    S32LE = "S32LE"
    S32BE = "S32BE"
    U32LE = "U32LE"
    U32BE = "U32BE"

    S24LE = "S24LE"
    S24BE = "S24BE"
    U24LE = "U24LE"
    U24BE = "U24BE"

    S20LE = "S20LE"
    S20BE = "S20BE"
    U20LE = "U20LE"
    U20BE = "U20BE"

    S18LE = "S18LE"
    S18BE = "S18BE"
    U18LE = "U18LE"
    U18BE = "U18BE"

    F32LE = "F32LE"
    F32BE = "F32BE"
    F64LE = "F64LE"
    F64BE = "F64BE"


class AudioLayout(str, Enum):
    """String constants for audio channel layouts supported by AudioFilter.
    """
    INTERLEAVED = "interleaved"
    NON_INTERLEAVED = "non-interleaved"


class AudioFilter(StreamFilter):
    """Builds raw audio caps with format, channel count, rate, and layout fields.
    """
    def __init__(self) -> None:
        """Initializes the object.

        Returns:
            None.
        """
        super().__init__("")

        self._cached_caps = self._Gst.Caps.new_empty_simple("audio/x-raw")
        self._dirty = False

    def add(self, expr: str) -> "AudioFilter":
        """Adds an element, caps field, or property depending on the receiver type.

        Args:
            expr: Expr value. Type: str.

        Returns:
            "AudioFilter": Result of the operation.
        """
        try:
            key, value = (part.strip() for part in expr.split("=", 1))
        except ValueError:
            raise RuntimeError("AudioFilter.add expects 'key=value'")

        if not key:
            raise RuntimeError("AudioFilter.add: empty key")

        self._cached_caps.set_value(key, value)
        return self

    def format(self, fmt: str | AudioFormat) -> "AudioFilter":
        """Sets the stream format value.

        Args:
            fmt: Format string. Type: str | AudioFormat.

        Returns:
            "AudioFilter": Result of the operation.
        """
        value = fmt.value if isinstance(fmt, AudioFormat) else fmt
        self._cached_caps.set_value("format", value)
        return self

    def channels(self, n: int) -> "AudioFilter":
        """Sets the audio channel count.

        Args:
            n: Numerator value. Type: int.

        Returns:
            "AudioFilter": Result of the operation.
        """
        self._cached_caps.set_value("channels", int(n))
        return self

    def rate(self, hz: int) -> "AudioFilter":
        """Sets the audio sampling rate.

        Args:
            hz: Hz value. Type: int.

        Returns:
            "AudioFilter": Result of the operation.
        """
        self._cached_caps.set_value("rate", int(hz))
        return self

    def layout(self, layout: str | AudioLayout) -> "AudioFilter":
        """Sets the audio layout.

        Args:
            layout: Layout value. Type: str | AudioLayout.

        Returns:
            "AudioFilter": Result of the operation.
        """
        value = layout.value if isinstance(layout, AudioLayout) else layout
        self._cached_caps.set_value("layout", value)
        return self
