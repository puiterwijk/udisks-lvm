/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __UL_MANAGER_H__
#define __UL_MANAGER_H__

#include "daemon.h"
#include "volumegroup.h"

G_BEGIN_DECLS

#define UL_TYPE_MANAGER         (ul_manager_get_type ())
#define UL_MANAGER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), UL_TYPE_MANAGER, UlManager))
#define UL_IS_MANAGER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), UL_TYPE_MANAGER))

GType                  ul_manager_get_type                 (void) G_GNUC_CONST;

UlManager *            ul_manager_new                      (void);

UlVolumeGroup *        ul_manager_find_volume_group        (UlManager *self,
                                                            const gchar *name);

G_END_DECLS

#endif /* __UL_MANAGER_H__ */
