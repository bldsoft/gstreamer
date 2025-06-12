/* GStreamer
 * Copyright (C) 2011 Alessandro Decina <alessandro.d@gmail.com>
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
 * SECTION:element-hlssink
 * @title: hlssink
 *
 * HTTP Live Streaming sink/server
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 videotestsrc is-live=true ! x264enc ! mpegtsmux ! hlssink max-files=5
 * ]|
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gsthlselements.h"
#include "gsthlssink.h"
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include <glib/gstdio.h>
#include <glib.h>
#include <memory.h>
#if defined(HAVE_OPENSSL)
#include <openssl/evp.h>
#elif defined(HAVE_NETTLE)
#include <nettle/aes.h>
#include <nettle/cbc.h>
#else
#include <gcrypt.h>
#endif

GST_DEBUG_CATEGORY_STATIC (gst_hls_sink_debug);
#define GST_CAT_DEFAULT gst_hls_sink_debug

#define DEFAULT_LOCATION "segment%05d.ts"
#define DEFAULT_PLAYLIST_LOCATION "playlist.m3u8"
#define DEFAULT_PLAYLIST_ROOT NULL
#define DEFAULT_MAX_FILES 10
#define DEFAULT_TARGET_DURATION 15
#define DEFAULT_PLAYLIST_LENGTH 5
#define DEFAULT_ENCRYPTION_METHOD 0     /* no encryption */
#define DEFAULT_KEY_LOCATION "playlist.key"
#define DEFAULT_KEY_URI "playlist.key"
#define DEFAULT_PROGRAM_DATE_TIME_MODE GST_HLS_PROGRAM_DATE_TIME_NEVER
#define DEFAULT_PROGRAM_DATE_TIME_SHIFT 0
#define DEFAULT_RESET_INDEX_ON_STOP TRUE
#define DEFAULT_START_MEDIA_SEQUENCE -1
#define DEFAULT_START_DISCONTINUETY_SEQUENCE 0

#define GST_M3U8_PLAYLIST_VERSION 3

#define AES_BLOCK_SIZE 16
#define ENCRYPTED_CHUNK_FILENAME "encrypted"
#define CHUNK_BLOCK_SIZE 1024

#if defined(HAVE_OPENSSL)
# if OPENSSL_VERSION_NUMBER < 0x10100000L
typedef EVP_CIPHER_CTX crypto_ctx_t;
# else
typedef EVP_CIPHER_CTX *crypto_ctx_t;
# endif
#elif defined(HAVE_NETTLE)
typedef struct CBC_CTX (struct aes_ctx, AES_BLOCK_SIZE) crypto_ctx_t;
#else
typedef gcry_cipher_hd_t crypto_ctx_t;
#endif

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_PLAYLIST_LOCATION,
  PROP_PLAYLIST_ROOT,
  PROP_MAX_FILES,
  PROP_TARGET_DURATION,
  PROP_PLAYLIST_LENGTH,
  PROP_ENCRYPTION_METHOD,
  PROP_KEY_LOCATION,
  PROP_KEY_URI,
  PROP_GSTM3U8PLAYLIST_H_PATCH_VERSION,
  PROP_GSTM3U8PLAYLIST_C_PATCH_VERSION,
  PROP_GSTHLSSINK_H_PATCH_VERSION,
  PROP_GSTHLSSINK_C_PATCH_VERSION,
  PROP_PROGRAM_DATE_TIME_MODE,
  PROP_PROGRAM_DATE_TIME_SHIFT,
  PROP_RESET_INDEX_ON_STOP,
  PROP_START_MEDIA_SEQUENCE,
  PROP_START_DISCONTINUITY_SEQUENCE
};

const int RIXJOB_GSTHLSSINK_H_PATCH_VERSION = 3;
const int RIXJOB_GSTHLSSINK_C_PATCH_VERSION = 3;

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define gst_hls_sink_parent_class parent_class
G_DEFINE_TYPE (GstHlsSink, gst_hls_sink, GST_TYPE_BIN);
#define _do_init \
  hls_element_init (plugin); \
  GST_DEBUG_CATEGORY_INIT (gst_hls_sink_debug, "hlssink", 0, "HlsSink");
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (hlssink, "hlssink", GST_RANK_NONE,
    GST_TYPE_HLS_SINK, _do_init);

static void gst_hls_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * spec);
static void gst_hls_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * spec);
static void gst_hls_sink_handle_message (GstBin * bin, GstMessage * message);
static GstPadProbeReturn gst_hls_sink_ghost_event_probe (GstPad * pad,
    GstPadProbeInfo * info, gpointer data);
static GstPadProbeReturn gst_hls_sink_ghost_buffer_probe (GstPad * pad,
    GstPadProbeInfo * info, gpointer data);
