/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ml-qairt-engine.h"

#include <string>
#include <vector>
#include <memory>

#include <gst/ml/gstmlmeta.h>
#include <fastrpc/remote.h>
#include <QairtApi.hpp>
#include <QairtBackend.hpp>
#include <QairtContext.hpp>
#include <QairtGraph.hpp>
#include <QairtInfo.hpp>
#include <QairtTensor.hpp>
#include <QairtLog.hpp>
#include <System/QairtSystemContext.hpp>
#include <System/QairtSystemDlc.hpp>

#define GST_CAT_DEFAULT gst_ml_qairt_engine_debug_category()
#define GST_CAT_QAIRT_SDK gst_ml_qairt_sdk_debug_category()

#define GST_ML_QAIRT_HTP_BACKEND_MIN_ARCH_VER 0x68

struct _GstMLQairtEngine
{
  GstMLInfo *ininfo;
  GstMLInfo *outinfo;

  // QAIRT low-level runtime objects. Declared in destruction-safe order; the
  // struct is released with delete so members tear down bottom-to-top.
  std::shared_ptr<qairt::Api>       api;
  std::shared_ptr<qairt::SystemApi> sysapi;
  std::shared_ptr<qairt::Backend>   backend;
  std::shared_ptr<qairt::Context>   context;

  // The single graph being executed (graph 0).
  std::shared_ptr<qairt::Graph> graph;

  // Pre-built input and output tensors, in negotiated order.
  std::vector<std::shared_ptr<qairt::Tensor>> inputs;
  std::vector<std::shared_ptr<qairt::Tensor>> outputs;
};

GType
gst_ml_qairt_backend_get_type (void)
{
  static GType gtype = 0;
  static const GEnumValue variants[] = {
    { GST_ML_QAIRT_BACKEND_CPU,
        "Run the processing on the CPU", "cpu"
    },
    { GST_ML_QAIRT_BACKEND_GPU,
        "Run the processing on the GPU", "gpu"
    },
    { GST_ML_QAIRT_BACKEND_HTP,
        "Run the processing on the Hexagon Tensor Processor", "htp"
    },
    {0, NULL, NULL},
  };

  if (!gtype)
    gtype = g_enum_register_static ("GstMLQairtBackend", variants);

  return gtype;
}

GType
gst_ml_qairt_exec_priority_get_type (void)
{
  static GType gtype = 0;
  static const GEnumValue variants[] = {
    { GST_ML_QAIRT_EXEC_PRIORITY_NORMAL,
        "Normal priority", "normal"
    },
    { GST_ML_QAIRT_EXEC_PRIORITY_LOW,
        "Lower priority", "low"
    },
    { GST_ML_QAIRT_EXEC_PRIORITY_NORMAL_HIGH,
        "Between normal and high priority", "normal-high"
    },
    { GST_ML_QAIRT_EXEC_PRIORITY_HIGH,
        "Higher than normal priority", "high"
    },
    { GST_ML_QAIRT_EXEC_PRIORITY_CRITICAL,
        "Critical priority", "critical"
    },
    {0, NULL, NULL},
  };

  if (!gtype)
    gtype = g_enum_register_static ("GstMLQairtExecPriority", variants);

  return gtype;
}

static GstDebugCategory *
gst_ml_qairt_engine_debug_category (void)
{
  static gsize catonce = 0;

  if (g_once_init_enter (&catonce)) {
    gsize catdone = (gsize) _gst_debug_category_new ("ml-qairt-engine", 0,
        "Machine Learning QAIRT Engine");
    g_once_init_leave (&catonce, catdone);
  }
  return (GstDebugCategory *) catonce;
}

static GstDebugCategory *
gst_ml_qairt_sdk_debug_category (void)
{
  static gsize catonce = 0;

  if (g_once_init_enter (&catonce)) {
    gsize catdone = (gsize) _gst_debug_category_new ("ml-qairt-sdk", 0,
        "Machine Learning QAIRT SDK");
    g_once_init_leave (&catonce, catdone);
  }
  return (GstDebugCategory *) catonce;
}

