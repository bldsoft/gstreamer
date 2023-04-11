/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) 2021 Aaron Boxer <aaron.boxer@collabora.com>
 *
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

#ifndef __GST_ONNX_SUPER_RESOLUTION_H__
#define __GST_ONNX_SUPER_RESOLUTION_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include "gstonnxelement.h"

G_BEGIN_DECLS


#define GST_TYPE_ONNX_SUPER_RESOLUTION (gst_onnx_super_resolution_get_type())
G_DECLARE_FINAL_TYPE(GstOnnxSuperResolution, gst_onnx_super_resolution, GST,
    GST_ONNX_SUPER_RESOLUTION, GstVideoFilter);
#define GST_ONNX_SUPER_RESOLUTION(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_ONNX_SUPER_RESOLUTION,GstOnnxSuperResolution))
#define GST_ONNX_SUPER_RESOLUTION_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_ONNX_SUPER_RESOLUTION,GstOnnxSuperResolutionClass))
#define GST_ONNX_SUPER_RESOLUTION_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_ONNX_SUPER_RESOLUTION,GstOnnxSuperResolutionClass))
#define GST_IS_ONNX_SUPER_RESOLUTION(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_ONNX_SUPER_RESOLUTION))
#define GST_IS_ONNX_SUPER_RESOLUTION_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_ONNX_SUPER_RESOLUTION))


/**
 * GstOnnxSuperResolution:
 *
 * Private data structure
 */
struct _GstOnnxSuperResolution {
  GstVideoFilter element;

  gchar *model_file;
  GstOnnxOptimizationLevel optimization_level;
  GstOnnxExecutionProvider execution_provider;
  gpointer onnx_ptr;
  gboolean onnx_disabled;
};

GST_ELEMENT_REGISTER_DECLARE (onnxsuperresolution)


G_END_DECLS

#endif /* __GST_ONNX_SUPER_RESOLUTION_H__ */
