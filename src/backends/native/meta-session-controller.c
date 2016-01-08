/*
 * Copyright (C) 2013 Red Hat, Inc.
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
 */

#include "config.h"

#include "meta-session-controller.h"

#include <gio/gunixfdlist.h>

#include <clutter/clutter.h>
#include <clutter/egl/clutter-egl.h>
#include <clutter/evdev/clutter-evdev.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <malloc.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-login.h>
#include <gudev/gudev.h>

#include "dbus-utils.h"
#include "meta-dbus-login1.h"

#include "meta-native-renderer.h"
#include "meta-idle-monitor-native.h"

struct _MetaSessionController
{
  Login1Session *session_proxy;
  Login1Seat *seat_proxy;
  MetaNativeRenderer *renderer;

  char *seat_id;

  gboolean session_active;
};

static Login1Session *
get_session_proxy (GCancellable *cancellable,
                   GError      **error)
{
  char *proxy_path;
  char *session_id;
  Login1Session *session_proxy;

  if (sd_pid_get_session (getpid (), &session_id) < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "Could not get session ID: %m");
      return NULL;
    }

  proxy_path = get_escaped_dbus_path ("/org/freedesktop/login1/session", session_id);

  session_proxy = login1_session_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                         "org.freedesktop.login1",
                                                         proxy_path,
                                                         cancellable, error);
  if (!session_proxy)
    g_prefix_error(error, "Could not get session proxy: ");

  free (proxy_path);

  return session_proxy;
}

static Login1Seat *
get_seat_proxy (GCancellable *cancellable,
                GError      **error)
{
  Login1Seat *seat = login1_seat_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                         "org.freedesktop.login1",
                                                         "/org/freedesktop/login1/seat/self",
                                                         cancellable, error);
  if (!seat)
    g_prefix_error(error, "Could not get seat proxy: ");

  return seat;
}

static void
session_unpause (MetaSessionController *self)
{
  clutter_evdev_reclaim_devices ();
  meta_native_renderer_unpause (self->renderer);
}

static void
session_pause (MetaSessionController *self)
{
  clutter_evdev_release_devices ();
  meta_native_renderer_pause (self->renderer);
}

static gboolean
take_device (Login1Session *session_proxy,
             int            dev_major,
             int            dev_minor,
             int           *out_fd,
             GCancellable  *cancellable,
             GError       **error)
{
  g_autoptr (GVariant) fd_variant = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  int fd = -1;

  if (!login1_session_call_take_device_sync (session_proxy,
                                             dev_major,
                                             dev_minor,
                                             NULL,
                                             &fd_variant,
                                             NULL, /* paused */
                                             &fd_list,
                                             cancellable,
                                             error))
    return FALSE;

  fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_variant), error);
  if (fd == -1)
    return FALSE;

  *out_fd = fd;
  return TRUE;
}

static gboolean
get_device_info_from_path (const char *path,
                           int        *out_major,
                           int        *out_minor)
{
  int r;
  struct stat st;

  r = stat (path, &st);
  if (r < 0 || !S_ISCHR (st.st_mode))
    return FALSE;

  *out_major = major (st.st_rdev);
  *out_minor = minor (st.st_rdev);
  return TRUE;
}

static gboolean
get_device_info_from_fd (int  fd,
                         int *out_major,
                         int *out_minor)
{
  int r;
  struct stat st;

  r = fstat (fd, &st);
  if (r < 0 || !S_ISCHR (st.st_mode))
    return FALSE;

  *out_major = major (st.st_rdev);
  *out_minor = minor (st.st_rdev);
  return TRUE;
}

static int
on_evdev_device_open (const char  *path,
                      int          flags,
                      gpointer     user_data,
                      GError     **error)
{
  MetaSessionController *self = user_data;
  int fd;
  int major, minor;

  if (!get_device_info_from_path (path, &major, &minor))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "Could not get device info for path %s: %m", path);
      return -1;
    }

  if (!take_device (self->session_proxy, major, minor, &fd, NULL, error))
    return -1;

  return fd;
}

static void
on_evdev_device_close (int      fd,
                       gpointer user_data)
{
  MetaSessionController *self = user_data;
  int major, minor;
  GError *error = NULL;

  if (!get_device_info_from_fd (fd, &major, &minor))
    {
      g_warning ("Could not get device info for fd %d: %m", fd);
      goto out;
    }

  if (!login1_session_call_release_device_sync (self->session_proxy,
                                                major, minor,
                                                NULL, &error))
    {
      g_warning ("Could not release device %d,%d: %s", major, minor, error->message);
    }

out:
  close (fd);
}