static void
gst_ml_qairt_log_callback (const char * format, QairtLog_Level_t loglvl,
    G_GNUC_UNUSED uint64_t timestamp, G_GNUC_UNUSED void * userdata,
    va_list vargs)
{
  GstDebugLevel level = GST_LEVEL_NONE;

  switch (loglvl) {
    case QAIRT_LOG_LEVEL_ERROR:
      level = GST_LEVEL_ERROR;
      break;
    case QAIRT_LOG_LEVEL_WARN:
      level = GST_LEVEL_WARNING;
      break;
    case QAIRT_LOG_LEVEL_INFO:
      level = GST_LEVEL_INFO;
      break;
    case QAIRT_LOG_LEVEL_DEBUG:
      level = GST_LEVEL_DEBUG;
      break;
    case QAIRT_LOG_LEVEL_VERBOSE:
      level = GST_LEVEL_LOG;
      break;
    default:
      break;
  }

  if (G_UNLIKELY (level <= GST_LEVEL_MAX && level <= _gst_debug_min)) {
    gst_debug_log_valist (GST_CAT_QAIRT_SDK, level, __FILE__, GST_FUNCTION,
        __LINE__, NULL, format, vargs);
  }
}

static GstMLType
gst_ml_type_from_data_type (qairt::DataType type)
{
  switch (type) {
    case qairt::DataType::Int8:
    case qairt::DataType::SFixedPoint8:
      return GST_ML_TYPE_INT8;
    case qairt::DataType::UInt8:
    case qairt::DataType::UFixedPoint8:
    case qairt::DataType::Bool8:
      return GST_ML_TYPE_UINT8;
    case qairt::DataType::Int16:
    case qairt::DataType::SFixedPoint16:
      return GST_ML_TYPE_INT16;
    case qairt::DataType::UInt16:
    case qairt::DataType::UFixedPoint16:
      return GST_ML_TYPE_UINT16;
    case qairt::DataType::Int32:
    case qairt::DataType::SFixedPoint32:
      return GST_ML_TYPE_INT32;
    case qairt::DataType::UInt32:
    case qairt::DataType::UFixedPoint32:
      return GST_ML_TYPE_UINT32;
    case qairt::DataType::Int64:
      return GST_ML_TYPE_INT64;
    case qairt::DataType::UInt64:
      return GST_ML_TYPE_UINT64;
    case qairt::DataType::Float16:
      return GST_ML_TYPE_FLOAT16;
    case qairt::DataType::Float32:
      return GST_ML_TYPE_FLOAT32;
    default:
      GST_ERROR ("Unsupported QAIRT data type %d!", static_cast<int> (type));
      break;
  }

  return GST_ML_TYPE_UNKNOWN;
}

template <typename T>
static std::shared_ptr<T>
gst_ml_qairt_load_library (std::string library)
{
  std::shared_ptr<T> api;

  try {
    GST_DEBUG ("Loading library '%s'", library.c_str ());
    api = std::make_shared<T> (library.c_str ());
  } catch (const std::exception &e) {
    GST_DEBUG ("Failed to load library '%s' (%s)", library.c_str (), e.what ());
    library += ".2";

    GST_DEBUG ("Loading versioned library '%s'", library.c_str ());
    api = std::make_shared<T> (library.c_str ());
  }

  GST_DEBUG ("Loaded library '%s'", library.c_str ());
  return api;
}

static qairt::Priority
gst_ml_translate_priority (GstMLQairtExecPriority priority)
{
  switch (priority) {
    case GST_ML_QAIRT_EXEC_PRIORITY_LOW:
      return qairt::Priority::Low;
    case GST_ML_QAIRT_EXEC_PRIORITY_NORMAL_HIGH:
      return qairt::Priority::NormalHigh;
    case GST_ML_QAIRT_EXEC_PRIORITY_HIGH:
      return qairt::Priority::High;
    case GST_ML_QAIRT_EXEC_PRIORITY_CRITICAL:
      return qairt::Priority::Critical;
    case GST_ML_QAIRT_EXEC_PRIORITY_NORMAL:
    default:
      return qairt::Priority::Normal;
  }
}