static void gst_hls_sink_reset (GstHlsSink * sink);
static GstStateChangeReturn
gst_hls_sink_change_state (GstElement * element, GstStateChange trans);
static gboolean schedule_next_key_unit (GstHlsSink * sink);
static GstFlowReturn gst_hls_sink_chain_list (GstPad * pad, GstObject * parent,
    GstBufferList * list);
static void gst_hls_sink_open_key (GstHlsSink * sink);
static void gst_hls_sink_create_iv (GstHlsSink * sink, unsigned char *iv);
static gboolean gst_hls_sink_init_encrypting (GstHlsSink * sink,
    crypto_ctx_t * hd);
static void gst_hls_sink_encrypt_chunk (GstHlsSink * sink,
    const gchar * filename);
#if !defined(HAVE_OPENSSL)
static int add_padding (unsigned char *buf, int data_size);
#endif

enum
{
  GST_HLS_SINK_ENC_NONE,
  GST_HLS_SINK_ENC_AES_128
};

#define GST_HLS_SINK_ENCRYPTION_METHOD_TYPE \
  (gst_hls_sink_encryption_method_get_type())

static GType
gst_hls_sink_encryption_method_get_type (void)
{
  static GType encryption_type = 0;

  static const GEnumValue encryption_method_types[] = {
    {GST_HLS_SINK_ENC_NONE, "No encryption", "none"},
    {GST_HLS_SINK_ENC_AES_128, "AES-128 encryption", "aes-128"},
    {0, NULL, NULL}
  };

  if (!encryption_type)
    encryption_type = g_enum_register_static ("GstHlsSinkEncryptionType",
        encryption_method_types);

  return encryption_type;
}

static void
gst_hls_sink_dispose (GObject * object)
{
  GstHlsSink *sink = GST_HLS_SINK_CAST (object);

  G_OBJECT_CLASS (parent_class)->dispose ((GObject *) sink);
}

static void
gst_hls_sink_finalize (GObject * object)
{
  GstHlsSink *sink = GST_HLS_SINK_CAST (object);

  g_free (sink->location);
  g_free (sink->playlist_location);
  g_free (sink->playlist_root);
  g_free (sink->key_location);
  if (sink->playlist)
    gst_m3u8_playlist_free (sink->playlist);
  if (sink->start_time)
    g_date_time_unref (sink->start_time);

  G_OBJECT_CLASS (parent_class)->finalize ((GObject *) sink);
}

