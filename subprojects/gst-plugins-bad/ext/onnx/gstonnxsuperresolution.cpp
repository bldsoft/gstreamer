/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) 2005-2012 David Schleef <ds@schleef.org>
 * Copyright (C) 2021 Aaron Boxer <aaron.boxer@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-onnxsuperresolution
 * @title: onnxsuperresolution
 * @see_also: videoscale
 *
 * This element resizes video frames. By default the element will try to
 * negotiate to the same size on the source and sinkpad so that no scaling
 * is needed. It is therefore safe to insert this element in a pipeline to
 * get more robust behaviour without any cost if no scaling is needed.
 *
 * This element supports a wide range of color spaces including various YUV and
 * RGB formats and is therefore generally able to operate anywhere in a
 * pipeline.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 -v filesrc location=videotestsrc.ogg ! oggdemux ! theoradec ! videoconvert ! onnxsuperresolution ! autovideosink
 * ]|
 *  Decode an Ogg/Theora and display the video. If the video sink chosen
 * cannot perform scaling, the video scaling will be performed by videoscale
 * when you resize the video window.
 * To create the test Ogg/Theora file refer to the documentation of theoraenc.
 * |[
 * gst-launch-1.0 -v filesrc location=videotestsrc.ogg ! oggdemux ! theoradec ! videoconvert ! onnxsuperresolution ! video/x-raw,width=100 ! autovideosink
 * ]|
 *  Decode an Ogg/Theora and display the video with a width of 100.
 *
 */