static gboolean
gst_cdsp_get_arch_ver (guint32 * arch_ver)
{
  struct remote_dsp_capability dsp_cap = { CDSP_DOMAIN_ID, ARCH_VER, 0 };
  gint ret = 0;

  ret = remote_handle_control (DSPRPC_GET_DSP_INFO, &dsp_cap, sizeof (dsp_cap));
  if (ret != 0) {
    GST_WARNING ("Failed to query CDSP architecture version: 0x%x", ret);
    return FALSE;
  }

  *arch_ver = dsp_cap.capability & 0xFF;
  GST_INFO ("CDSP architecture version: 0x%x (raw capability: 0x%x)",
      *arch_ver, dsp_cap.capability);

  if (*arch_ver == 0)
    return FALSE;

  return TRUE;
}

static const gchar *
gst_ml_qairt_get_htp_backend_library (void)
{
  guint32 arch_ver = 0;

  if (!gst_cdsp_get_arch_ver (&arch_ver)) {
    GST_INFO ("Using QAIRT HTP backend as fallback");
    return "libQairtHtp.so";
  }

  if (arch_ver < GST_ML_QAIRT_HTP_BACKEND_MIN_ARCH_VER) {
    GST_INFO ("Using QAIRT DSP backend for CDSP architecture v%x", arch_ver);
    return "libQairtDsp.so";
  }

  return "libQairtHtp.so";
}

static gboolean
gst_ml_info_populate_tensor (GstMLInfo * info, guint idx, GstMLType type,
    guint rank, const uint32_t * dimensions)
{
  if (type == GST_ML_TYPE_UNKNOWN) {
    GST_ERROR ("Tensor[%u] has unsupported data type!", idx);
    return FALSE;
  }

  if (rank > GST_ML_TENSOR_MAX_DIMS) {
    GST_ERROR ("Tensor rank %u exceeds maximum %u!", rank, GST_ML_TENSOR_MAX_DIMS);
    return FALSE;
  }

  GST_ML_INFO_TENSOR_TYPE (info, idx) = type;
  GST_ML_INFO_N_DIMENSIONS (info, idx) = rank;

  GST_DEBUG ("Tensor[%u] type: %s", idx, gst_ml_type_to_string (type));

  for (guint num = 0; num < rank; num++) {
    GST_ML_INFO_TENSOR_DIM (info, idx, num) = dimensions[num];
    GST_DEBUG ("Tensor[%u] Dimension[%u]: %u", idx, num, dimensions[num]);
  }

  return TRUE;
}

