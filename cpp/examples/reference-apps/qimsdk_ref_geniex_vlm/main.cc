/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <qti/qimsdk.h>

using namespace qti;

namespace {

constexpr const char *kDefaultModel = "unsloth/Qwen3.5-2B-GGUF:Q4_0";
constexpr const char *kDefaultPrompt = "Describe this image.";

void run(const std::string &model, const std::string &prompt,
    const std::string &device) {
  std::size_t inference_count = 0;

  Element source("videotestsrc", "source");
  source.set("is-live", true).set("pattern", "ball");

  Element rate("videorate", "rate");
  rate.set("drop-only", true);

  Element encoder("jpegenc", "encoder");
  encoder.set("quality", 85);

  Element sample_queue("queue", "sample-queue");
  sample_queue.set("leaky", 2)
      .set("max-size-buffers", 1U)
      .set("max-size-bytes", 0U)
      .set("max-size-time", 0ULL);

  Element vlm("geniex", "vlm");
  vlm.set("model", model).set("prompt", prompt).set("device", device);

  AppSink sink("output");
  sink.set("sync", false);
  sink.set_buffer_consumer([&inference_count](Buffer buffer) {
    const uint8_t *data = static_cast<const Buffer &>(buffer).data();
    if (data == nullptr)
      return;

    ++inference_count;
    const uint64_t pts = buffer.pts();
    std::cout << "\n[Inference " << inference_count << " at video time ";
    if (pts != std::numeric_limits<uint64_t>::max())
      std::cout << std::fixed << std::setprecision(1)
                << static_cast<double>(pts) / 1'000'000'000.0 << 's';
    else
      std::cout << "unknown time";
    std::cout << "]\n";
    std::cout.write(reinterpret_cast<const char *>(data),
        static_cast<std::streamsize>(buffer.size()));
    std::cout << std::endl;
  });

  const auto source_format =
      VideoFilter().format("I420").resolution(640, 480).framerate(30);
  const auto inference_rate =
      VideoFilter().format("I420").resolution(640, 480).framerate(1, 10);
  const auto jpeg_format = ImageFilter().format("JPEG");

  Pipeline pipeline("geniex-vlm-video");
  pipeline.add(source)
      .add_stream_filter("source-format", source_format)
      .add(rate)
      .add_stream_filter("inference-rate", inference_rate)
      .add(encoder)
      .add_stream_filter("jpeg-format", jpeg_format)
      .add(sample_queue)
      .add(vlm)
      .add(sink);

  pipeline.start();
  std::cout << "Running GenieX VLM inference every 10 seconds. Press Ctrl+C to stop."
            << std::endl;
  pipeline.wait();
  pipeline.stop();

  if (inference_count == 0)
    throw std::runtime_error("Pipeline ended without a GenieX VLM response");
}

} // namespace

int main(int argc, char **argv) {
  std::string model = kDefaultModel;
  std::string prompt = kDefaultPrompt;
  std::string device = "auto";

  auto print_usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " [OPTIONS]\n\n"
        << "Options:\n"
        << "  --model ID       GenieX model ID (default: " << kDefaultModel << ")\n"
        << "  --prompt TEXT    Prompt sent with each image (default: \""
        << kDefaultPrompt << "\")\n"
        << "  --device MODE    Device alias/list (default: auto)\n"
        << "  -h, --help       Show this help message and exit\n";
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(std::cout);
      return 0;
    }
    if ((arg == "--model" || arg == "--prompt" || arg == "--device") &&
        i + 1 < argc) {
      const std::string value = argv[++i];
      if (arg == "--model")
        model = value;
      else if (arg == "--prompt")
        prompt = value;
      else
        device = value;
      continue;
    }

    std::cerr << "Error: unexpected or incomplete argument '" << arg
              << "'\n\n";
    print_usage(std::cerr);
    return 1;
  }

  SetImsdkGstLogMode(ImsdkGstLogMode::ImsdkLog);
  SetImsdkLogLevel(ImsdkLogLevel::Debug);

  try {
    run(model, prompt, device);
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