/*
 * Formulas for PAR, DAR, width and height relations:
 *
 * dar_n   w   par_n
 * ----- = - * -----
 * dar_d   h   par_d
 *
 * par_n    h   dar_n
 * ----- =  - * -----
 * par_d    w   dar_d
 *
 *         dar_n   par_d
 * w = h * ----- * -----
 *         dar_d   par_n
 *
 *         dar_d   par_n
 * h = w * ----- * -----
 *         dar_n   par_d
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <math.h>
#include "gstonnxsuperresolution.h"
#include "gstonnxclient.h"

#define GST_CAT_DEFAULT onnx_super_resolution_debug
GST_DEBUG_CATEGORY_STATIC (onnx_super_resolution_debug);
GST_DEBUG_CATEGORY_STATIC (CAT_PERFORMANCE);
#define GST_ONNX_MEMBER( self ) ((GstOnnxNamespace::GstOnnxClient *) (self->onnx_ptr))
GST_ELEMENT_REGISTER_DEFINE (onnxsuperresolution, "onnxsuperresolution",
    GST_RANK_PRIMARY, GST_TYPE_ONNX_SUPER_RESOLUTION);

enum
{
  PROP_0,
  PROP_MODEL_FILE,
  PROP_INPUT_IMAGE_FORMAT,
  PROP_OPTIMIZATION_LEVEL,
  PROP_EXECUTION_PROVIDER
};

#undef GST_VIDEO_SIZE_RANGE
#define GST_VIDEO_SIZE_RANGE "(int) [ 1, 32767]"
static const int RATIOS[] = { 2, 4 };

#define VIDEO_FORMATS "{ RGB,RGBA,BGR,BGRA }"

static GstStaticCaps gst_onnx_super_resolution_format_caps =
GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (VIDEO_FORMATS)
    ", pixel-aspect-ratio=1/1");

static GstCaps *
gst_onnx_super_resolution_get_capslist (void)
{
  static GstCaps *caps = NULL;
  static gsize inited = 0;

  if (g_once_init_enter (&inited)) {
    caps = gst_static_caps_get (&gst_onnx_super_resolution_format_caps);
    g_once_init_leave (&inited, 1);
  }
  return caps;
}

static GstPadTemplate *
gst_onnx_super_resolution_src_template_factory (void)
{
  return gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
      gst_onnx_super_resolution_get_capslist ());
}

static GstPadTemplate *
gst_onnx_super_resolution_sink_template_factory (void)
{
  return gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
      gst_onnx_super_resolution_get_capslist ());
}

static void gst_onnx_super_resolution_finalize (GstOnnxSuperResolution *
    videoscale);
static gboolean gst_onnx_super_resolution_src_event (GstBaseTransform * trans,
    GstEvent * event);

/* base transform vmethods */
static GstCaps *gst_onnx_super_resolution_transform_caps (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstCaps *gst_onnx_super_resolution_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static void gst_onnx_super_resolution_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_onnx_super_resolution_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static gboolean gst_onnx_super_resolution_set_info (GstVideoFilter * filter,
    GstCaps * in, GstVideoInfo * in_info, GstCaps * out,
    GstVideoInfo * out_info);
static GstFlowReturn gst_onnx_super_resolution_transform_frame (GstVideoFilter *
    filter, GstVideoFrame * in, GstVideoFrame * out);
static gboolean
gst_onnx_super_resolution_create_session (GstBaseTransform * trans);
static gboolean gst_onnx_super_resolution_process (GstBaseTransform * trans,
    GstBuffer * buf);

#define gst_onnx_super_resolution_parent_class parent_class
G_DEFINE_TYPE (GstOnnxSuperResolution, gst_onnx_super_resolution,
    GST_TYPE_VIDEO_FILTER);

static void
gst_onnx_super_resolution_class_init (GstOnnxSuperResolutionClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = (GstElementClass *) klass;
  GstBaseTransformClass *trans_class = (GstBaseTransformClass *) klass;
  GstVideoFilterClass *filter_class = (GstVideoFilterClass *) klass;

  GST_DEBUG_CATEGORY_INIT (onnx_super_resolution_debug, "onnxsuperresolution",
      0, "onnx_super_resolution");
  gobject_class->set_property = gst_onnx_super_resolution_set_property;
  gobject_class->get_property = gst_onnx_super_resolution_get_property;

  gobject_class->finalize =
      (GObjectFinalizeFunc) gst_onnx_super_resolution_finalize;

  /**
   * GstOnnxObjectDetector:model-file
   *
   * ONNX model file
   *
   * Since: 1.20
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_MODEL_FILE,
      g_param_spec_string ("model-file",
          "ONNX model file", "ONNX model file", NULL, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /**
   * GstOnnxObjectDetector:input-image-format
   *
   * Model input image format
   *
   * Since: 1.20
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_INPUT_IMAGE_FORMAT,
      g_param_spec_enum ("input-image-format",
          "Input image format",
          "Input image format",
          GST_TYPE_ML_MODEL_INPUT_IMAGE_FORMAT,
          GST_ML_MODEL_INPUT_IMAGE_FORMAT_HWC, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

   /**
    * GstOnnxObjectDetector:optimization-level
    *
    * ONNX optimization level
    *
    * Since: 1.20
    */
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_OPTIMIZATION_LEVEL,
      g_param_spec_enum ("optimization-level",
          "Optimization level",
          "ONNX optimization level",
          GST_TYPE_ONNX_OPTIMIZATION_LEVEL,
          GST_ONNX_OPTIMIZATION_LEVEL_ENABLE_EXTENDED, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /**
   * GstOnnxObjectDetector:execution-provider
   *
   * ONNX execution provider
   *
   * Since: 1.20
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_EXECUTION_PROVIDER,
      g_param_spec_enum ("execution-provider",
          "Execution provider",
          "ONNX execution provider",
          GST_TYPE_ONNX_EXECUTION_PROVIDER,
          GST_ONNX_EXECUTION_PROVIDER_CPU, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_static_metadata (element_class,
      "ONNX Super-resolution video upscaler", "Filter/Converter/Video/Scaler",
      "Upscales video", "Aaron Boxer <aaron.boxer@collabora.com>");

  gst_element_class_add_pad_template (element_class,
      gst_onnx_super_resolution_sink_template_factory ());
  gst_element_class_add_pad_template (element_class,
      gst_onnx_super_resolution_src_template_factory ());

  trans_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_onnx_super_resolution_transform_caps);
  trans_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_onnx_super_resolution_fixate_caps);
  trans_class->src_event =
      GST_DEBUG_FUNCPTR (gst_onnx_super_resolution_src_event);

  filter_class->set_info =
      GST_DEBUG_FUNCPTR (gst_onnx_super_resolution_set_info);
  filter_class->transform_frame =
      GST_DEBUG_FUNCPTR (gst_onnx_super_resolution_transform_frame);
}

static void
gst_onnx_super_resolution_init (GstOnnxSuperResolution * self)
{
  self->onnx_ptr = new GstOnnxNamespace::GstOnnxClient ();
  self->onnx_disabled = false;
}

static void
gst_onnx_super_resolution_finalize (GstOnnxSuperResolution * videoscale)
{
  g_free (videoscale->model_file);
  delete GST_ONNX_MEMBER (videoscale);
  G_OBJECT_CLASS (parent_class)->finalize (G_OBJECT (videoscale));
}

static void
gst_onnx_super_resolution_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOnnxSuperResolution *self = GST_ONNX_SUPER_RESOLUTION (object);
  const gchar *filename;
  auto onnxClient = GST_ONNX_MEMBER (self);

  switch (prop_id) {
    case PROP_MODEL_FILE:
      filename = g_value_get_string (value);
      if (filename
          && g_file_test (filename,
              (GFileTest) (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))) {
        if (self->model_file)
          g_free (self->model_file);
        self->model_file = g_strdup (filename);
      } else {
        GST_WARNING_OBJECT (self, "Model file '%s' not found!", filename);
        gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (self), TRUE);
      }
      break;
    case PROP_OPTIMIZATION_LEVEL:
      self->optimization_level =
          (GstOnnxOptimizationLevel) g_value_get_enum (value);
      break;
    case PROP_EXECUTION_PROVIDER:
      self->execution_provider =
          (GstOnnxExecutionProvider) g_value_get_enum (value);
      break;
    case PROP_INPUT_IMAGE_FORMAT:
      onnxClient->setInputImageFormat ((GstMlModelInputImageFormat)
          g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_onnx_super_resolution_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstOnnxSuperResolution *self = GST_ONNX_SUPER_RESOLUTION (object);
  auto onnxClient = GST_ONNX_MEMBER (self);

  switch (prop_id) {
    case PROP_MODEL_FILE:
      g_value_set_string (value, self->model_file);
      break;
    case PROP_OPTIMIZATION_LEVEL:
      g_value_set_enum (value, self->optimization_level);
      break;
    case PROP_EXECUTION_PROVIDER:
      g_value_set_enum (value, self->execution_provider);
      break;
    case PROP_INPUT_IMAGE_FORMAT:
      g_value_set_enum (value, onnxClient->getInputImageFormat ());
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_onnx_super_resolution_create_session (GstBaseTransform * trans)
{
  GstOnnxSuperResolution *self = GST_ONNX_SUPER_RESOLUTION (trans);
  auto onnxClient = GST_ONNX_MEMBER (self);

  GST_OBJECT_LOCK (self);
  if (self->onnx_disabled || onnxClient->hasSession ()) {
    GST_OBJECT_UNLOCK (self);

    return TRUE;
  }
  if (self->model_file) {
    gboolean ret = GST_ONNX_MEMBER (self)->createSession (self->model_file,
        self->optimization_level,
        self->execution_provider);
    if (!ret) {
      GST_ERROR_OBJECT (self,
          "Unable to create ONNX session. Detection disabled.");
    } else {
      // model is not usable, so fail
      if (self->onnx_disabled) {
        GST_ELEMENT_WARNING (self, RESOURCE, FAILED,
            ("ONNX model cannot be used for object detection"), (NULL));

        return FALSE;
      }
    }
  } else {
    self->onnx_disabled = TRUE;
  }
  GST_OBJECT_UNLOCK (self);
  if (self->onnx_disabled) {
    gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (self), TRUE);
  }

  return TRUE;
}

static GstCaps *
gst_onnx_super_resolution_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *ret;
  GstStructure *structure;
  GstCapsFeatures *features;
  gint i, n;
  auto self = GST_ONNX_SUPER_RESOLUTION (trans);
  auto onnxClient = GST_ONNX_MEMBER (self);

  if (!gst_onnx_super_resolution_create_session (trans))
    return NULL;

  if (gst_base_transform_is_passthrough (trans)
      || (!onnxClient->isFixedInputImageSize ()))
    return gst_caps_ref (caps);


  GST_DEBUG_OBJECT (trans,
      "Transforming caps %" GST_PTR_FORMAT " in direction %s", caps,
      (direction == GST_PAD_SINK) ? "sink" : "src");

  ret = gst_caps_new_empty ();
  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (caps, i);
    features = gst_caps_get_features (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure_full (ret, structure, features))
      continue;

    /* make copy */
    structure = gst_structure_copy (structure);

    /* If the features are non-sysmem we can only do passthrough */
    if (!gst_caps_features_is_any (features)
        && gst_caps_features_is_equal (features,
            GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)) {
      gint i;
      gint w = 0;
      gint h = 0;

      gst_structure_get_int (structure, "width", &w);
      gst_structure_get_int (structure, "height", &h);

      if (w || h) {

        for (i = 0; i < G_N_ELEMENTS (RATIOS); i++) {
          GstStructure *s2;

          g_assert (structure != NULL);

          if (i != G_N_ELEMENTS (RATIOS) - 1)
            s2 = gst_structure_copy (structure);
          else
            s2 = g_steal_pointer (&structure);

          if (direction == GST_PAD_SINK) {
            if (w)
              gst_structure_set (s2, "width", G_TYPE_INT, w * RATIOS[i], NULL);
            if (h)
              gst_structure_set (s2, "height", G_TYPE_INT, h * RATIOS[i], NULL);
          } else {
            if (w)
              gst_structure_set (s2, "width", G_TYPE_INT, w / RATIOS[i], NULL);
            if (h)
              gst_structure_set (s2, "height", G_TYPE_INT, h / RATIOS[i], NULL);
          }

          gst_caps_append_structure_full (ret, s2,
              gst_caps_features_copy (features));
        }
      }

      if (structure)
        gst_structure_set (structure, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
    }
    if (structure)
      gst_caps_append_structure_full (ret, structure,
          gst_caps_features_copy (features));
  }

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = intersection;
  }

  GST_DEBUG_OBJECT (trans, "returning caps: %" GST_PTR_FORMAT, ret);

  return ret;
}

static gboolean
gst_onnx_super_resolution_set_info (GstVideoFilter * filter, GstCaps * in,
    GstVideoInfo * in_info, GstCaps * out, GstVideoInfo * out_info)
{
  GstOnnxSuperResolution *videoscale = GST_ONNX_SUPER_RESOLUTION (filter);

  if (in_info->width == out_info->width && in_info->height == out_info->height) {
    gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (filter), TRUE);
  } else {
    //GstStructure *options;
    GST_CAT_DEBUG_OBJECT (CAT_PERFORMANCE, filter, "setup videoscaling");
    gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (filter), FALSE);

    /*
     * SUPERRES: Replace this with the init of algo
     */
/*
    options = gst_structure_new ("videoscale",
        GST_VIDEO_CONVERTER_OPT_RESAMPLER_METHOD,
        GST_TYPE_VIDEO_RESAMPLER_METHOD, GST_VIDEO_RESAMPLER_METHOD_NEAREST,
        GST_VIDEO_RESAMPLER_OPT_ENVELOPE, G_TYPE_DOUBLE, 2.0,
        GST_VIDEO_RESAMPLER_OPT_SHARPNESS, G_TYPE_DOUBLE, 1.0,
        GST_VIDEO_RESAMPLER_OPT_SHARPEN, G_TYPE_DOUBLE, 0.0,
        GST_VIDEO_CONVERTER_OPT_DEST_X, G_TYPE_INT, 0,
        GST_VIDEO_CONVERTER_OPT_DEST_Y, G_TYPE_INT, 0,
        GST_VIDEO_CONVERTER_OPT_DEST_WIDTH, G_TYPE_INT, out_info->width,
        GST_VIDEO_CONVERTER_OPT_DEST_HEIGHT, G_TYPE_INT, out_info->height,
        GST_VIDEO_CONVERTER_OPT_MATRIX_MODE, GST_TYPE_VIDEO_MATRIX_MODE,
        GST_VIDEO_MATRIX_MODE_NONE, GST_VIDEO_CONVERTER_OPT_DITHER_METHOD,
        GST_TYPE_VIDEO_DITHER_METHOD, GST_VIDEO_DITHER_NONE,
        GST_VIDEO_CONVERTER_OPT_CHROMA_MODE, GST_TYPE_VIDEO_CHROMA_MODE,
        GST_VIDEO_CHROMA_MODE_NONE,
        GST_VIDEO_CONVERTER_OPT_THREADS, G_TYPE_UINT, 1, NULL);

   videoscale->convert = gst_video_converter_new (in_info, out_info, options);
*/
    /* End init */
  }

  GST_DEBUG_OBJECT (videoscale, "from=%dx%d (par=%d/%d), size %"
      G_GSIZE_FORMAT " -> to=%dx%d (par=%d/%d), size %" G_GSIZE_FORMAT,
      in_info->width, in_info->height, in_info->par_n, in_info->par_d,
      in_info->size, out_info->width, out_info->height, out_info->par_n,
      out_info->par_d, out_info->size);

  return TRUE;
}