static void
gst_ml_frame_normalize (GstMLFrame * outframe, guint idx,
    qairt::DataType datatype, const qairt::QuantizeParams &qparams)
{
  // If output is not FLOAT32 and native type does not differ then nothing to
  // do.
  if (GST_ML_FRAME_TENSOR_TYPE (outframe, idx) != GST_ML_TYPE_FLOAT32 ||
      GST_ML_FRAME_TENSOR_TYPE (outframe, idx) ==
          gst_ml_type_from_data_type (datatype))
    return;

  qairt::QuantizationEncoding encoding = qparams.getQuantizationEncoding ();

  gfloat scale = 1.0f;
  gint32 offset = 0;

  if (encoding == qairt::QuantizationEncoding::ScaleOffset) {
    const qairt::ScaleOffset &so = qparams.getScaleOffsetEncoding ();

    scale = so.getScale ();
    offset = so.getOffset ();
  }

  gfloat *output =
      reinterpret_cast<gfloat*>(GST_ML_FRAME_BLOCK_DATA (outframe, idx));
  gsize n_elements = gst_ml_info_tensor_size (&(outframe->info), idx) /
      gst_ml_type_get_size (GST_ML_FRAME_TENSOR_TYPE (outframe, idx));

  switch (datatype) {
    case qairt::DataType::Float32:
      // Native type already matches, no conversion needed.
      break;
#if defined(__ARM_FP16_FORMAT_IEEE)
    case qairt::DataType::Float16:
    {
      __fp16 *data = reinterpret_cast<__fp16 *>(output);

      for (gint pos = (n_elements - 1); pos >= 0; pos--)
        output[pos] = (static_cast<gfloat> (data[pos]) + offset) * scale;

      break;
    }
#endif  // defined(__ARM_FP16_FORMAT_IEEE)
    case qairt::DataType::UFixedPoint8:
    case qairt::DataType::UInt8:
    case qairt::DataType::Bool8:
    {
      uint8_t *data = reinterpret_cast<uint8_t *> (output);

      for (gint pos = static_cast<gint> (n_elements) - 1; pos >= 0; pos--)
        output[pos] = (static_cast<gfloat> (data[pos]) + offset) * scale;

      break;
    }
    case qairt::DataType::SFixedPoint8:
    case qairt::DataType::Int8:
    {
      int8_t *data = reinterpret_cast<int8_t *> (output);

      for (gint pos = static_cast<gint> (n_elements) - 1; pos >= 0; pos--)
        output[pos] = (static_cast<gfloat> (data[pos]) + offset) * scale;

      break;
    }
    case qairt::DataType::UFixedPoint16:
    case qairt::DataType::UInt16:
    {
      uint16_t *data = reinterpret_cast<uint16_t *> (output);

      for (gint pos = static_cast<gint> (n_elements) - 1; pos >= 0; pos--)
        output[pos] = (static_cast<gfloat> (data[pos]) + offset) * scale;

      break;
    }
    case qairt::DataType::SFixedPoint16:
    case qairt::DataType::Int16:
    {
      int16_t *data = reinterpret_cast<int16_t *> (output);

      for (gint pos = static_cast<gint> (n_elements) - 1; pos >= 0; pos--)
        output[pos] = (static_cast<gfloat> (data[pos]) + offset) * scale;

      break;
    }
    case qairt::DataType::UFixedPoint32:
    case qairt::DataType::UInt32:
    {
      uint32_t *data = reinterpret_cast<uint32_t *> (output);

      for (gint pos = static_cast<gint> (n_elements) - 1; pos >= 0; pos--)
        output[pos] = (static_cast<gfloat> (data[pos]) + offset) * scale;

      break;
    }
    case qairt::DataType::SFixedPoint32:
    case qairt::DataType::Int32:
    {
      int32_t *data = reinterpret_cast<int32_t *> (output);

      for (gint pos = static_cast<gint> (n_elements) - 1; pos >= 0; pos--)
        output[pos] = (static_cast<gfloat> (data[pos]) + offset) * scale;

      break;
    }
    default:
      GST_ERROR ("Output data type %d not supported for conversion!",
          static_cast<int> (datatype));
      break;
  }
}

static std::shared_ptr<qairt::Tensor>
gst_ml_qairt_make_tensor (GstMLQairtEngine * engine,
    qairt::SystemContextTensorInfo &tinfo, gboolean is_input)
{
  std::shared_ptr<qairt::Tensor> tensor =
      engine->api->makeShared<qairt::Tensor> ();

  qairt::TensorProperties prop = engine->api->make<qairt::TensorProperties> ();
  prop.setIsInput (is_input);
  prop.setIsOutput (!is_input);

  tensor->setId (tinfo.getId ());
  tensor->setName (tinfo.getName ());
  tensor->setTensorProperties (prop);
  tensor->setDataFormat (0);
  tensor->setDataType (tinfo.getDataType ());
  tensor->setDimensions (std::vector<uint32_t> (tinfo.getDimensions (),
      tinfo.getDimensions () + tinfo.getRank ()));
  tensor->setIsDynamicDimensions (std::vector<bool> (tinfo.getRank (), false));

  const qairt::SystemContextQuantizationInfo &qinfo = tinfo.getQuantInfo ();

  if (qinfo.getQuantizationType () ==
          qairt::SystemContextQuantInfoType::QAIRT_QUANTIZATION_INFO_SCALE_OFFSET) {
    const qairt::SystemContextScaleOffset &so = qinfo.getScaleOffset ();
    qairt::QuantizeParams qparams = engine->api->make<qairt::QuantizeParams> ();

    qparams.setScaleOffsetEncoding (engine->api->make<qairt::ScaleOffset> ());
    qparams.getScaleOffsetEncoding ().setScale (so.getScale ());
    qparams.getScaleOffsetEncoding ().setOffset (so.getOffset ());

    tensor->setQuantizeParams (qparams);

    GST_DEBUG ("Tensor '%s' quantized: scale %f offset %d",
        tinfo.getName ().c_str (), so.getScale (), so.getOffset ());
  }

  return tensor;
}

