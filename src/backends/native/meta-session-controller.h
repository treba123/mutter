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

#ifndef META_SESSION_CONTROLLER_H
#define META_SESSION_CONTROLLER_H

#include <gio/gio.h>

typedef struct _MetaSessionController MetaSessionController;

#include "backends/native/meta-native-renderer.h"

MetaSessionController *meta_session_controller_new              (GError                **error);
const char            *meta_session_controller_get_seat_id      (MetaSessionController  *self);
MetaNativeRenderer    *meta_session_controller_get_renderer     (MetaSessionController *self);

void                   meta_session_controller_free             (MetaSessionController  *self);

gboolean               meta_session_controller_activate_session (MetaSessionController  *self,
                                                                 GError                **error);

gboolean               meta_session_controller_activate_vt      (MetaSessionController  *self,
                                                                 signed char             vt,
                                                                 GError                **error);

gboolean               meta_session_controller_take_device      (MetaSessionController  *self,
                                                                 const char             *device_path,
                                                                 GCancellable           *cancellable,
                                                                 int                    *device_fd,
                                                                 GError                **error);

#endif /* META_SESSION_CONTROLLER_H */
