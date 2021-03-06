/*
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
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
 */

#include "config.h"

#include "manager.h"

#include "block.h"
#include "daemon.h"
#include "invocation.h"
#include "util.h"
#include "volumegroup.h"

#include <gudev/gudev.h>
#include <glib/gi18n.h>

struct _UlManager
{
  LvmManagerSkeleton parent;

  UDisksClient *udisks_client;
  GUdevClient *udev_client;

  /* maps from volume group name to UlVolumeGroupObject
     instances.
  */
  GHashTable *name_to_volume_group;

  gint lvm_delayed_update_id;

  /* GDBusObjectManager is that special kind of ugly */
  gulong sig_object_added;
  gulong sig_object_removed;
  gulong sig_interface_added;
  gulong sig_interface_removed;
};

typedef struct
{
  LvmManagerSkeletonClass parent;
} UlManagerClass;

static void lvm_manager_iface_init (LvmManagerIface *iface);

G_DEFINE_TYPE_WITH_CODE (UlManager, ul_manager, LVM_TYPE_MANAGER_SKELETON,
                         G_IMPLEMENT_INTERFACE (LVM_TYPE_MANAGER, lvm_manager_iface_init);
);

static void
lvm_update_from_variant (GPid pid,
                         GVariant *volume_groups,
                         GError *error,
                         gpointer user_data)
{
  UlManager *self = user_data;
  GVariantIter var_iter;
  GHashTableIter vg_name_iter;
  gpointer key, value;
  const gchar *name;

  if (error != NULL)
    {
      g_critical ("%s", error->message);
      return;
    }

  /* Remove obsolete groups */
  g_hash_table_iter_init (&vg_name_iter, self->name_to_volume_group);
  while (g_hash_table_iter_next (&vg_name_iter, &key, &value))
    {
      const gchar *vg;
      UlVolumeGroup *group;
      gboolean found = FALSE;

      name = key;
      group = value;

      g_variant_iter_init (&var_iter, volume_groups);
      while (g_variant_iter_next (&var_iter, "&s", &vg))
        {
          if (g_strcmp0 (vg, name) == 0)
            {
              found = TRUE;
              break;
            }
        }

      if (!found)
        {
          /* Object unpublishes itself */
          g_object_run_dispose (G_OBJECT (group));
          g_hash_table_iter_remove (&vg_name_iter);
        }
    }

  /* Add new groups and update existing groups */
  g_variant_iter_init (&var_iter, volume_groups);
  while (g_variant_iter_next (&var_iter, "&s", &name))
    {
      UlVolumeGroup *group;
      group = g_hash_table_lookup (self->name_to_volume_group, name);

      if (group == NULL)
        {
          group = ul_volume_group_new (name);
          g_debug ("adding volume group: %s", name);

          g_hash_table_insert (self->name_to_volume_group, g_strdup (name), group);
        }

      ul_volume_group_update (group);
    }
}

static void
lvm_update (UlManager *self)
{
  const gchar *args[] = {
      "udisks-lvm-helper", "-b", "list",
      NULL
  };

  ul_daemon_spawn_for_variant (ul_daemon_get (), args, G_VARIANT_TYPE("as"),
                               lvm_update_from_variant, self);
}

static gboolean
delayed_lvm_update (gpointer user_data)
{
  UlManager *self = UL_MANAGER (user_data);

  lvm_update (self);
  self->lvm_delayed_update_id = 0;

  return FALSE;
}

static void
trigger_delayed_lvm_update (UlManager *self)
{
  if (self->lvm_delayed_update_id > 0)
    return;

  self->lvm_delayed_update_id =
    g_timeout_add (100, delayed_lvm_update, self);
}

static void
do_delayed_lvm_update_now (UlManager *self)
{
  if (self->lvm_delayed_update_id > 0)
    {
      g_source_remove (self->lvm_delayed_update_id);
      self->lvm_delayed_update_id = 0;
      lvm_update (self);
    }
}

static gboolean
is_logical_volume (GUdevDevice *device)
{
  const gchar *dm_vg_name = g_udev_device_get_property (device, "DM_VG_NAME");
  return dm_vg_name && *dm_vg_name;
}

static gboolean
has_physical_volume_label (GUdevDevice *device)
{
  const gchar *id_fs_type = g_udev_device_get_property (device, "ID_FS_TYPE");
  return g_strcmp0 (id_fs_type, "LVM2_member") == 0;
}