static gboolean
gst_ml_qairt_engine_load_backend (GstMLQairtEngine * engine,
    GstMLQairtBackend backend)
{
  const gchar *libname = NULL;

  switch (backend) {
    case GST_ML_QAIRT_BACKEND_GPU:
      libname = "libQairtGpu.so";
      break;
    case GST_ML_QAIRT_BACKEND_HTP:
      libname = gst_ml_qairt_get_htp_backend_library ();
      break;
    case GST_ML_QAIRT_BACKEND_CPU:
    default:
      libname = "libQairtCpu.so";
      break;
  }

  try {
    engine->api = gst_ml_qairt_load_library<qairt::Api> (libname);
    engine->sysapi =
        gst_ml_qairt_load_library<qairt::SystemApi> ("libQairtSystem.so");

    qairt::Log log = engine->api->make<qairt::Log> (gst_ml_qairt_log_callback,
        static_cast<QairtLog_Level_t> (qairt::LogLevel::Warn));

    engine->backend = engine->api->makeShared<qairt::Backend> (log);
  } catch (const std::exception &e) {
    GST_ERROR ("Failed to load backend: %s", e.what ());
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_ml_qairt_engine_build_input_tensors (GstMLQairtEngine * engine,
    std::vector<qairt::SystemContextTensorInfo> &intensors)
{
  gboolean success = FALSE;

  if (intensors.size () > GST_ML_MAX_TENSORS) {
    GST_ERROR ("Model has %zu inputs, maximum is %u!", intensors.size (),
        GST_ML_MAX_TENSORS);
    return FALSE;
  }

  try {
    for (guint idx = 0; idx < intensors.size (); idx++) {
      qairt::SystemContextTensorInfo &tinfo = intensors[idx];

      GST_DEBUG ("Input tensor[%u] name: %s", idx, tinfo.getName ().c_str ());

      success = gst_ml_info_populate_tensor (engine->ininfo, idx,
          gst_ml_type_from_data_type (tinfo.getDataType ()), tinfo.getRank (),
          tinfo.getDimensions ());

      if (!success)
        return FALSE;

      engine->ininfo->n_tensors++;
      engine->inputs.push_back (gst_ml_qairt_make_tensor (engine, tinfo, TRUE));
    }
  } catch (const std::exception &e) {
    GST_ERROR ("Failed to build input tensors: %s", e.what ());
    return FALSE;
  }

  GST_DEBUG ("Number of input tensors: %u",
      GST_ML_INFO_N_TENSORS (engine->ininfo));

  return TRUE;
}

static gboolean
gst_ml_qairt_engine_build_output_tensors (GstMLQairtEngine * engine,
    GList * names, std::vector<qairt::SystemContextTensorInfo> &outtensors)
{
  gboolean success = FALSE;

  try {
    // Honor the optional user-specified output names, if any were given.
    if (names != NULL) {
      guint length = g_list_length (names);

      if (length > GST_ML_MAX_TENSORS) {
        GST_ERROR ("Requested %u outputs, maximum is %u!", length,
            GST_ML_MAX_TENSORS);
        return FALSE;
      }

      for (guint idx = 0; idx < length; idx++) {
        const gchar *name = (const gchar *) g_list_nth_data (names, idx);

        auto it = std::find_if(outtensors.begin(), outtensors.end(),
            [name] (const qairt::SystemContextTensorInfo &tinfo)
            { return tinfo.getName () == name; } );

        if (it == outtensors.end()) {
          GST_ERROR ("Output tensor name '%s' not found in graph!", name);
          return FALSE;
        }

        engine->outputs.push_back (gst_ml_qairt_make_tensor (engine, *it, FALSE));
      }
    } else {
      if (outtensors.size () > GST_ML_MAX_TENSORS) {
        GST_ERROR ("Model has %zu outputs, maximum is %u!", outtensors.size (),
            GST_ML_MAX_TENSORS);
        return FALSE;
      }

      for (guint idx = 0; idx < outtensors.size (); idx++) {
        qairt::SystemContextTensorInfo &tinfo = outtensors[idx];
        engine->outputs.push_back (gst_ml_qairt_make_tensor (engine, tinfo, FALSE));
      }
    }
  } catch (const std::exception &e) {
    GST_ERROR ("Failed to build output tensors: %s", e.what ());
    return FALSE;
  }

  for (guint idx = 0; idx < engine->outputs.size (); idx++) {
    std::shared_ptr<qairt::Tensor> &tensor = engine->outputs[idx];
    const std::vector<uint32_t> &dims = tensor->getDimensions ();

    GST_DEBUG ("Output tensor[%u] name: %s", idx, tensor->getName ().c_str ());

    success = gst_ml_info_populate_tensor (engine->outinfo, idx,
        gst_ml_type_from_data_type (tensor->getDataType ()), dims.size (),
        dims.data ());

    if (!success)
      return FALSE;

    engine->outinfo->n_tensors++;
  }

  GST_DEBUG ("Number of output tensors: %u",
      GST_ML_INFO_N_TENSORS (engine->outinfo));

  return TRUE;
}

static gboolean
gst_ml_qairt_engine_build_tensors (GstMLQairtEngine * engine, GList * names,
    qairt::SystemContextGraphInfo &graphinfo)
{
  if (!gst_ml_qairt_engine_build_input_tensors (engine,
        graphinfo.getGraphInputs ()))
    return FALSE;

  return gst_ml_qairt_engine_build_output_tensors (engine, names,
      graphinfo.getGraphOutputs ());
}

static gboolean
gst_ml_qairt_engine_setup_dlc_graphs (GstMLQairtEngine * engine,
    GstMLQairtSettings * settings)
{
  qairt::SystemContextGraphInfoSet graphinfoset;

  try {
    // Some backends (e.g. CPU) reject a priority context config with
    // QNN_COMMON_ERROR_NOT_SUPPORTED; fall back to the default config.
    try {
      qairt::ContextConfiguration config =
          engine->api->make<qairt::ContextConfiguration> ();

      GST_DEBUG ("Creating context with priority configuration");
      config.setPriority (gst_ml_translate_priority (settings->priority));

      engine->context = std::make_shared<qairt::Context> (
          engine->backend->createContext (config));
    } catch (const std::exception &e) {
      GST_WARNING ("Context creation with priority config failed (%s); "
          "retrying with default configuration", e.what ());

      engine->context = std::make_shared<qairt::Context> (
          engine->backend->createContext ());
    }

    GST_DEBUG ("Composing graphs from DLC '%s'", settings->modelfile);
    std::shared_ptr<qairt::SystemDlc> dlc =
        engine->sysapi->makeShared<qairt::SystemDlc> (settings->modelfile);

    graphinfoset = dlc->composeGraphs (
        *engine->backend, *engine->context, *engine->api, *engine->sysapi);

    if (graphinfoset.getGraphInfos ().size () != 1)
      throw std::runtime_error ("Model contains 0 or more then 1 graphs!");

    auto &graphinfo = graphinfoset.getGraphInfos ()[0];

    engine->graph = engine->context->retrieveGraph (graphinfo.getGraphName ());
    engine->graph->finalize ();
  } catch (const std::exception &e) {
    GST_ERROR ("Failed to compose DLC graphs from '%s': %s",
        settings->modelfile, e.what ());
    return FALSE;
  }

  auto &graphinfo = graphinfoset.getGraphInfos ()[0];
  return gst_ml_qairt_engine_build_tensors (engine, settings->outputs, graphinfo);
}

static gboolean
gst_ml_qairt_engine_setup_binary_graphs (GstMLQairtEngine * engine,
    GstMLQairtSettings * settings)
{
  gchar *contents = NULL;
  GError *error = NULL;
  gsize length = 0;

  if (!g_file_get_contents (settings->modelfile, &contents, &length, &error)) {
    GST_ERROR ("Failed to read model file '%s': %s!", settings->modelfile,
        GST_STR_NULL (error ? error->message : NULL));
    g_clear_error (&error);
    return FALSE;
  }

  GST_DEBUG ("Creating context from cached binary '%s'", settings->modelfile);

  std::shared_ptr<qairt::SystemContext> syscontext;
  qairt::SystemContextBinaryInfo bininfo;

  try {
    qairt::ContextBinaryBuffer binary =
        engine->api->make<qairt::ContextBinaryBuffer> ();

    binary.setData (reinterpret_cast<void *> (contents));
    binary.setSize (static_cast<uint64_t> (length));

    syscontext = engine->sysapi->makeShared<qairt::SystemContext> ();
    bininfo = syscontext->getBinaryInfo (
        reinterpret_cast<void *> (contents), static_cast<uint64_t> (length));

    engine->context = std::make_shared<qairt::Context> (
        engine->backend->createContextFromBinary (std::nullopt, std::nullopt, binary));

    if (bininfo.getGraphs ().size () != 1)
      throw std::runtime_error ("Model contains 0 or more then 1 graphs!");

    auto &graphinfo = bininfo.getGraphs ()[0];
    engine->graph = engine->context->retrieveGraph (graphinfo.getGraphName ());
  } catch (const std::exception &e) {
    GST_ERROR ("Failed to create context '%s': %s", settings->modelfile, e.what ());
    g_clear_pointer (&contents, g_free);
    return FALSE;
  }

  g_clear_pointer (&contents, g_free);
  auto &graphinfo = bininfo.getGraphs ()[0];

  return gst_ml_qairt_engine_build_tensors (engine, settings->outputs, graphinfo);
}

static gboolean
gst_ml_qairt_engine_setup_model (GstMLQairtEngine * engine,
    GstMLQairtSettings * settings)
{
  // '.dlc' containers are composed into a fresh context; anything else is
  // treated as a cached context binary.
  if (g_str_has_suffix (settings->modelfile, ".dlc"))
    return gst_ml_qairt_engine_setup_dlc_graphs (engine, settings);

  return gst_ml_qairt_engine_setup_binary_graphs (engine, settings);
}

GstMLQairtEngine *
gst_ml_qairt_engine_new (GstMLQairtSettings * settings)
{
  GstMLQairtEngine *engine = NULL;

  g_return_val_if_fail (settings != NULL, NULL);

  if (settings->modelfile == NULL) {
    GST_ERROR ("No model file name!");
    return NULL;
  } else if (!g_file_test (settings->modelfile, G_FILE_TEST_IS_REGULAR)) {
    GST_ERROR ("Model file '%s' does not exist!", settings->modelfile);
    return NULL;
  }

  engine = new GstMLQairtEngine;
  g_return_val_if_fail (engine != NULL, NULL);

  engine->ininfo = gst_ml_info_new ();
  engine->outinfo = gst_ml_info_new ();

  if (!gst_ml_qairt_engine_load_backend (engine, settings->backend))
    goto cleanup;

  if (!gst_ml_qairt_engine_setup_model (engine, settings))
    goto cleanup;

  GST_INFO ("Created engine: %p", engine);
  return engine;

cleanup:
  gst_ml_qairt_engine_free (engine);
  return NULL;
}

void
gst_ml_qairt_engine_free (GstMLQairtEngine * engine)
{
  if (NULL == engine)
    return;

  g_clear_pointer (&engine->outinfo, gst_ml_info_free);
  g_clear_pointer (&engine->ininfo, gst_ml_info_free);

  GST_INFO ("Destroyed engine: %p", engine);
  delete engine;
}

GstCaps *
gst_ml_qairt_engine_get_input_caps (GstMLQairtEngine * engine)
{
  if (engine == NULL)
    return NULL;

  return gst_ml_info_to_caps (engine->ininfo);
}

GstCaps *
gst_ml_qairt_engine_get_output_caps (GstMLQairtEngine * engine)
{
  GstCaps *caps = NULL;
  GValue mltypes = G_VALUE_INIT;

  if (engine == NULL)
    return NULL;

  caps = gst_ml_info_to_caps (engine->outinfo);

  g_value_init (&mltypes, GST_TYPE_ARRAY);

  for (guint idx = 0; idx < GST_ML_INFO_N_TENSORS (engine->outinfo); idx++) {
    GstMLType type = GST_ML_INFO_TENSOR_TYPE (engine->outinfo, idx);
    GValue value = G_VALUE_INIT;

    g_value_init (&value, G_TYPE_STRING);

    // If current type is already FLOAT, attach that same value for this tensor.
    if (type == GST_ML_TYPE_FLOAT32) {
      g_value_set_string (&value, gst_ml_type_to_string (type));
      gst_value_array_append_and_take_value (&mltypes, &value);
      continue;
    }

    GValue list = G_VALUE_INIT;
    g_value_init (&list, GST_TYPE_LIST);

    g_value_set_string (&value, gst_ml_type_to_string (GST_ML_TYPE_FLOAT32));
    gst_value_list_append_value (&list, &value);

    g_value_set_string (&value, gst_ml_type_to_string (type));
    gst_value_list_append_and_take_value (&list, &value);

    gst_value_array_append_and_take_value (&mltypes, &list);
  }

  // Overwrite the type field by adding FLOAT in addition to current type.
  gst_caps_set_value (caps, "type", &mltypes);
  g_value_unset (&mltypes);

  GST_DEBUG ("Output caps: %" GST_PTR_FORMAT, caps);

  return caps;
}

gboolean
gst_ml_qairt_engine_execute (GstMLQairtEngine * engine, GstMLFrame * inframe,
    GstMLFrame * outframe)
{
  GstMLTensorMeta *mlmeta = NULL;

  g_return_val_if_fail (engine != NULL, FALSE);

  if (GST_ML_FRAME_N_BLOCKS (inframe) != engine->ininfo->n_tensors) {
    GST_WARNING ("Input buffer has %u memory blocks but engine requires %u!",
        GST_ML_FRAME_N_BLOCKS (inframe), engine->ininfo->n_tensors);
    return FALSE;
  }

  if (GST_ML_FRAME_N_BLOCKS (outframe) != engine->outinfo->n_tensors) {
    GST_WARNING ("Output buffer has %u memory blocks but engine requires %u!",
        GST_ML_FRAME_N_BLOCKS (outframe), engine->outinfo->n_tensors);
    return FALSE;
  }

  try {
    for (guint idx = 0; idx < engine->inputs.size (); idx++) {
      qairt::ClientBuffer &cb = engine->inputs[idx]->getClientBuffer ();

      cb.setData (GST_ML_FRAME_BLOCK_DATA (inframe, idx));
      cb.setDataSize (gst_ml_info_tensor_size (&(inframe->info), idx));
    }

    for (guint idx = 0; idx < engine->outputs.size (); idx++) {
      qairt::ClientBuffer &cb = engine->outputs[idx]->getClientBuffer ();

      cb.setData (GST_ML_FRAME_BLOCK_DATA (outframe, idx));
      cb.setDataSize (gst_ml_info_tensor_size (&(outframe->info), idx));
    }

    engine->graph->execute (engine->inputs, engine->outputs);

    // Convert the native output tensors to float and stamp the tensor names.
    for (guint idx = 0; idx < engine->outputs.size (); idx++) {
      std::shared_ptr<qairt::Tensor> &tensor = engine->outputs[idx];
      qairt::DataType datatype = tensor->getDataType ();
      const qairt::QuantizeParams &qparams = tensor->getQuantizeParams ();

      gst_ml_frame_normalize (outframe, idx, datatype, qparams);
      mlmeta = gst_buffer_get_ml_tensor_meta_id (outframe->buffer, idx);

      if (mlmeta == NULL)
        continue;

      mlmeta->name = g_quark_from_string (tensor->getName ().c_str ());
    }
  } catch (const std::exception &e) {
    GST_ERROR ("Model execution failed: %s", e.what ());
    return FALSE;
  }

  return TRUE;
}
