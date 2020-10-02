/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gstm3u8playlist.c:
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

#include <glib.h>
#include <math.h>

#include "gstm3u8playlist.h"
#include "gsthlselements.h"

#define GST_CAT_DEFAULT hls_debug

const int RIXJOB_GSTM3U8PLAYLIST_H_PATCH_VERSION = 2;
const int RIXJOB_GSTM3U8PLAYLIST_C_PATCH_VERSION = 2;

enum
{
  GST_M3U8_PLAYLIST_TYPE_EVENT,
  GST_M3U8_PLAYLIST_TYPE_VOD,
};

GType
gst_hls_program_date_time_mode_get_type (void)
{
  static GType program_date_time_mode = 0;
  static const GEnumValue program_date_time_modes[] = {
    {GST_HLS_PROGRAM_DATE_TIME_NEVER, "Don't show tag", "never"},
    {GST_HLS_PROGRAM_DATE_TIME_FIRST_CHUNK, "Show only for first chunk",
        "first"},
    {GST_HLS_PROGRAM_DATE_TIME_ALL_CHUNKS, "Show for each chunk", "all"},
    {0, NULL, NULL}
  };

  if (!program_date_time_mode)
    program_date_time_mode =
        g_enum_register_static ("GstHlsSinkProgramDateMode",
        program_date_time_modes);

  return program_date_time_mode;
}

typedef struct _GstM3U8Entry GstM3U8Entry;

struct _GstM3U8Entry
{
  gfloat duration;
  gchar *title;
  gchar *url;
  gboolean discontinuous;
  GDateTime *program_date_time;
};

static GstM3U8Entry *
gst_m3u8_entry_new (const gchar * url, const gchar * title,
    gfloat duration, gboolean discontinuous, GDateTime * program_date_time)
{
  GstM3U8Entry *entry;

  g_return_val_if_fail (url != NULL, NULL);

  entry = g_new0 (GstM3U8Entry, 1);
  entry->url = g_strdup (url);
  entry->title = g_strdup (title);
  entry->duration = duration;
  entry->discontinuous = discontinuous;
  entry->program_date_time = program_date_time;

  return entry;
}

static void
gst_m3u8_entry_free (GstM3U8Entry * entry)
{
  g_return_if_fail (entry != NULL);

  g_free (entry->url);
  g_free (entry->title);
  g_date_time_unref (entry->program_date_time);
  g_free (entry);
}

GstM3U8Playlist *
gst_m3u8_playlist_new (guint version, guint window_size)
{
  GstM3U8Playlist *playlist;

  playlist = g_new0 (GstM3U8Playlist, 1);
  playlist->version = version;
  playlist->window_size = window_size;
  playlist->type = GST_M3U8_PLAYLIST_TYPE_EVENT;
  playlist->end_list = FALSE;
  playlist->encryption_method = 0;
  playlist->key_location = "playlist.key";
  playlist->entries = g_queue_new ();
  playlist->program_date_time_mode = GST_HLS_PROGRAM_DATE_TIME_ALL_CHUNKS;

  return playlist;
}

void
gst_m3u8_playlist_free (GstM3U8Playlist * playlist)
{
  g_return_if_fail (playlist != NULL);

  g_queue_foreach (playlist->entries, (GFunc) gst_m3u8_entry_free, NULL);
  g_queue_free (playlist->entries);
  g_free (playlist);
}


gboolean
gst_m3u8_playlist_add_entry (GstM3U8Playlist * playlist, const gchar * url,
    const gchar * title, gfloat duration, guint index, gboolean discontinuous,
    GDateTime * program_date_time)
{
  GstM3U8Entry *entry;

  g_return_val_if_fail (playlist != NULL, FALSE);
  g_return_val_if_fail (url != NULL, FALSE);

  if (playlist->type == GST_M3U8_PLAYLIST_TYPE_VOD)
    return FALSE;

  entry = gst_m3u8_entry_new (url, title, duration, discontinuous,
      program_date_time);

  if (playlist->window_size > 0) {
    /* Delete old entries from the playlist */
    while (playlist->entries->length >= playlist->window_size) {
      GstM3U8Entry *old_entry;

      old_entry = g_queue_pop_head (playlist->entries);
      gst_m3u8_entry_free (old_entry);
    }
  }

  playlist->sequence_number = index + 1;
  g_queue_push_tail (playlist->entries, entry);

  return TRUE;
}