static gboolean
gst_onnx_super_resolution_src_event (GstBaseTransform * trans, GstEvent * event)
{
  GstOnnxSuperResolution *videoscale = GST_ONNX_SUPER_RESOLUTION (trans);
  GstVideoFilter *filter = GST_VIDEO_FILTER_CAST (trans);
  gboolean ret;
  gdouble a;
  GstStructure *structure;

  GST_DEBUG_OBJECT (videoscale, "handling %s event",
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NAVIGATION:
      if (filter->in_info.width != filter->out_info.width ||
          filter->in_info.height != filter->out_info.height) {
        event =
            GST_EVENT (gst_mini_object_make_writable (GST_MINI_OBJECT (event)));

        structure = (GstStructure *) gst_event_get_structure (event);
        if (gst_structure_get_double (structure, "pointer_x", &a)) {
          gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE,
              a * filter->in_info.width / filter->out_info.width, NULL);
        }
        if (gst_structure_get_double (structure, "pointer_y", &a)) {
          gst_structure_set (structure, "pointer_y", G_TYPE_DOUBLE,
              a * filter->in_info.height / filter->out_info.height, NULL);
        }
      }
      break;
    default:
      break;
  }

  ret = GST_BASE_TRANSFORM_CLASS (parent_class)->src_event (trans, event);

  return ret;
}

