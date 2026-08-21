/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <iostream>
#include <cstdlib>

#include <qti/qimsdk.h>
#include <getopt.h>

using namespace qti;

static const std::string home_path =
    std::getenv("HOME") ? std::getenv("HOME") : "";

// Base path for sample assets (models/, labels/ live under it).
// Set from the --model-base-path argument, or the default location.
static std::string model_base_path;

// Input MP4 file path, overridden via the --input-config argument.
static std::string input_config;

//  Example pipeline:
//
//    src → demux ┬→ vqueue → vparse → vdecoder → [vf] → composer(sink_0)
//                └→ aq1 → aparse → adecoder → aq2 → convert → resample →
//                   [af] → splitter → aq3 → converter → q1 → inferencing →
//                   postprocessing → [mlf] → aq4 → composer(sink_1)
//    composer → oq → display
//
//  The pipeline reads an MP4 file with H.264 video and FLAC audio tracks.
//  The video stream is decoded and passed straight to the composer. The
//  audio stream is decoded, normalized to signed 16-bit PCM, split into
//  fixed-size buffers, converted into YamNet's model input tensor format,
//  and classified. The composer overlays the classification result as a
//  label card on top of the video before displaying the result through
//  Wayland.

void create_and_execute_pipeline() {

  // Reads the input MP4 file as raw bytes.
  Element src("filesrc", "src");
  src.set("location", input_config);

  // Extracts elementary streams from the MP4 container.
  Element demux("qtdemux", "demux");

  // Buffers compressed video access units while the audio-classification
  // branch accumulates its first inference window (~2s at 16kHz for
  // YamNet's 31200-sample buffer). qtivcomposer withholds output until
  // both sink pads have a buffer, so the default queue size is not enough
  // to absorb that startup latency without stalling the demuxer.
  Element vqueue("queue", "vqueue");
  vqueue.set("max-size-buffers", 0);
  vqueue.set("max-size-bytes", 0);
  vqueue.set("max-size-time", 5000000000ULL);

  // Prepares the H.264 bitstream for the decoder.
  Element vparse("h264parse", "vparse");

  // Decodes the compressed H.264 stream into raw video frames.
  //
  // The I/O mode is configured to enforce DMA buffer usage,
  // avoiding unnecessary buffer copies.
  Element vdecoder("v4l2h264dec", "vdecoder");
  vdecoder.set("output-io-mode", 4);
  vdecoder.set("capture-io-mode", 4);

  // Queues data between pipeline stages.
  Element aq1("queue", "aq1");

  // Prepares the FLAC bitstream for the decoder.
  Element aparse("flacparse", "aparse");

  // Decodes the compressed FLAC stream into raw PCM audio.
  Element adecoder("flacdec", "adecoder");

  // Queues data between pipeline stages.
  Element aq2("queue", "aq2");

  // Converts the decoded audio to a format audioresample can operate on.
  Element convert("audioconvert", "convert");

  // Resamples the audio stream to the rate expected by the ML converter.
  Element resample("audioresample", "resample");

  // Restricts the audio stream to signed 16-bit PCM.
  auto af = AudioFilter().format(AudioFilter::Format::S16LE);

  // Splits the raw audio stream into fixed-size buffers sized for YamNet's
  // expected input window.
  Element splitter("audiobuffersplit", "splitter");
  splitter.set("output-buffer-size", 31200);

  // Queues data between pipeline stages.
  Element aq3("queue", "aq3");

  // Converts raw audio buffers into model input tensor format.
  Element converter("qtimlaconverter", "converter");
  converter.set("sample-rate", 16000);

  // Queues converted tensors before inference.
  Element q1("queue", "q1");

  // Executes the ML model and attaches classification tensor outputs to
  // each buffer.
  Element inferencing("qtimltflite", "inferencing");
  inferencing.set("model", model_base_path + "/models/yamnet.tflite");

  // Decodes model output tensors into an audio classification label card.
  //
  // results caps the number of classifications kept per inference window;
  // settings sets the minimum confidence (percent) a label must reach to
  // be reported.
  Element postprocessing("qtimlpostprocess", "postprocessing");
  postprocessing.set("module", "yamnet");
  postprocessing.set("labels", model_base_path + "/labels/yamnet.json");
  postprocessing.set("settings", "{\"confidence\": 10.0}");
  postprocessing.set("results", 3);

  // Restricts the video stream to NV12 before it reaches the composer.
  auto vf = VideoFilter().format("NV12");

  // Restricts the classification label card to a fixed size before it
  // reaches the composer.
  //
  // qtimlpostprocess only emits RGBA or RGBx frames; BGRA is not supported.
  auto mlf = VideoFilter().resolution(368, 64).format("RGBA");

  // Queues data between pipeline stages.
  Element aq4("queue", "aq4");

  // Composites the video stream and the classification label card into a
  // single output frame.
  Element composer("qtivcomposer", "composer");

  // Queues data between pipeline stages.
  Element oq("queue", "oq");

  // Render the composited stream on display.
  Element display("waylandsink", "display");
  display.set("fullscreen", true);

  // Creates the pipeline, adds and links elements, and executes it.
  //
  // Explicit linking is applied to branch the demuxed streams into the
  // video and audio-classification paths, and to merge them back at the
  // composer.
  Pipeline pipeline("audio-classification-yamnet");
  pipeline.add(src)
          .add(demux)
          .add(vqueue)
          .add(vparse)
          .add(vdecoder)
          .add_stream_filter("vf", vf)
          .add(aq1)
          .add(aparse)
          .add(adecoder)
          .add(aq2)
          .add(convert)
          .add(resample)
          .add_stream_filter("af", af)
          .add(splitter)
          .add(aq3)
          .add(converter)
          .add(q1)
          .add(inferencing)
          .add(postprocessing)
          .add_stream_filter("mlf", mlf)
          .add(aq4)
          .add(composer)
          .add(oq)
          .add(display)

          .link("src", "demux")
          .link("demux", "vqueue", "vparse", "vdecoder", "vf", "composer")
          .link("demux", "aq1", "aparse", "adecoder", "aq2", "convert",
                "resample", "af", "splitter", "aq3", "converter", "q1",
                "inferencing", "postprocessing", "mlf", "aq4", "composer")
          .link("composer", "oq", "display");

  pipeline.get("composer").input(1).set("position", "<50, 50>");
  pipeline.get("composer").input(1).set("dimensions", "<368, 64>");

  pipeline.execute();
}