static LvmObject *
find_block (UlManager *self,
            dev_t device_number)
{
  LvmObject *our_block = NULL;
  UDisksBlock *real_block;
  GDBusObject *object;
  const gchar *path;

  real_block = udisks_client_get_block_for_dev (self->udisks_client, device_number);
  if (real_block != NULL)
    {
      object = g_dbus_interface_get_object (G_DBUS_INTERFACE (real_block));
      path = g_dbus_object_get_object_path (object);

      our_block = ul_daemon_find_thing (ul_daemon_get (), path, 0);
      g_object_unref (real_block);
    }

  return our_block;
}

static gboolean
is_recorded_as_physical_volume (UlManager *self,
                                GUdevDevice *device)
{
  LvmObject *object;
  gboolean ret = FALSE;

  object = find_block (self, g_udev_device_get_device_number (device));
  if (object != NULL)
    {
      ret = (lvm_object_peek_physical_volume_block (object) != NULL);
      g_object_unref (object);
    }

  return ret;
}

static void
handle_block_uevent_for_lvm (UlManager *self,
                             const gchar *action,
                             GUdevDevice *device)
{
  if (is_logical_volume (device)
      || has_physical_volume_label (device)
      || is_recorded_as_physical_volume (self, device))
    trigger_delayed_lvm_update (self);
}

static void
on_uevent (GUdevClient *client,
           const gchar *action,
           GUdevDevice *device,
           gpointer user_data)
{
  g_debug ("udev event '%s' for %s", action,
           device ? g_udev_device_get_name (device) : "???");
  handle_block_uevent_for_lvm (user_data, action, device);
}

static void
on_udisks_interface_added (GDBusObjectManager *udisks_object_manager,
                           GDBusObject *object,
                           GDBusInterface *interface,
                           gpointer user_data)
{
  UlManager *self = user_data;
  GDBusObjectSkeleton *overlay;
  const gchar *path;

  if (!UDISKS_IS_BLOCK (interface))
    return;

  /* Same path as the original real udisks block */
  path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface));

  overlay = g_object_new (UL_TYPE_BLOCK,
                          "real-block", interface,
                          "udev-client", self->udev_client,
                          NULL);

  ul_daemon_publish (ul_daemon_get (), path, FALSE, overlay);
  g_object_unref (overlay);
}

static void
on_udisks_object_added (GDBusObjectManager *udisks_object_manager,
                        GDBusObject *object,
                        gpointer user_data)
{
  GList *interfaces, *l;

  /* Yes, GDBusObjectManager really is this awkward */
  interfaces = g_dbus_object_get_interfaces (object);
  for (l = interfaces; l != NULL; l = g_list_next (l))
    on_udisks_interface_added (udisks_object_manager, object, l->data, user_data);
  g_list_free_full (interfaces, g_object_unref);
}

static void
on_udisks_interface_removed (GDBusObjectManager *udisks_object_manager,
                             GDBusObject *object,
                             GDBusInterface *interface,
                             gpointer user_data)
{
  const gchar *path;

  if (!UDISKS_IS_BLOCK (interface))
    return;

  /* Same path as the original real udisks block */
  path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface));

  ul_daemon_unpublish (ul_daemon_get (), path, NULL);
}

static void
on_udisks_object_removed (GDBusObjectManager *udisks_object_manager,
                          GDBusObject *object,
                          gpointer user_data)
{
  GList *interfaces, *l;

  /* Yes, GDBusObjectManager really is this awkward */
  interfaces = g_dbus_object_get_interfaces (object);
  for (l = interfaces; l != NULL; l = g_list_next (l))
    on_udisks_interface_removed (udisks_object_manager, object, l->data, user_data);
  g_list_free_full (interfaces, g_object_unref);
}

