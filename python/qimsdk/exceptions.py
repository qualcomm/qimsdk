# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

class ImsdkError(RuntimeError):
    """Base exception type for IMSDK Python errors.
    """


class GstError(ImsdkError):
    """Exception raised for GStreamer-related failures.
    """


class PipelineError(ImsdkError):
    """Exception raised for pipeline construction, linking, or runtime failures.
    """