static GstCaps *
gst_onnx_super_resolution_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstStructure *ins, *outs;
  gint from_w, from_h;
  gint w = 0, h = 0;

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);

  GST_DEBUG_OBJECT (base, "trying to fixate othercaps %" GST_PTR_FORMAT
      " based on caps %" GST_PTR_FORMAT, othercaps, caps);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);


  gst_structure_get_int (ins, "width", &from_w);
  gst_structure_get_int (ins, "height", &from_h);

  gst_structure_get_int (outs, "width", &w);
  gst_structure_get_int (outs, "height", &h);

  /* if both width and height are already fixed, we can't do anything
   * about it anymore */
  if (w && h) {
    GST_DEBUG_OBJECT (base, "dimensions already set to %dx%d, not fixating",
        w, h);
    goto done;
  }

  /* If either width or height are fixed there's not much we
   * can do either except choosing a height or width and PAR
   * that matches the DAR as good as possible
   */
  if (h) {
    GST_DEBUG_OBJECT (base, "height is fixed (%d)", h);

    w = (guint) gst_util_uint64_scale_int_round (h, from_w, from_h);
    gst_structure_set (outs, "width", G_TYPE_INT, w, NULL);
  } else if (w) {
    GST_DEBUG_OBJECT (base, "width is fixed (%d)", w);

    h = (guint) gst_util_uint64_scale_int_round (w, from_h, from_w);
    gst_structure_set (outs, "height", G_TYPE_INT, h, NULL);
  } else {
    gst_structure_fixate_field_nearest_int (outs, "height", from_h);
    gst_structure_get_int (outs, "height", &h);
    w = (guint) gst_util_uint64_scale_int_round (h, from_w, from_h);
    gst_structure_set (outs, "width", G_TYPE_INT, w, NULL);
  }