static void
ul_manager_init (UlManager *self)
{
  GDBusObjectManager *object_manager;
  GError *error = NULL;
  GList *objects, *o;
  GList *interfaces, *i;

  const gchar *subsystems[] = {
      "block",
      "iscsi_connection",
      "scsi",
      NULL
  };

  /* get ourselves an udev client */
  self->udev_client = g_udev_client_new (subsystems);
  g_signal_connect (self->udev_client, "uevent", G_CALLBACK (on_uevent), self);

  self->udisks_client = udisks_client_new_sync (NULL, &error);
  if (error != NULL)
    {
      g_critical ("Couldn't connect to the main udisksd: %s", error->message);
      g_clear_error (&error);
    }
  else
    {
      object_manager = udisks_client_get_object_manager (self->udisks_client);
      objects = g_dbus_object_manager_get_objects (object_manager);
      for (o = objects; o != NULL; o = g_list_next (o))
        {
          interfaces = g_dbus_object_get_interfaces (o->data);
          for (i = interfaces; i != NULL; i = g_list_next (i))
            on_udisks_interface_added (object_manager, o->data, i->data, self);
          g_list_free_full (interfaces, g_object_unref);
        }
      g_list_free_full (objects, g_object_unref);

      self->sig_object_added = g_signal_connect (object_manager, "object-added",
                                                 G_CALLBACK (on_udisks_object_added), self);
      self->sig_interface_added = g_signal_connect (object_manager, "interface-added",
                                                    G_CALLBACK (on_udisks_interface_added), self);
      self->sig_object_removed = g_signal_connect (object_manager, "object-removed",
                                                   G_CALLBACK (on_udisks_object_removed), self);
      self->sig_interface_removed = g_signal_connect (object_manager, "interface-removed",
                                                      G_CALLBACK (on_udisks_interface_removed), self);
    }
}

static void
ul_manager_constructed (GObject *object)
{
  UlManager *self = UL_MANAGER (object);

  G_OBJECT_CLASS (ul_manager_parent_class)->constructed (object);

  self->name_to_volume_group = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                      (GDestroyNotify) g_object_unref);

  do_delayed_lvm_update_now (self);

  udisks_client_settle (self->udisks_client);
}

static void
ul_manager_finalize (GObject *object)
{
  UlManager *self = UL_MANAGER (object);
  GDBusObjectManager *objman;

  if (self->udisks_client)
    {
      objman = udisks_client_get_object_manager (self->udisks_client);
      if (self->sig_object_added)
        g_signal_handler_disconnect (objman, self->sig_object_added);
      if (self->sig_interface_added)
        g_signal_handler_disconnect (objman, self->sig_interface_added);
      if (self->sig_object_removed)
        g_signal_handler_disconnect (objman, self->sig_object_removed);
      if (self->sig_interface_removed)
        g_signal_handler_disconnect (objman, self->sig_interface_removed);
      g_object_unref (self->udisks_client);
    }

  g_clear_object (&self->udev_client);
  g_hash_table_unref (self->name_to_volume_group);

  G_OBJECT_CLASS (ul_manager_parent_class)->finalize (object);
}

static void
ul_manager_class_init (UlManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ul_manager_constructed;
  object_class->finalize = ul_manager_finalize;
}

typedef struct {
  gchar **devices;
  gchar *vgname;
  gchar *extent_size;
} VolumeGroupCreateJobData;

static gboolean
volume_group_create_job_thread (GCancellable *cancellable,
                                gpointer user_data,
                                GError **error)
{
  VolumeGroupCreateJobData *data = user_data;
  gchar *standard_output;
  gchar *standard_error;
  gint exit_status;
  GPtrArray *argv;
  gboolean ret;
  gint i;

  for (i = 0; data->devices[i] != NULL; i++)
    {
      if (!ul_util_wipe_block (data->devices[i], error))
        return FALSE;
    }

  argv = g_ptr_array_new ();
  g_ptr_array_add (argv, (gchar *)"vgcreate");
  g_ptr_array_add (argv, data->vgname);
  if (data->extent_size)
    {
      g_ptr_array_add (argv, (gchar *)"-s");
      g_ptr_array_add (argv, data->extent_size);
    }
  for (i = 0; data->devices[i] != NULL; i++)
    g_ptr_array_add (argv, data->devices[i]);
  g_ptr_array_add (argv, NULL);

  ret = g_spawn_sync (NULL, (gchar **)argv->pdata, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &standard_output, &standard_error,
                      &exit_status, error);

  g_ptr_array_free (argv, TRUE);

  if (ret)
    {
      ret = ul_util_check_status_and_output ("vgcreate",
                                             exit_status, standard_output,
                                             standard_error, error);
    }

  g_free (standard_output);
  g_free (standard_error);
  return ret;
}

typedef struct {
  /* This part only accessed by thread */
  VolumeGroupCreateJobData data;

  GDBusMethodInvocation *invocation;
  gulong wait_sig;
} CompleteClosure;