static guint
gst_m3u8_playlist_target_duration (GstM3U8Playlist * playlist)
{
  guint64 target_duration = 0;
  GList *l;

  for (l = playlist->entries->head; l != NULL; l = l->next) {
    GstM3U8Entry *entry = l->data;

    if (entry->duration > target_duration)
      target_duration = entry->duration;
  }

  return (guint) ceil ((target_duration + 500.0 * GST_MSECOND) / GST_SECOND);
}

static const gchar *
encryption_method_to_string (gint method)
{
  static const gchar *encryption_methods[] = {
    "NONE",
    "AES-128"
  };
  gsize nmethods = sizeof (encryption_methods) / sizeof (encryption_methods[0]);
  return method >= 0 && method < nmethods ? encryption_methods[method] : NULL;
}

static void
format_program_date_time (GstM3U8Playlist * playlist, GstM3U8Entry * entry,
    GString * playlist_str)
{
  gchar *time_str = NULL;
  gint milliseconds = 0;

  if (playlist->program_date_time_mode == GST_HLS_PROGRAM_DATE_TIME_NEVER)
    return;
  if (playlist->program_date_time_mode == GST_HLS_PROGRAM_DATE_TIME_FIRST_CHUNK
      && entry != playlist->entries->head->data && !entry->discontinuous)
    return;

  time_str = g_date_time_format (entry->program_date_time, "%FT%T");
  milliseconds = g_date_time_get_microsecond (entry->program_date_time) / 1000;

  if (time_str)
    g_string_append_printf (playlist_str, "#EXT-X-PROGRAM-DATE-TIME:%s.%03dZ\n",
        time_str, milliseconds);

  g_free (time_str);
}

gchar *
gst_m3u8_playlist_render (GstM3U8Playlist * playlist)
{
  GString *playlist_str;
  GList *l;

  g_return_val_if_fail (playlist != NULL, NULL);

  playlist_str = g_string_new ("#EXTM3U\n");

  g_string_append_printf (playlist_str, "#EXT-X-VERSION:%d\n",
      playlist->version);

  g_string_append_printf (playlist_str, "#EXT-X-MEDIA-SEQUENCE:%d\n",
      playlist->sequence_number - playlist->entries->length);

  g_string_append_printf (playlist_str, "#EXT-X-TARGETDURATION:%u\n",
      gst_m3u8_playlist_target_duration (playlist));

  if (playlist->encryption_method && playlist->key_location) {
    g_string_append_printf (playlist_str, "#EXT-X-KEY:METHOD=%s,URI=\"%s\"\n",
        encryption_method_to_string (playlist->encryption_method),
        playlist->key_location);
  }

  g_string_append (playlist_str, "\n");

  /* Entries */
  for (l = playlist->entries->head; l != NULL; l = l->next) {
    GstM3U8Entry *entry = l->data;

    if (entry->discontinuous)
      g_string_append (playlist_str, "#EXT-X-DISCONTINUITY\n");

    format_program_date_time (playlist, entry, playlist_str);

    if (playlist->version < 3) {
      g_string_append_printf (playlist_str, "#EXTINF:%d,%s\n",
          (gint) ((entry->duration + 500 * GST_MSECOND) / GST_SECOND),
          entry->title ? entry->title : "");
    } else {
      g_string_append_printf (playlist_str, "#EXTINF:%.6f,%s\n",
          entry->duration / GST_SECOND, entry->title ? entry->title : "");

    }

    g_string_append_printf (playlist_str, "%s\n", entry->url);
  }

  if (playlist->end_list)
    g_string_append (playlist_str, "#EXT-X-ENDLIST");

  return g_string_free (playlist_str, FALSE);
}
