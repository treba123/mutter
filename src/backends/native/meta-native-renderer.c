/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2014 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

#include "config.h"

#include "meta-native-renderer.h"

#include <meta/meta-backend.h>
#include <meta/util.h>

#include <cogl/cogl.h>
#include <clutter/clutter.h>
#include <clutter/egl/clutter-egl.h>

#include <systemd/sd-login.h>
#include <gudev/gudev.h>

#include "backends/meta-backend-private.h"
#include "meta-cursor-renderer-native.h"
#include "meta-session-controller.h"

struct _MetaNativeRenderer
{
  GObject parent;

  MetaSessionController *session_controller;
  int modesetting_fd;
};

G_DEFINE_TYPE (MetaNativeRenderer, meta_native_renderer, G_TYPE_OBJECT);

static void
meta_native_renderer_class_init (MetaNativeRendererClass *klass)
{
}

static void
meta_native_renderer_init (MetaNativeRenderer *self)
{
  self->modesetting_fd = -1;
}

static gchar *
get_primary_gpu_path (const gchar *seat_name)
{
  const gchar *subsystems[] = {"drm", NULL};
  gchar *path = NULL;
  GList *devices, *tmp;

  GUdevClient *gudev_client = g_udev_client_new (subsystems);
  GUdevEnumerator *enumerator = g_udev_enumerator_new (gudev_client);

  g_udev_enumerator_add_match_name (enumerator, "card*");
  g_udev_enumerator_add_match_tag (enumerator, "seat");

  devices = g_udev_enumerator_execute (enumerator);
  if (!devices)
    goto out;

  for (tmp = devices; tmp != NULL && path == NULL; tmp = tmp->next)
    {
      GUdevDevice *platform_device = NULL, *pci_device = NULL;
      GUdevDevice *dev = tmp->data;
      gint boot_vga;
      const gchar *device_seat;

      /* filter out devices that are not character device, like card0-VGA-1 */
      if (g_udev_device_get_device_type (dev) != G_UDEV_DEVICE_TYPE_CHAR)
        continue;

      device_seat = g_udev_device_get_property (dev, "ID_SEAT");
      if (!device_seat)
        {
          /* when ID_SEAT is not set, it means seat0 */
          device_seat = "seat0";
        }
      else if (g_strcmp0 (device_seat, "seat0") != 0)
        {
          /* if the device has been explicitly assigned other seat
           * than seat0, it is probably the right device to use */
          path = g_strdup (g_udev_device_get_device_file (dev));
          break;
        }

      /* skip devices that do not belong to our seat */
      if (g_strcmp0 (seat_name, device_seat))
        continue;

      platform_device = g_udev_device_get_parent_with_subsystem (dev, "platform", NULL);
      pci_device = g_udev_device_get_parent_with_subsystem (dev, "pci", NULL);

      if (platform_device != NULL)
        {
          path = g_strdup (g_udev_device_get_device_file (dev));
        }
      else if (pci_device != NULL)
        {
          /* get value of boot_vga attribute or 0 if the device has no boot_vga */
          boot_vga = g_udev_device_get_sysfs_attr_as_int (pci_device, "boot_vga");
          if (boot_vga == 1)
            {
              /* found the boot_vga device */
              path = g_strdup (g_udev_device_get_device_file (dev));
            }
        }

      g_object_unref (platform_device);
      g_object_unref (pci_device);
    }

  g_list_free_full (devices, g_object_unref);

out:
  g_object_unref (enumerator);
  g_object_unref (gudev_client);

  return path;
}

MetaNativeRenderer *
meta_native_renderer_new (MetaSessionController *session_controller)
{
  GObject *object;
  MetaNativeRenderer *self;

  object = g_object_new (META_TYPE_NATIVE_RENDERER, NULL);

  self = META_NATIVE_RENDERER (object);

  self->session_controller = session_controller;

  return self;
}

void
meta_native_renderer_pause (MetaNativeRenderer *self)
{
  clutter_egl_freeze_master_clock ();
}

void
meta_native_renderer_unpause (MetaNativeRenderer *self)
{
  ClutterBackend *clutter_backend;
  CoglContext *cogl_context;
  CoglDisplay *cogl_display;
  MetaBackend *backend = meta_get_backend ();
  MetaCursorRenderer *renderer = meta_backend_get_cursor_renderer (backend);
  ClutterActor *stage = meta_backend_get_stage (backend);

  clutter_backend = clutter_get_default_backend ();
  cogl_context = clutter_backend_get_cogl_context (clutter_backend);
  cogl_display = cogl_context_get_display (cogl_context);

  cogl_kms_display_queue_modes_reset (cogl_display);

  clutter_egl_thaw_master_clock ();

  /* When we mode-switch back, we need to immediately queue a redraw
   * in case nothing else queued one for us, and force the cursor to
   * update. */

  clutter_actor_queue_redraw (stage);
  meta_cursor_renderer_native_force_update (META_CURSOR_RENDERER_NATIVE (renderer));
}

gboolean
meta_native_renderer_start (MetaNativeRenderer  *self,
                            GError             **error)
{
  g_autofree gchar *path = NULL;
  int fd;
  const char *seat_id;

  seat_id = meta_session_controller_get_seat_id (self->session_controller);

  path = get_primary_gpu_path (seat_id);

  if (!path)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "could not find drm kms device");
      return FALSE;
    }

  if (!meta_session_controller_take_device (self->session_controller, path, NULL, &fd, error))
    return FALSE;

  self->modesetting_fd = fd;
  clutter_egl_set_kms_fd (fd);

  return TRUE;
}