done:
  GST_DEBUG_OBJECT (base, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);

  return othercaps;
}

static GstFlowReturn
gst_onnx_super_resolution_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame)
{
  GstFlowReturn ret = GST_FLOW_OK;

  GST_CAT_DEBUG_OBJECT (CAT_PERFORMANCE, filter, "doing video scaling");

  if (!gst_base_transform_is_passthrough (GST_BASE_TRANSFORM_CAST(filter))
      && !gst_onnx_super_resolution_process (GST_BASE_TRANSFORM_CAST(filter), in_frame->buffer)){
	    GST_ELEMENT_WARNING (filter, STREAM, FAILED,
          ("ONNX object detection failed"), (NULL));
	    return GST_FLOW_ERROR;
  }

  gst_buffer_replace(&out_frame->buffer, in_frame->buffer);

  return ret;
}


static gboolean
gst_onnx_super_resolution_process (GstBaseTransform * trans, GstBuffer * buf)
{
  GstMapInfo info;
  GstVideoMeta *vmeta = gst_buffer_get_video_meta (buf);

  if (!vmeta) {
    GST_WARNING_OBJECT (trans, "missing video meta");
    return FALSE;
  }
  if (gst_buffer_map (buf, &info, GST_MAP_READ)) {
    GstOnnxSuperResolution *self = GST_ONNX_SUPER_RESOLUTION (trans);
    auto boxes = GST_ONNX_MEMBER (self)->runSuperResolution (info.data, vmeta);
    gst_buffer_unmap (buf, &info);
  }

  return TRUE;
}