static void
sync_active (MetaSessionController *self)
{
  gboolean active = login1_session_get_active (LOGIN1_SESSION (self->session_proxy));

  if (active == self->session_active)
    return;

  self->session_active = active;

  if (active)
    session_unpause (self);
  else
    session_pause (self);
}

static void
on_active_changed (Login1Session *session,
                   GParamSpec    *pspec,
                   gpointer       user_data)
{
  MetaSessionController *self = user_data;
  sync_active (self);
}

static gchar *
get_seat_id (GError **error)
{
  char *session_id, *seat_id;
  int r;

  r = sd_pid_get_session (0, &session_id);
  if (r < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "Could not get session for PID: %s", g_strerror (-r));
      return NULL;
    }

  r = sd_session_get_seat (session_id, &seat_id);
  free (session_id);

  if (r < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "Could not get seat for session: %s", g_strerror (-r));
      return NULL;
    }

  return seat_id;
}

gboolean
meta_session_controller_take_device (MetaSessionController  *self,
                                     const char             *device_path,
                                     GCancellable           *cancellable,
                                     int                    *device_fd,
                                     GError                **error)
{
  int major, minor;

  if (!get_device_info_from_path (device_path, &major, &minor))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_NOT_FOUND,
                   "Could not access device %s", device_path);
      return FALSE;
    }

  if (!take_device (self->session_proxy, major, minor, device_fd, cancellable, error))
    return FALSE;

  return TRUE;
}

MetaSessionController *
meta_session_controller_new (GError **error)
{
  MetaSessionController *self = NULL;
  Login1Session *session_proxy = NULL;
  Login1Seat *seat_proxy = NULL;
  MetaNativeRenderer *renderer;
  char *seat_id = NULL;
  gboolean have_control = FALSE;

  session_proxy = get_session_proxy (NULL, error);
  if (!session_proxy)
    goto fail;

  if (!login1_session_call_take_control_sync (session_proxy, FALSE, NULL, error))
    {
      g_prefix_error (error, "Could not take control: ");
      goto fail;
    }

  have_control = TRUE;

  seat_id = get_seat_id (error);
  if (!seat_id)
    goto fail;

  seat_proxy = get_seat_proxy (NULL, error);
  if (!seat_proxy)
    goto fail;

  self = g_slice_new0 (MetaSessionController);
  self->session_proxy = session_proxy;
  self->seat_proxy = seat_proxy;
  self->seat_id = seat_id;

  self->session_active = TRUE;

  renderer = meta_native_renderer_new (self);

  if (!meta_native_renderer_start (renderer, error))
    goto fail;

  self->renderer = renderer;

  clutter_evdev_set_device_callbacks (on_evdev_device_open,
                                      on_evdev_device_close,
                                      self);

  g_signal_connect (self->session_proxy, "notify::active", G_CALLBACK (on_active_changed), self);
  return self;

 fail:
  if (have_control)
    login1_session_call_release_control_sync (session_proxy, NULL, NULL);
  g_clear_object (&session_proxy);
  g_clear_object (&seat_proxy);
  g_clear_object (&renderer);

  if (self)
    g_slice_free (MetaSessionController, self);

  free (seat_id);

  return NULL;
}

const char *
meta_session_controller_get_seat_id (MetaSessionController  *self)
{
  return self->seat_id;
}

MetaNativeRenderer *
meta_session_controller_get_renderer (MetaSessionController *self)
{
  return self->renderer;
}

void
meta_session_controller_free (MetaSessionController *self)
{
  g_object_unref (self->seat_proxy);
  g_object_unref (self->session_proxy);
  free (self->seat_id);
  g_slice_free (MetaSessionController, self);
}

gboolean
meta_session_controller_activate_session (MetaSessionController  *session_controller,
                                          GError                **error)
{
  if (!login1_session_call_activate_sync (session_controller->session_proxy, NULL, error))
    return FALSE;

  sync_active (session_controller);
  return TRUE;
}

gboolean
meta_session_controller_activate_vt (MetaSessionController  *session_controller,
                                     signed char             vt,
                                     GError                **error)
{
  return login1_seat_call_switch_to_sync (session_controller->seat_proxy, vt, NULL, error);
}