static void
gst_hls_sink_class_init (GstHlsSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBinClass *bin_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);
  bin_class = GST_BIN_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_template);

  gst_element_class_set_static_metadata (element_class,
      "HTTP Live Streaming sink", "Sink", "HTTP Live Streaming sink",
      "Alessandro Decina <alessandro.d@gmail.com>");

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_hls_sink_change_state);

  bin_class->handle_message = gst_hls_sink_handle_message;

  gobject_class->dispose = gst_hls_sink_dispose;
  gobject_class->finalize = gst_hls_sink_finalize;
  gobject_class->set_property = gst_hls_sink_set_property;
  gobject_class->get_property = gst_hls_sink_get_property;

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "File Location",
          "Location of the file to write", DEFAULT_LOCATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PLAYLIST_LOCATION,
      g_param_spec_string ("playlist-location", "Playlist Location",
          "Location of the playlist to write", DEFAULT_PLAYLIST_LOCATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PLAYLIST_ROOT,
      g_param_spec_string ("playlist-root", "Playlist Root",
          "Location of the playlist to write", DEFAULT_PLAYLIST_ROOT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_FILES,
      g_param_spec_uint ("max-files", "Max files",
          "Maximum number of files to keep on disk. Once the maximum is reached,"
          "old files start to be deleted to make room for new ones.",
          0, G_MAXUINT, DEFAULT_MAX_FILES,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_TARGET_DURATION,
      g_param_spec_uint ("target-duration", "Target duration",
          "The target duration in seconds of a segment/file. "
          "(0 - disabled, useful for management of segment duration by the "
          "streaming server)",
          0, G_MAXUINT, DEFAULT_TARGET_DURATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PLAYLIST_LENGTH,
      g_param_spec_uint ("playlist-length", "Playlist length",
          "Length of HLS playlist. To allow players to conform to section 6.3.3 "
          "of the HLS specification, this should be at least 3. If set to 0, "
          "the playlist will be infinite.",
          0, G_MAXUINT, DEFAULT_PLAYLIST_LENGTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ENCRYPTION_METHOD,
      g_param_spec_enum ("encryption-method", "Encryption method",
          "Encryption method",
          GST_HLS_SINK_ENCRYPTION_METHOD_TYPE, DEFAULT_ENCRYPTION_METHOD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_KEY_LOCATION,
      g_param_spec_string ("key-location", "Encryption key location",
          "Location of the key for decrypting chunks", DEFAULT_KEY_LOCATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_KEY_URI,
      g_param_spec_string ("key-uri", "Key URI",
          "URI that specifies how to obtain the key", DEFAULT_KEY_URI,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_GSTM3U8PLAYLIST_H_PATCH_VERSION,
      g_param_spec_uint ("gstm3u8playlist-h-patch-version",
          "Version of pathch for gstm3u8playlist.h file",
          "gstm3u8playlist.h patch version",
          0, G_MAXUINT, RIXJOB_GSTM3U8PLAYLIST_H_PATCH_VERSION,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_GSTM3U8PLAYLIST_C_PATCH_VERSION,
      g_param_spec_uint ("gstm3u8playlist-c-patch-version",
          "Version of patch for gstm3u8playlist.c file",
          "gstm3u8playlist.c patch version",
          0, G_MAXUINT, RIXJOB_GSTM3U8PLAYLIST_C_PATCH_VERSION,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_GSTHLSSINK_H_PATCH_VERSION,
      g_param_spec_uint ("gsthlssink-h-patch-version",
          "Version of patch for gsthlssink.h file",
          "gsthlssink.h patch version",
          0, G_MAXUINT, RIXJOB_GSTHLSSINK_H_PATCH_VERSION,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_GSTHLSSINK_C_PATCH_VERSION,
      g_param_spec_uint ("gsthlssink-c-patch-version",
          "Version of patch for gsthlssink.c file",
          "gsthlssink.c patch version", 0, G_MAXUINT,
          RIXJOB_GSTHLSSINK_C_PATCH_VERSION,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PROGRAM_DATE_TIME_MODE,
      g_param_spec_enum ("program-date-time-mode",
          "Mode for #EXT-X-PROGRAM-DATE-TIME tag",
          "When to show #EXT-X-PROGRAM-DATE-TIME tag (YYYY-MM-DDThh:mm:ss.SSSZ)",
          GST_HLS_PROGRAM_DATE_TIME_MODE_TYPE, DEFAULT_PROGRAM_DATE_TIME_MODE,
          G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_PROGRAM_DATE_TIME_SHIFT,
      g_param_spec_int64 ("program-date-time-shift", "PROGRAM-DATE-TIME shift",
          "PROGRAM-DATE-TIME shift in nanoseconds", G_MININT64, G_MAXINT64,
          DEFAULT_PROGRAM_DATE_TIME_SHIFT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_RESET_INDEX_ON_STOP,
      g_param_spec_boolean ("reset-index", "Reset index on stop",
          "Reset index on stop", DEFAULT_RESET_INDEX_ON_STOP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_START_MEDIA_SEQUENCE,
      g_param_spec_uint ("start-media-sequence", "Start media sequence",
          "Initial value for EXT-X-MEDIA-SEQUENCE.",
          0, G_MAXUINT, DEFAULT_START_MEDIA_SEQUENCE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_START_DISCONTINUITY_SEQUENCE,
      g_param_spec_uint ("start-discontinuity-sequence",
          "Start discontinuity sequence",
          "Initial value for EXT-X-DISCONTINUITY-SEQUENCE.", 0, G_MAXUINT,
          DEFAULT_START_DISCONTINUETY_SEQUENCE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_hls_sink_init (GstHlsSink * sink)
{
  GstPadTemplate *templ = gst_static_pad_template_get (&sink_template);
  sink->ghostpad = gst_ghost_pad_new_no_target_from_template ("sink", templ);
  gst_object_unref (templ);
  gst_element_add_pad (GST_ELEMENT_CAST (sink), sink->ghostpad);
  gst_pad_add_probe (sink->ghostpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      gst_hls_sink_ghost_event_probe, sink, NULL);
  gst_pad_add_probe (sink->ghostpad, GST_PAD_PROBE_TYPE_BUFFER,
      gst_hls_sink_ghost_buffer_probe, sink, NULL);
  gst_pad_set_chain_list_function (sink->ghostpad, gst_hls_sink_chain_list);

  sink->location = g_strdup (DEFAULT_LOCATION);
  sink->playlist_location = g_strdup (DEFAULT_PLAYLIST_LOCATION);
  sink->playlist_root = g_strdup (DEFAULT_PLAYLIST_ROOT);
  sink->playlist_length = DEFAULT_PLAYLIST_LENGTH;
  sink->max_files = DEFAULT_MAX_FILES;
  sink->target_duration = DEFAULT_TARGET_DURATION;
  sink->encryption_method = DEFAULT_ENCRYPTION_METHOD;
  sink->key_location = g_strdup (DEFAULT_KEY_LOCATION);
  sink->key_uri = g_strdup (DEFAULT_KEY_URI);
  sink->is_reset_index_on_stop = DEFAULT_RESET_INDEX_ON_STOP;
  sink->index = DEFAULT_START_MEDIA_SEQUENCE;
  /* haven't added a sink yet, make it is detected as a sink meanwhile */
  GST_OBJECT_FLAG_SET (sink, GST_ELEMENT_FLAG_SINK);

  gst_hls_sink_reset (sink);
}

static void
gst_hls_sink_reset (GstHlsSink * sink)
{
  if (sink->is_reset_index_on_stop) {
    sink->index = DEFAULT_START_MEDIA_SEQUENCE;
    if (sink->playlist) {
      gst_m3u8_playlist_free (sink->playlist);
      sink->playlist = NULL;
    }
  }
  if (!sink->playlist) {
    sink->playlist =
        gst_m3u8_playlist_new (GST_M3U8_PLAYLIST_VERSION, sink->playlist_length);
  }
  sink->last_running_time = 0;
  sink->waiting_fku = FALSE;
  gst_event_replace (&sink->force_key_unit_event, NULL);
  gst_segment_init (&sink->segment, GST_FORMAT_UNDEFINED);

  if (sink->start_time) {
    g_date_time_unref (sink->start_time);
    sink->start_time = NULL;
  }

  sink->state = GST_M3U8_PLAYLIST_RENDER_INIT;
}

static gboolean
gst_hls_sink_create_elements (GstHlsSink * sink)
{
  GstPad *pad = NULL;

  GST_DEBUG_OBJECT (sink, "Creating internal elements");

  if (sink->elements_created)
    return TRUE;

  sink->multifilesink = gst_element_factory_make ("multifilesink", NULL);
  if (sink->multifilesink == NULL)
    goto missing_element;

  g_object_set (sink->multifilesink, "location", sink->location,
      "next-file", 3, "post-messages", TRUE, "max-files", sink->max_files,
      NULL);

  gst_bin_add (GST_BIN_CAST (sink), sink->multifilesink);

  pad = gst_element_get_static_pad (sink->multifilesink, "sink");
  gst_ghost_pad_set_target (GST_GHOST_PAD (sink->ghostpad), pad);
  gst_object_unref (pad);

  sink->elements_created = TRUE;
  return TRUE;

missing_element:
  gst_element_post_message (GST_ELEMENT_CAST (sink),
      gst_missing_element_message_new (GST_ELEMENT_CAST (sink),
          "multifilesink"));
  GST_ELEMENT_ERROR (sink, CORE, MISSING_PLUGIN,
      (("Missing element '%s' - check your GStreamer installation."),
          "multifilesink"), (NULL));
  return FALSE;
}

static void
gst_hls_sink_open_key (GstHlsSink * sink)
{
  FILE *key_file = fopen (sink->key_location, "rb");
  if (!key_file) {
    GST_ERROR_OBJECT (sink, "failed to open encryption key %s",
        sink->key_location);
    return;
  }
  if (fread (sink->key, 1, AES_BLOCK_SIZE, key_file) != AES_BLOCK_SIZE) {
    GST_ERROR_OBJECT (sink, "invalid encryption key");
  }
  fclose (key_file);
}

static void
gst_hls_sink_create_iv (GstHlsSink * sink, unsigned char *iv)
{
  guint index_be = GUINT_TO_BE (sink->index);
  unsigned char *raw = (unsigned char *) &index_be;

  for (int i = 0; i != sizeof (guint); ++i)
    iv[AES_BLOCK_SIZE - sizeof (guint) + i] = raw[i];
}

static gboolean
gst_hls_sink_init_encrypting (GstHlsSink * sink, crypto_ctx_t * hd)
{
  unsigned char iv[AES_BLOCK_SIZE] = { 0 };
  gst_hls_sink_create_iv (sink, iv);

#if defined(HAVE_OPENSSL)
# if OPENSSL_VERSION_NUMBER < 0x10100000L
  EVP_CIPHER_CTX_init (hd);
# else
  *hd = EVP_CIPHER_CTX_new ();
#endif
  if (!EVP_EncryptInit_ex (hd, EVP_aes_128_cbc (), NULL, sink->key, iv))
    return FALSE;
#elif defined(HAVE_NETTLE)
  aes_set_encrypt_key (&hd->ctx, AES_BLOCK_SIZE, sink->key);

  CBC_SET_IV (hd, iv);
#else //gcrypt
  gcry_error_t err;
  err = gcry_cipher_open (hd, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_CBC, 0);
  if (err) {
    GST_ERROR_OBJECT (sink, "failed to open cipher: %s / %s",
        gcry_strsource (err), gcry_strerror (err));
    return FALSE;
  }

  err = gcry_cipher_setkey (*hd, sink->key, AES_BLOCK_SIZE);
  if (err) {
    GST_ERROR_OBJECT (sink, "failed to set key for cipher: %s / %s",
        gcry_strsource (err), gcry_strerror (err));
    return FALSE;
  }

  err = gcry_cipher_setiv (*hd, iv, sizeof (iv));
  if (err) {
    GST_ERROR_OBJECT (sink, "failed to set iv for cipher: %s / %s",
        gcry_strsource (err), gcry_strerror (err));
    return FALSE;
  }
#endif //defined(HAVE_NETTLE)
  return TRUE;
}

#if !defined (HAVE_OPENSSL)
static int
add_padding (unsigned char *buf, int size)
{
  int padding = AES_BLOCK_SIZE - size % AES_BLOCK_SIZE;
  for (int i = 0; i != padding; ++i)
    buf[size + i] = padding;
  return size + padding;
}
#endif

static void
gst_hls_sink_encrypt_chunk (GstHlsSink * sink, const gchar * filename)
{
  char *encrypted_chunk_path = NULL, *root_dir = NULL;
  FILE *chunk = NULL, *encrypted = NULL;
  unsigned char *in = NULL, *out = NULL;
  struct stat stats;
  int size, final_size;

#if defined(HAVE_OPENSSL)
  EVP_CIPHER_CTX *ctx;
  crypto_ctx_t aes_ctx;
#elif defined(HAVE_NETTLE)
  crypto_ctx_t aes_ctx;
#else // gcrypt
  gcry_error_t err;
  crypto_ctx_t aes_ctx;
#endif

  if (!gst_hls_sink_init_encrypting (sink, &aes_ctx))
    goto cleanup;

#if defined(HAVE_OPENSSL)
# if OPENSSL_VERSION_NUMBER < 0x10100000L
  ctx = &aes_ctx;
# else
  ctx = aes_ctx;
# endif
#endif

  chunk = fopen (filename, "rb");
  root_dir = g_path_get_dirname (filename);
  encrypted_chunk_path = g_build_filename (root_dir, "encrypted", NULL);
  encrypted = fopen (encrypted_chunk_path, "wb");
  if (!chunk || !encrypted) {
    GST_ERROR_OBJECT (sink, "failed to open %s: %s",
        (!chunk ? filename : encrypted_chunk_path), strerror (errno));
    goto cleanup;
  }

  if (stat (filename, &stats) < 0) {
    GST_ERROR_OBJECT (sink, "failed to get stats for %s: %s", filename,
        strerror (errno));
    goto cleanup;
  }
  in = malloc (stats.st_size + AES_BLOCK_SIZE);
  out = malloc (stats.st_size + AES_BLOCK_SIZE);
  size = fread (in, 1, stats.st_size, chunk);
  if (size == 0 && ferror (chunk)) {
    GST_ERROR_OBJECT (sink, "failed to read from chunk %s: %s", filename,
        strerror (errno));
    goto cleanup;
  }
#if !defined(HAVE_OPENSSL)
  final_size = add_padding (in, size);
#endif

#if defined(HAVE_OPENSSL)
  if (!EVP_EncryptUpdate (ctx, out, &size, in, size))
    goto cleanup;
  EVP_EncryptFinal_ex (ctx, out + size, &final_size);
  final_size += size;
#elif defined(HAVE_NETTLE)
  CBC_ENCRYPT (&aes_ctx, aes_encrypt, final_size, out, in);
#else
  err = gcry_cipher_encrypt (aes_ctx, out, stats.st_size + AES_BLOCK_SIZE, in,
      final_size);
  if (err) {
    GST_ERROR_OBJECT (sink, "failed to encrypt buffer: %s / %s",
        gcry_strsource (err), gcry_strerror (err));
    goto cleanup;
  }
#endif

  if (fwrite (out, 1, final_size, encrypted) != final_size) {
    GST_ERROR_OBJECT (sink, "failed to write encrypted buffer");
    goto cleanup;
  }

  if (rename (encrypted_chunk_path, filename) < 0)
    GST_ERROR_OBJECT (sink, "failed to rename encrypted chunk: %s",
        strerror (errno));

cleanup:
  g_free (encrypted_chunk_path);
  g_free (root_dir);
  if (chunk)
    fclose (chunk);
  if (encrypted)
    fclose (encrypted);
  g_free (out);
  g_free (in);

#if defined (HAVE_OPENSSL)
# if OPENSSL_VERSION_NUMBER < 0x10100000L
  EVP_CIPHER_CTX_cleanup (&aes_ctx);
# else
  EVP_CIPHER_CTX_free (aes_ctx);
# endif
#elif defined(HAVE_NETTLE)
  /* NOP */
#else
  gcry_cipher_close (aes_ctx);
#endif
}

static void
gst_hls_sink_write_playlist (GstHlsSink * sink)
{
  char *playlist_content;
  GError *error = NULL;

  playlist_content = gst_m3u8_playlist_render (sink->playlist);
  if (!g_file_set_contents (sink->playlist_location,
          playlist_content, -1, &error)) {
    GST_ERROR ("Failed to write playlist: %s", error->message);
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE,
        (("Failed to write playlist '%s'."), error->message), (NULL));
    g_error_free (error);
    error = NULL;
  }
  g_free (playlist_content);

}

static void
gst_hls_sink_handle_message (GstBin * bin, GstMessage * message)
{
  GstHlsSink *sink = GST_HLS_SINK_CAST (bin);

  switch (message->type) {
    case GST_MESSAGE_ELEMENT:
    {
      const char *filename;
      GstClockTime running_time, duration;
      gboolean discont = FALSE;
      gchar *entry_location;
      const GstStructure *structure;
      GDateTime *program_date_time;
      double chunk_time;

      structure = gst_message_get_structure (message);
      if (strcmp (gst_structure_get_name (structure), "GstMultiFileSink"))
        break;

      filename = gst_structure_get_string (structure, "filename");
      gst_structure_get_clock_time (structure, "running-time", &running_time);
      duration = running_time - sink->last_running_time;

      chunk_time = sink->last_running_time / (double) GST_SECOND +
          sink->program_date_time_shift / (double) GST_SECOND;
      program_date_time = g_date_time_add_seconds (sink->start_time,
          chunk_time);

      sink->last_running_time = running_time;

      GST_INFO_OBJECT (sink, "COUNT %d", sink->index);
      if (sink->playlist_root == NULL)
        entry_location = g_path_get_basename (filename);
      else {
        gchar *name = g_path_get_basename (filename);
        entry_location = g_build_filename (sink->playlist_root, name, NULL);
        g_free (name);
      }

      gst_m3u8_playlist_add_entry (sink->playlist, entry_location, NULL,
          duration, sink->index, discont, program_date_time);
      g_free (entry_location);

      if (sink->encryption_method != GST_HLS_SINK_ENC_NONE)
        gst_hls_sink_encrypt_chunk (sink, filename);

      gst_hls_sink_write_playlist (sink);
      sink->state |= GST_M3U8_PLAYLIST_RENDER_STARTED;

      /* multifilesink is starting a new file. It means that upstream sent a key
       * unit and we can schedule the next key unit now.
       */
      sink->waiting_fku = FALSE;
      schedule_next_key_unit (sink);

      /* multifilesink is an internal implementation detail. If applications
       * need a notification, we should probably do our own message */
      //GST_DEBUG_OBJECT (bin, "dropping message %" GST_PTR_FORMAT, message);
      //gst_message_unref (message);
      //message = NULL;
      break;
    }
    case GST_MESSAGE_EOS:{
      if (sink->is_reset_index_on_stop) {
        sink->playlist->end_list = TRUE;
      } else {
        gst_m3u8_playlist_add_discontinuity (sink->playlist);
        sink->index++;
        sink->playlist->sequence_number++;
      }
      gst_hls_sink_write_playlist (sink);
      sink->state |= GST_M3U8_PLAYLIST_RENDER_ENDED;
      break;
    }
    default:
      break;
  }

  if (message)
    GST_BIN_CLASS (parent_class)->handle_message (bin, message);
}

static GstStateChangeReturn
gst_hls_sink_change_state (GstElement * element, GstStateChange trans)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstHlsSink *sink = GST_HLS_SINK_CAST (element);

  switch (trans) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_hls_sink_create_elements (sink)) {
        return GST_STATE_CHANGE_FAILURE;
      }
      if (sink->encryption_method != GST_HLS_SINK_ENC_NONE) {
        gst_hls_sink_open_key (sink);
      }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      if (sink->start_time)
        g_date_time_unref (sink->start_time);
      sink->start_time = g_date_time_new_now_utc ();
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, trans);

  switch (trans) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      /* drain playlist with #EXT-X-ENDLIST */
      if (sink->playlist && (sink->state & GST_M3U8_PLAYLIST_RENDER_STARTED) &&
          !(sink->state & GST_M3U8_PLAYLIST_RENDER_ENDED)) {
        if (sink->is_reset_index_on_stop) {
          sink->playlist->end_list = TRUE;
        } else {
          gst_m3u8_playlist_add_discontinuity (sink->playlist);
          sink->index++;
          sink->playlist->sequence_number++;
        }
        gst_hls_sink_write_playlist (sink);
      }
      gst_hls_sink_reset (sink);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_hls_sink_reset (sink);
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_hls_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstHlsSink *sink = GST_HLS_SINK_CAST (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_free (sink->location);
      sink->location = g_value_dup_string (value);
      if (sink->multifilesink)
        g_object_set (sink->multifilesink, "location", sink->location, NULL);
      break;
    case PROP_PLAYLIST_LOCATION:
      g_free (sink->playlist_location);
      sink->playlist_location = g_value_dup_string (value);
      break;
    case PROP_PLAYLIST_ROOT:
      g_free (sink->playlist_root);
      sink->playlist_root = g_value_dup_string (value);
      break;
    case PROP_MAX_FILES:
      sink->max_files = g_value_get_uint (value);
      if (sink->multifilesink) {
        g_object_set (sink->multifilesink, "location", sink->location,
            "next-file", 3, "post-messages", TRUE, "max-files", sink->max_files,
            NULL);
      }
      break;
    case PROP_TARGET_DURATION:
      sink->target_duration = g_value_get_uint (value);
      break;
    case PROP_PLAYLIST_LENGTH:
      sink->playlist_length = g_value_get_uint (value);
      sink->playlist->window_size = sink->playlist_length;
      break;
    case PROP_ENCRYPTION_METHOD:
      sink->encryption_method = g_value_get_enum (value);
      sink->playlist->encryption_method = sink->encryption_method;
      break;
    case PROP_KEY_LOCATION:
      g_free (sink->key_location);
      sink->key_location = g_value_dup_string (value);
      break;
    case PROP_KEY_URI:
      g_free (sink->key_uri);
      sink->key_uri = g_value_dup_string (value);
      sink->playlist->key_location = sink->key_uri;
      break;
    case PROP_PROGRAM_DATE_TIME_MODE:
      sink->playlist->program_date_time_mode = g_value_get_enum (value);
      break;
    case PROP_PROGRAM_DATE_TIME_SHIFT:
      sink->program_date_time_shift = g_value_get_int64 (value);
      break;
    case PROP_RESET_INDEX_ON_STOP:
      sink->is_reset_index_on_stop = g_value_get_boolean (value);
      break;
    case PROP_START_MEDIA_SEQUENCE:
      sink->index = g_value_get_uint (value) - 1;
      break;
    case PROP_START_DISCONTINUITY_SEQUENCE:
      sink->playlist->discontinuity_sequence_number = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_hls_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstHlsSink *sink = GST_HLS_SINK_CAST (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, sink->location);
      break;
    case PROP_PLAYLIST_LOCATION:
      g_value_set_string (value, sink->playlist_location);
      break;
    case PROP_PLAYLIST_ROOT:
      g_value_set_string (value, sink->playlist_root);
      break;
    case PROP_MAX_FILES:
      g_value_set_uint (value, sink->max_files);
      break;
    case PROP_TARGET_DURATION:
      g_value_set_uint (value, sink->target_duration);
      break;
    case PROP_PLAYLIST_LENGTH:
      g_value_set_uint (value, sink->playlist_length);
      break;
    case PROP_ENCRYPTION_METHOD:
      g_value_set_enum (value, sink->encryption_method);
      break;
    case PROP_KEY_LOCATION:
      g_value_set_string (value, sink->key_location);
      break;
    case PROP_KEY_URI:
      g_value_set_string (value, sink->key_uri);
      break;
    case PROP_GSTM3U8PLAYLIST_H_PATCH_VERSION:
      g_value_set_uint (value, RIXJOB_GSTM3U8PLAYLIST_H_PATCH_VERSION);
      break;
    case PROP_GSTM3U8PLAYLIST_C_PATCH_VERSION:
      g_value_set_uint (value, RIXJOB_GSTM3U8PLAYLIST_C_PATCH_VERSION);
      break;
    case PROP_GSTHLSSINK_H_PATCH_VERSION:
      g_value_set_uint (value, RIXJOB_GSTHLSSINK_H_PATCH_VERSION);
      break;
    case PROP_GSTHLSSINK_C_PATCH_VERSION:
      g_value_set_uint (value, RIXJOB_GSTHLSSINK_C_PATCH_VERSION);
      break;
    case PROP_PROGRAM_DATE_TIME_MODE:
      g_value_set_enum (value, sink->playlist->program_date_time_mode);
      break;
    case PROP_PROGRAM_DATE_TIME_SHIFT:
      g_value_set_int64 (value, sink->program_date_time_shift);
      break;
    case PROP_RESET_INDEX_ON_STOP:
      g_value_set_boolean (value, sink->is_reset_index_on_stop);
      break;
    case PROP_START_MEDIA_SEQUENCE:
      g_value_set_uint (value, sink->playlist->sequence_number);
      break;
    case PROP_START_DISCONTINUITY_SEQUENCE:
      g_value_set_uint (value,
          gst_m3u8_playlist_get_discontinuity_number (sink->playlist));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstPadProbeReturn
gst_hls_sink_ghost_event_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer data)
{
  GstHlsSink *sink = GST_HLS_SINK_CAST (data);
  GstEvent *event = gst_pad_probe_info_get_event (info);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:
    {
      gst_event_copy_segment (event, &sink->segment);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      gst_segment_init (&sink->segment, GST_FORMAT_UNDEFINED);
      break;
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    {
      GstClockTime timestamp;
      GstClockTime running_time, stream_time;
      gboolean all_headers;
      guint count;

      if (!gst_video_event_is_force_key_unit (event))
        break;

      gst_event_replace (&sink->force_key_unit_event, event);
      gst_video_event_parse_downstream_force_key_unit (event,
          &timestamp, &stream_time, &running_time, &all_headers, &count);
      GST_INFO_OBJECT (sink, "setting index %d", count);
      sink->index = count;
      break;
    }
    default:
      break;
  }

  return GST_PAD_PROBE_OK;
}

static gboolean
schedule_next_key_unit (GstHlsSink * sink)
{
  gboolean res = TRUE;
  GstClockTime running_time;
  GstPad *sinkpad = gst_element_get_static_pad (GST_ELEMENT (sink), "sink");

  if (sink->target_duration == 0)
    /* target-duration == 0 means that the app schedules key units itself */
    goto out;

  running_time = sink->last_running_time + sink->target_duration * GST_SECOND;
  GST_INFO_OBJECT (sink, "sending upstream force-key-unit, index %d "
      "now %" GST_TIME_FORMAT " target %" GST_TIME_FORMAT,
      sink->index + 1, GST_TIME_ARGS (sink->last_running_time),
      GST_TIME_ARGS (running_time));

  if (!(res = gst_pad_push_event (sinkpad,
              gst_video_event_new_upstream_force_key_unit (running_time,
                  TRUE, sink->index + 1)))) {
    GST_ERROR_OBJECT (sink, "Failed to push upstream force key unit event");
  }

out:
  /* mark as waiting for a fku event if the app schedules them or if we just
   * successfully scheduled one
   */
  sink->waiting_fku = res;
  gst_object_unref (sinkpad);
  return res;
}

static void
gst_hls_sink_check_schedule_next_key_unit (GstHlsSink * sink, GstBuffer * buf)
{
  GstClockTime timestamp;

  timestamp = GST_BUFFER_TIMESTAMP (buf);
  if (!GST_CLOCK_TIME_IS_VALID (timestamp))
    return;

  sink->last_running_time = gst_segment_to_running_time (&sink->segment,
      GST_FORMAT_TIME, timestamp);
  schedule_next_key_unit (sink);
}

static GstPadProbeReturn
gst_hls_sink_ghost_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer data)
{
  GstHlsSink *sink = GST_HLS_SINK_CAST (data);
  GstBuffer *buffer = gst_pad_probe_info_get_buffer (info);

  if (sink->target_duration == 0 || sink->waiting_fku)
    return GST_PAD_PROBE_OK;

  gst_hls_sink_check_schedule_next_key_unit (sink, buffer);
  return GST_PAD_PROBE_OK;
}

static GstFlowReturn
gst_hls_sink_chain_list (GstPad * pad, GstObject * parent, GstBufferList * list)
{
  guint i, len;
  GstBuffer *buffer;
  GstFlowReturn ret;
  GstHlsSink *sink = GST_HLS_SINK_CAST (parent);

  if (sink->target_duration == 0 || sink->waiting_fku)
    return gst_proxy_pad_chain_list_default (pad, parent, list);

  GST_DEBUG_OBJECT (pad, "chaining each group in list as a merged buffer");

  len = gst_buffer_list_length (list);

  ret = GST_FLOW_OK;
  for (i = 0; i < len; i++) {
    buffer = gst_buffer_list_get (list, i);

    if (!sink->waiting_fku)
      gst_hls_sink_check_schedule_next_key_unit (sink, buffer);

    ret = gst_pad_chain (pad, gst_buffer_ref (buffer));
    if (ret != GST_FLOW_OK)
      break;
  }
  gst_buffer_list_unref (list);

  return ret;
}
