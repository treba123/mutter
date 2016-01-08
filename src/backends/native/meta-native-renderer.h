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

#ifndef META_NATIVE_RENDERER_H
#define META_NATIVE_RENDERER_H

#include <glib-object.h>
#include "backends/native/meta-session-controller.h"

#define META_TYPE_NATIVE_RENDERER (meta_native_renderer_get_type ())
G_DECLARE_FINAL_TYPE (MetaNativeRenderer,
                      meta_native_renderer,
                      META, NATIVE_RENDERER,
                      GObject);

MetaNativeRenderer * meta_native_renderer_new (MetaSessionController *session_controller);

gboolean             meta_native_renderer_start (MetaNativeRenderer  *self,
                                                 GError             **error);
void                 meta_native_renderer_pause (MetaNativeRenderer *self);
void                 meta_native_renderer_unpause (MetaNativeRenderer *self);

int                  meta_native_renderer_get_modesetting_fd (MetaNativeRenderer *self);

#endif /* META_NATIVE_RENDERER_H */