static void
complete_closure_free (gpointer user_data,
                       GClosure *unused)
{
  CompleteClosure *complete = user_data;
  VolumeGroupCreateJobData *data = &complete->data;
  g_strfreev (data->devices);
  g_free (data->vgname);
  g_free (data->extent_size);
  g_object_unref (complete->invocation);
  g_free (complete);
}

static void
on_create_volume_group (UlDaemon *daemon,
                        UlVolumeGroup *group,
                        gpointer user_data)
{
  CompleteClosure *complete = user_data;

  if (g_str_equal (ul_volume_group_get_name (group), complete->data.vgname))
    {
      lvm_manager_complete_volume_group_create (NULL, complete->invocation,
                                                ul_volume_group_get_object_path (group));
      g_signal_handler_disconnect (daemon, complete->wait_sig);
    }
}

static void
on_create_complete (UDisksJob *job,
                    gboolean success,
                    gchar *message,
                    gpointer user_data)
{
  CompleteClosure *complete = user_data;

  if (success)
    return;

  g_dbus_method_invocation_return_error (complete->invocation, UDISKS_ERROR,
                                         UDISKS_ERROR_FAILED, "Error creating volume group: %s", message);
  g_signal_handler_disconnect (ul_daemon_get (), complete->wait_sig);
}

static gboolean
handle_volume_group_create (LvmManager *manager,
                            GDBusMethodInvocation *invocation,
                            const gchar *const *arg_blocks,
                            const gchar *arg_name,
                            guint64 arg_extent_size,
                            GVariant *arg_options)
{
  GError *error = NULL;
  GList *blocks = NULL;
  GList *l;
  guint n;
  UlDaemon *daemon;
  gchar *encoded_name = NULL;
  CompleteClosure *complete;
  UlJob *job;

  daemon = ul_daemon_get ();

  /* Collect and validate block objects
   *
   * Also, check we can open the block devices at the same time - this
   * is to avoid start deleting half the block devices while the other
   * half is already in use.
   */
  for (n = 0; arg_blocks != NULL && arg_blocks[n] != NULL; n++)
    {
      GDBusObject *object = NULL;

      object = ul_daemon_find_thing (ul_daemon_get (), arg_blocks[n], 0);

      /* Assumes ref, do this early for memory management */
      blocks = g_list_prepend (blocks, object);

      if (object == NULL)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Invalid object path %s at index %d",
                                                 arg_blocks[n], n);
          goto out;
        }

      if (!UL_IS_BLOCK (object))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Object path %s for index %d is not a block device",
                                                 arg_blocks[n], n);
          goto out;
        }

      if (!ul_block_is_unused (UL_BLOCK (object), &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
    }

  blocks = g_list_reverse (blocks);

  /* Create the volume group... */
  complete = g_new0 (CompleteClosure, 1);
  complete->invocation = g_object_ref (invocation);
  complete->data.vgname = ul_util_encode_lvm_name (arg_name, FALSE);
  if (arg_extent_size > 0)
    complete->data.extent_size = g_strdup_printf ("%" G_GUINT64_FORMAT "b", arg_extent_size);

  complete->data.devices = g_new0 (gchar *, n + 1);
  for (n = 0, l = blocks; l != NULL; l = g_list_next (l), n++)
    {
      g_assert (arg_blocks[n] != NULL);
      complete->data.devices[n] = g_strdup (ul_block_get_device (l->data));
    }

  job = ul_daemon_launch_threaded_job (daemon, NULL,
                                       "lvm-vg-create",
                                       ul_invocation_get_caller_uid (invocation),
                                       volume_group_create_job_thread,
                                       &complete->data,
                                       NULL, NULL);

  /* Wait for the job to finish */
  g_signal_connect (job, "completed", G_CALLBACK (on_create_complete), complete);

  /* Wait for the object to appear */
  complete->wait_sig = g_signal_connect_data (daemon,
                                              "published::UlVolumeGroup",
                                              G_CALLBACK (on_create_volume_group),
                                              complete, complete_closure_free, 0);

out:
  g_list_free_full (blocks, g_object_unref);
  g_free (encoded_name);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

static void
lvm_manager_iface_init (LvmManagerIface *iface)
{
  iface->handle_volume_group_create = handle_volume_group_create;
}

UlManager *
ul_manager_new (void)
{
  return g_object_new (UL_TYPE_MANAGER, NULL);
}