int main(int argc, char **argv) {
  if (home_path.empty()) {
    std::cerr << "Error: HOME environment variable is not set." << std::endl;
    return 1;
  }

  // Base path for sample assets; override via --model-base-path argument.
  model_base_path = home_path + "/Downloads/qimsdk_samples";
  // Path for sample input assets; override via --input-config argument.
  input_config = home_path + "/Downloads/qimsdk_samples/media/audio_video_sample.mp4";

  const std::string default_model_base_path = model_base_path;
  const std::string default_input_config = input_config;

  static struct option long_options[] = {
    {"model-base-path", required_argument, 0, 'm'},
    {"input-config", required_argument, 0, 'i'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
  };

  auto print_usage = [&](std::ostream &out) {
    out << "Usage: " << argv[0] << " [OPTIONS]\n"
        << "\n"
        << "Options:\n"
        << "  -m, --model-base-path PATH   Base path for models and labels\n"
        << "                                (default: " << default_model_base_path << ")\n"
        << "  -i, --input-config PATH      Input MP4 file path (H.264 video + FLAC audio)\n"
        << "                                (default: " << default_input_config << ")\n"
        << "  -h, --help                   Show this help message and exit\n";
  };

  opterr = 0;  // Suppress getopt_long's own diagnostics; print_usage covers it.

  int option_index = 0;
  int c;
  while ((c = getopt_long(argc, argv, "m:i:h", long_options, &option_index)) != -1) {
    switch (c) {
      case 'm':
        model_base_path = optarg;
        break;
      case 'i':
        input_config = optarg;
        break;
      case 'h':
        print_usage(std::cout);
        return 0;
      case '?':
      default:
        print_usage(std::cerr);
        return 1;
    }
  }

  if (optind != argc) {
    std::cerr << "Error: unexpected argument '" << argv[optind] << "'\n\n";
    print_usage(std::cerr);
    return 1;
  }

  // Route GStreamer logs through the QIMSDK logger and enable debug output.
  qti::SetImsdkGstLogMode(qti::ImsdkGstLogMode::ImsdkLog);
  qti::SetImsdkLogLevel(qti::ImsdkLogLevel::Debug);

  try {
    create_and_execute_pipeline();
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
