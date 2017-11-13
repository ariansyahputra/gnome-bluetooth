/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2005-2008  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2010       Giovanni Campagna <scampa.giovanni@gmail.com>
 *  Copyright (C) 2013       Intel Corporation.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

/**
 * SECTION:bluetooth-client
 * @short_description: Bluetooth client object
 * @stability: Stable
 * @include: bluetooth-client.h
 *
 * The #BluetoothClient object is used to query the state of Bluetooth
 * devices and adapters.
 **/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include "bluetooth-client.h"
#include "bluetooth-client-private.h"
#include "bluetooth-client-glue.h"
#include "bluetooth-fdo-glue.h"
#include "bluetooth-utils.h"
#include "gnome-bluetooth-enum-types.h"
#include "pin.h"

#define BLUEZ_SERVICE			"org.bluez"
#define BLUEZ_MANAGER_PATH		"/"
#define BLUEZ_ADAPTER_INTERFACE		"org.bluez.Adapter1"
#define BLUEZ_DEVICE_INTERFACE		"org.bluez.Device1"

#define BLUETOOTH_CLIENT_GET_PRIVATE(obj) bluetooth_client_get_instance_private (obj)

typedef struct _BluetoothClientPrivate BluetoothClientPrivate;

struct _BluetoothClientPrivate {
	GDBusObjectManager *manager;
	GtkTreeStore *store;
	GtkTreeRowReference *default_adapter;
};

enum {
	PROP_0,
	PROP_DEFAULT_ADAPTER,
	PROP_DEFAULT_ADAPTER_POWERED,
	PROP_DEFAULT_ADAPTER_DISCOVERABLE,
	PROP_DEFAULT_ADAPTER_NAME,
	PROP_DEFAULT_ADAPTER_DISCOVERING
};

enum {
	DEVICE_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static const char *connectable_uuids[] = {
	"HSP",
	"AudioSource",
	"AudioSink",
	"A/V_RemoteControlTarget",
	"A/V_RemoteControl",
	"Headset_-_AG",
	"Handsfree",
	"HandsfreeAudioGateway",
	"HumanInterfaceDeviceService",
};

G_DEFINE_TYPE_WITH_PRIVATE(BluetoothClient, bluetooth_client, G_TYPE_OBJECT)

typedef gboolean (*IterSearchFunc) (GtkTreeStore *store,
				GtkTreeIter *iter, gpointer user_data);

static gboolean iter_search(GtkTreeStore *store,
				GtkTreeIter *iter, GtkTreeIter *parent,
				IterSearchFunc func, gpointer user_data)
{
	gboolean cont, found = FALSE;

	if (parent == NULL)
		cont = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store),
									iter);
	else
		cont = gtk_tree_model_iter_children(GTK_TREE_MODEL(store),
								iter, parent);

	while (cont == TRUE) {
		GtkTreeIter child;

		found = func(store, iter, user_data);
		if (found == TRUE)
			break;

		found = iter_search(store, &child, iter, func, user_data);
		if (found == TRUE) {
			*iter = child;
			break;
		}

		cont = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), iter);
	}

	return found;
}

static gboolean compare_path(GtkTreeStore *store,
					GtkTreeIter *iter, gpointer user_data)
{
	const gchar *path = user_data;
	GDBusProxy *object;
	gboolean found = FALSE;

	gtk_tree_model_get(GTK_TREE_MODEL(store), iter,
					BLUETOOTH_COLUMN_PROXY, &object, -1);

	if (object != NULL) {
		found = g_str_equal(path, g_dbus_proxy_get_object_path(object));
		g_object_unref(object);
	}

	return found;
}

static gboolean
compare_address (GtkTreeStore *store,
		 GtkTreeIter *iter,
		 gpointer user_data)
{
	const char *address = user_data;
	char *tmp_address;
	gboolean found = FALSE;

	gtk_tree_model_get (GTK_TREE_MODEL(store), iter,
			    BLUETOOTH_COLUMN_ADDRESS, &tmp_address, -1);
	found = g_str_equal (address, tmp_address);
	g_free (tmp_address);

	return found;
}

static gboolean
get_iter_from_path (GtkTreeStore *store,
		    GtkTreeIter *iter,
		    const char *path)
{
	return iter_search(store, iter, NULL, compare_path, (gpointer) path);
}

static gboolean
get_iter_from_proxy(GtkTreeStore *store,
		    GtkTreeIter *iter,
		    GDBusProxy *proxy)
{
	return iter_search(store, iter, NULL, compare_path,
			   (gpointer) g_dbus_proxy_get_object_path (proxy));
}

static gboolean
get_iter_from_address (GtkTreeStore *store,
		       GtkTreeIter  *iter,
		       const char   *address,
		       GDBusProxy   *adapter)
{
	GtkTreeIter parent_iter;

	if (get_iter_from_proxy (store, &parent_iter, adapter) == FALSE)
		return FALSE;

	return iter_search (store, iter, &parent_iter, compare_address, (gpointer) address);
}

static char **
device_list_uuids (const gchar * const *uuids)
{
	GPtrArray *ret;
	guint i;

	if (uuids == NULL)
		return NULL;

	ret = g_ptr_array_new ();

	for (i = 0; uuids[i] != NULL; i++) {
		const char *uuid;

		uuid = bluetooth_uuid_to_string (uuids[i]);
		if (uuid == NULL)
			continue;
		g_ptr_array_add (ret, g_strdup (uuid));
	}

	if (ret->len == 0) {
		g_ptr_array_free (ret, TRUE);
		return NULL;
	}

	g_ptr_array_add (ret, NULL);

	return (char **) g_ptr_array_free (ret, FALSE);
}

static char **
device_list_uuids_v (GVariant *variant)
{
	const char **uuids;
	char **out;

	if (variant == NULL)
		return NULL;

	uuids = g_variant_get_strv (variant, NULL);
	if (uuids == NULL)
		return NULL;

	out = device_list_uuids (uuids);
	g_free (uuids);
	return out;
}

gboolean
bluetooth_client_get_connectable(const char **uuids)
{
	int i, j;

	for (i = 0; uuids && uuids[i] != NULL; i++) {
		for (j = 0; j < G_N_ELEMENTS (connectable_uuids); j++) {
			if (g_str_equal (connectable_uuids[j], uuids[i]))
				return TRUE;
		}
	}

	return FALSE;
}

static const char *
phone_oui_to_icon_name (const char *bdaddr)
{
	char *vendor;
	const char *ret = NULL;

	vendor = oui_to_vendor (bdaddr);
	if (vendor == NULL)
		return NULL;

	if (strstr (vendor, "Apple") != NULL)
		ret = "phone-apple-iphone";
	else if (strstr (vendor, "Samsung") != NULL)
		ret = "phone-samsung-galaxy-s";
	else if (strstr (vendor, "Google") != NULL)
		ret = "phone-google-nexus-one";
	g_free (vendor);

	return ret;
}

static const char *
icon_override (const char    *bdaddr,
	       BluetoothType  type)
{
	/* audio-card, you're ugly */
	switch (type) {
	case BLUETOOTH_TYPE_HEADSET:
		return "audio-headset";
	case BLUETOOTH_TYPE_HEADPHONES:
		return "audio-headphones";
	case BLUETOOTH_TYPE_OTHER_AUDIO:
		return "audio-speakers";
	case BLUETOOTH_TYPE_PHONE:
		return phone_oui_to_icon_name (bdaddr);
	case BLUETOOTH_TYPE_DISPLAY:
		return "video-display";
	case BLUETOOTH_TYPE_SCANNER:
		return "scanner";
	case BLUETOOTH_TYPE_REMOTE_CONTROL:
	case BLUETOOTH_TYPE_WEARABLE:
	case BLUETOOTH_TYPE_TOY:
		/* FIXME */
	default:
		return NULL;
	}
}

static void
device_g_properties_changed (GDBusProxy      *device,
			     GVariant        *changed_p,
			     GStrv            invalidated_p,
			     BluetoothClient *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GVariantIter i;
	const char *property;
	GtkTreeIter iter;
	GVariant *v;

	if (get_iter_from_proxy (priv->store, &iter, device) == FALSE)
		return;

	g_variant_iter_init (&i, changed_p);
	while (g_variant_iter_next (&i, "{&sv}", &property, &v)) {

		if (g_str_equal (property, "Name") == TRUE) {
			const gchar *name = g_variant_get_string (v, NULL);

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_NAME, name, -1);
		} else if (g_str_equal (property, "Alias") == TRUE) {
			const gchar *alias = g_variant_get_string (v, NULL);

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_ALIAS, alias, -1);
		} else if (g_str_equal (property, "Icon") == TRUE) {
			const gchar *icon = g_variant_get_string (v, NULL);

			/* See "Class" handling below */
			if (g_strcmp0 (icon, "audio-card") != 0) {
				gtk_tree_store_set (priv->store, &iter,
						    BLUETOOTH_COLUMN_ICON, icon, -1);
			}
		} else if (g_str_equal (property, "Paired") == TRUE) {
			gboolean paired = g_variant_get_boolean (v);

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_PAIRED, paired, -1);
		} else if (g_str_equal (property, "Trusted") == TRUE) {
			gboolean trusted = g_variant_get_boolean (v);

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_TRUSTED, trusted, -1);
		} else if (g_str_equal (property, "Connected") == TRUE) {
			gboolean connected = g_variant_get_boolean (v);

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_CONNECTED, connected, -1);
		} else if (g_str_equal (property, "UUIDs") == TRUE) {
			char **uuids;

			uuids = device_list_uuids_v (v);
			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_UUIDS, uuids, -1);
			g_strfreev (uuids);
		} else if (g_str_equal (property, "LegacyPairing") == TRUE) {
			gboolean legacypairing;

			legacypairing = g_variant_get_boolean (v);
			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_LEGACYPAIRING, legacypairing,
					    -1);
		} else if (g_str_equal (property, "Class") == TRUE ||
			   g_str_equal (property, "Appearance") == TRUE) {
			BluetoothType type;
			const char *icon = NULL;
			char *bdaddr;

			gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
					    BLUETOOTH_COLUMN_ADDRESS, &bdaddr,
					    -1);

			if (g_str_equal (property, "Class") == TRUE)
				type = bluetooth_class_to_type (g_variant_get_uint32 (v));
			else
				type = bluetooth_appearance_to_type (g_variant_get_uint16 (v));
			icon = icon_override (bdaddr, type);

			g_free (bdaddr);

			if (icon) {
				gtk_tree_store_set (priv->store, &iter,
						    BLUETOOTH_COLUMN_TYPE, type,
						    BLUETOOTH_COLUMN_ICON, icon,
						    -1);
			} else if (type != 0) {
				gtk_tree_store_set (priv->store, &iter,
						    BLUETOOTH_COLUMN_TYPE, type,
						    -1);
			}
		} else {
			g_debug ("Unhandled property: %s", property);
		}

		g_variant_unref (v);
	}
}

static void
device_added (GDBusObjectManager   *manager,
	      const char           *path,
	      BluetoothClient      *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GDBusProxy *adapter;
	Device1 *device;
	Properties *properties;
	const char *adapter_path, *address, *alias, *name, *icon;
	char **uuids;
	gboolean paired, trusted, connected;
	int legacypairing;
	BluetoothType type;
	GtkTreeIter iter, parent;
	guint16 appearance;

	device = device1_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
						 G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
						 BLUEZ_SERVICE,
						 path,
						 NULL,
						 NULL);
	if (device == NULL)
		return;

	properties = properties_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
							BLUEZ_SERVICE,
							path,
							NULL,
							NULL);

	adapter_path = device1_get_adapter (device);
	address = device1_get_address (device);
	alias = device1_get_alias (device);
	name = device1_get_name (device);

	appearance = device1_get_appearance (device);
	type = appearance ? bluetooth_appearance_to_type (appearance) : BLUETOOTH_TYPE_ANY;
	icon = icon_override (address, type);

	if (type == BLUETOOTH_TYPE_ANY) {
		guint32 class;
		class = device1_get_class (device);
		type = class ? bluetooth_class_to_type (class) : BLUETOOTH_TYPE_ANY;
		icon = icon_override (address, type);
	}

	if (icon == NULL)
		icon = device1_get_icon (device);

	paired = device1_get_paired (device);
	trusted = device1_get_trusted (device);
	connected = device1_get_connected (device);
	uuids = device_list_uuids (device1_get_uuids (device));
	legacypairing = device1_get_legacy_pairing (device);

	if (get_iter_from_path (priv->store, &parent, adapter_path) == FALSE) {
		g_object_unref (device);
		return;
	}

	gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &parent,
			    BLUETOOTH_COLUMN_PROXY, &adapter, -1);

	if (get_iter_from_address (priv->store, &iter, address, adapter) == FALSE) {
		gtk_tree_store_insert_with_values (priv->store, &iter, &parent, -1,
						   BLUETOOTH_COLUMN_ADDRESS, address,
						   BLUETOOTH_COLUMN_ALIAS, alias,
						   BLUETOOTH_COLUMN_NAME, name,
						   BLUETOOTH_COLUMN_TYPE, type,
						   BLUETOOTH_COLUMN_ICON, icon,
						   BLUETOOTH_COLUMN_LEGACYPAIRING, legacypairing,
						   BLUETOOTH_COLUMN_UUIDS, uuids,
						   BLUETOOTH_COLUMN_PAIRED, paired,
						   BLUETOOTH_COLUMN_CONNECTED, connected,
						   BLUETOOTH_COLUMN_TRUSTED, trusted,
						   BLUETOOTH_COLUMN_PROXY, device,
						   BLUETOOTH_COLUMN_PROPERTIES, properties,
						   -1);
	} else {
		gtk_tree_store_set(priv->store, &iter,
				   BLUETOOTH_COLUMN_ADDRESS, address,
				   BLUETOOTH_COLUMN_ALIAS, alias,
				   BLUETOOTH_COLUMN_NAME, name,
				   BLUETOOTH_COLUMN_TYPE, type,
				   BLUETOOTH_COLUMN_ICON, icon,
				   BLUETOOTH_COLUMN_LEGACYPAIRING, legacypairing,
				   BLUETOOTH_COLUMN_UUIDS, uuids,
				   BLUETOOTH_COLUMN_PAIRED, paired,
				   BLUETOOTH_COLUMN_CONNECTED, connected,
				   BLUETOOTH_COLUMN_TRUSTED, trusted,
				   BLUETOOTH_COLUMN_PROXY, device,
				   BLUETOOTH_COLUMN_PROPERTIES, properties,
				   -1);
	}
	g_strfreev (uuids);

	g_signal_connect (G_OBJECT (device), "g-properties-changed",
			  G_CALLBACK (device_g_properties_changed), client);

	g_object_unref (properties);
	g_object_unref (device);
	g_object_unref (adapter);
}

static void
device_removed (const char      *path,
		BluetoothClient *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GtkTreeIter iter;

	if (get_iter_from_path(priv->store, &iter, path) == TRUE) {
		g_signal_emit (G_OBJECT (client), signals[DEVICE_REMOVED], 0, path);
		gtk_tree_store_remove(priv->store, &iter);
	}
}

static void
powered_callback (GDBusProxy   *proxy,
		  GAsyncResult *res,
		  gpointer	data)
{
	GError *error = NULL;

	if (!properties_call_set_finish (PROPERTIES(proxy), res, &error)) {
		g_debug ("Call to Set Powered failed %s: %s",
			 g_dbus_proxy_get_object_path (proxy), error->message);
		g_error_free (error);
	}

	g_object_unref (proxy);
}

static gboolean
adapter_set_powered (BluetoothClient *client,
		     const char *path,
		     gboolean powered)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	Properties *properties;
	GtkTreeIter iter;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), FALSE);

	if (get_iter_from_path (priv->store, &iter, path) == FALSE)
		return FALSE;

	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
			    BLUETOOTH_COLUMN_PROPERTIES, &properties, -1);

	if (properties == NULL)
		return FALSE;


	properties_call_set (properties,
			     BLUEZ_ADAPTER_INTERFACE,
			     "Powered",
			     g_variant_new_variant (g_variant_new_boolean (powered)),
			     NULL,
			     (GAsyncReadyCallback) powered_callback,
			     NULL);

	return TRUE;
}

static void
default_adapter_changed (GDBusObjectManager   *manager,
			 const char           *path,
			 BluetoothClient      *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GtkTreeIter iter;
	GtkTreePath *tree_path;
	gboolean powered;

	g_assert (!priv->default_adapter);

	if (get_iter_from_path (priv->store, &iter, path) == FALSE)
		return;

	tree_path = gtk_tree_model_get_path (GTK_TREE_MODEL (priv->store), &iter);
	priv->default_adapter = gtk_tree_row_reference_new (GTK_TREE_MODEL (priv->store), tree_path);
	gtk_tree_path_free (tree_path);

	gtk_tree_store_set (priv->store, &iter,
			    BLUETOOTH_COLUMN_DEFAULT, TRUE, -1);

	gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
			   BLUETOOTH_COLUMN_POWERED, &powered, -1);

	if (powered) {
		g_object_notify (G_OBJECT (client), "default-adapter");
		g_object_notify (G_OBJECT (client), "default-adapter-powered");
		g_object_notify (G_OBJECT (client), "default-adapter-discoverable");
		g_object_notify (G_OBJECT (client), "default-adapter-discovering");
		g_object_notify (G_OBJECT (client), "default-adapter-name");
		return;
	}

	/*
	 * If the adapter is turn off (Powered = False in bluetooth) object
	 * notifications will be sent only when a Powered = True signal arrives
	 * from bluetoothd
	 */
	adapter_set_powered (client, path, TRUE);
}

static void
adapter_g_properties_changed (GDBusProxy      *adapter,
			      GVariant        *changed_p,
			      GStrv            invalidated_p,
			      BluetoothClient *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GVariantIter i;
	const char *property;
	GtkTreeIter iter;
	GVariant *v;
	gboolean notify = FALSE;

	if (get_iter_from_proxy (priv->store, &iter, adapter) == FALSE)
		return;

	g_variant_iter_init (&i, changed_p);
	while (g_variant_iter_next (&i, "{&sv}", &property, &v)) {
		if (g_str_equal (property, "Name") == TRUE) {
			const gchar *name = g_variant_get_string (v, NULL);
			gboolean is_default;

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_NAME, name, -1);
			gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
					    BLUETOOTH_COLUMN_DEFAULT, &is_default, -1);
			if (is_default != FALSE) {
				g_object_notify (G_OBJECT (client), "default-adapter-powered");
				g_object_notify (G_OBJECT (client), "default-adapter-name");
			}
			notify = TRUE;
		} else if (g_str_equal (property, "Discovering") == TRUE) {
			gboolean discovering = g_variant_get_boolean (v);
			gboolean is_default;

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_DISCOVERING, discovering, -1);
			gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
					    BLUETOOTH_COLUMN_DEFAULT, &is_default, -1);
			if (is_default != FALSE)
				g_object_notify (G_OBJECT (client), "default-adapter-discovering");
			notify = TRUE;
		} else if (g_str_equal (property, "Powered") == TRUE) {
			gboolean powered = g_variant_get_boolean (v);
			gboolean is_default;

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_POWERED, powered, -1);
			gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
					    BLUETOOTH_COLUMN_DEFAULT, &is_default, -1);
			if (is_default != FALSE && powered) {
				g_object_notify (G_OBJECT (client), "default-adapter");
				g_object_notify (G_OBJECT (client), "default-adapter-powered");
				g_object_notify (G_OBJECT (client), "default-adapter-discoverable");
				g_object_notify (G_OBJECT (client), "default-adapter-discovering");
				g_object_notify (G_OBJECT (client), "default-adapter-name");
			}
			notify = TRUE;
		} else if (g_str_equal (property, "Discoverable") == TRUE) {
			gboolean discoverable = g_variant_get_boolean (v);
			gboolean is_default;

			gtk_tree_store_set (priv->store, &iter,
					    BLUETOOTH_COLUMN_DISCOVERABLE, discoverable, -1);
			gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
					    BLUETOOTH_COLUMN_DEFAULT, &is_default, -1);
			if (is_default != FALSE)
				g_object_notify (G_OBJECT (client), "default-adapter-discoverable");
			notify = TRUE;
		}

		if (notify != FALSE) {
			GtkTreePath *path;

			/* Tell the world */
			path = gtk_tree_model_get_path (GTK_TREE_MODEL (priv->store), &iter);
			gtk_tree_model_row_changed (GTK_TREE_MODEL (priv->store), path, &iter);
			gtk_tree_path_free (path);
		}
		g_variant_unref (v);
	}
}

static void
adapter_added (GDBusObjectManager   *manager,
	       const char           *path,
	       BluetoothClient      *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GtkTreeIter iter;
	Adapter1 *adapter;
	Properties *properties;
	const gchar *address, *name;
	gboolean discovering, discoverable, powered;

	adapter = adapter1_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
						   G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
						   BLUEZ_SERVICE,
						   path,
						   NULL,
						   NULL);

	properties = properties_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
							BLUEZ_SERVICE,
							path,
							NULL,
							NULL);

	address = adapter1_get_address (adapter);
	name = adapter1_get_name (adapter);
	discovering = adapter1_get_discovering (adapter);
	powered = adapter1_get_powered (adapter);
	discoverable = adapter1_get_discoverable (adapter);

	gtk_tree_store_insert_with_values(priv->store, &iter, NULL, -1,
					  BLUETOOTH_COLUMN_PROXY, adapter,
					  BLUETOOTH_COLUMN_PROPERTIES, properties,
					  BLUETOOTH_COLUMN_ADDRESS, address,
					  BLUETOOTH_COLUMN_NAME, name,
					  BLUETOOTH_COLUMN_DISCOVERING, discovering,
					  BLUETOOTH_COLUMN_DISCOVERABLE, discoverable,
					  BLUETOOTH_COLUMN_POWERED, powered,
					  -1);

	g_signal_connect (G_OBJECT (adapter), "g-properties-changed",
			  G_CALLBACK (adapter_g_properties_changed), client);

	if (!priv->default_adapter)
		default_adapter_changed (manager, path, client);

	g_object_unref (properties);
	g_object_unref (adapter);
}

static void
adapter_removed (GDBusObjectManager   *manager,
		 const char           *path,
		 BluetoothClient      *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GtkTreeIter iter;
	gboolean was_default;

	if (get_iter_from_path (priv->store, &iter, path) == FALSE)
		return;

	gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
			   BLUETOOTH_COLUMN_DEFAULT, &was_default, -1);

	if (!was_default)
		return;

	g_clear_pointer (&priv->default_adapter, gtk_tree_row_reference_free);
	gtk_tree_store_remove (priv->store, &iter);

	if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL(priv->store),
					   &iter)) {
		GDBusProxy *adapter;
		const char *adapter_path;

		gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
				   BLUETOOTH_COLUMN_PROXY, &adapter, -1);

		adapter_path = g_dbus_proxy_get_object_path (adapter);
		default_adapter_changed (manager, adapter_path, client);

		g_object_unref(adapter);
	} else {
		g_object_notify (G_OBJECT (client), "default-adapter");
		g_object_notify (G_OBJECT (client), "default-adapter-powered");
		g_object_notify (G_OBJECT (client), "default-adapter-discoverable");
		g_object_notify (G_OBJECT (client), "default-adapter-discovering");
	}
}

static GType
object_manager_get_proxy_type_func (GDBusObjectManagerClient *manager,
				    const gchar              *object_path,
				    const gchar              *interface_name,
				    gpointer                  user_data)
{
	if (interface_name == NULL)
		return G_TYPE_DBUS_OBJECT_PROXY;

	if (g_str_equal (interface_name, BLUEZ_DEVICE_INTERFACE))
		return TYPE_DEVICE1_PROXY;
	if (g_str_equal (interface_name, BLUEZ_ADAPTER_INTERFACE))
		return TYPE_ADAPTER1_PROXY;

	return G_TYPE_DBUS_PROXY;
}

static void
interface_added (GDBusObjectManager *manager,
		 GDBusObject        *object,
		 GDBusInterface     *interface,
		 gpointer            user_data)
{
	BluetoothClient *client = user_data;

	if (IS_ADAPTER1 (interface)) {
		adapter_added (manager,
			       g_dbus_proxy_get_object_path (G_DBUS_PROXY (object)),
			       client);
	} else if (IS_DEVICE1 (interface)) {
		device_added (manager,
			      g_dbus_proxy_get_object_path (G_DBUS_PROXY (object)),
			      client);
	}
}

static void
interface_removed (GDBusObjectManager *manager,
		   GDBusObject        *object,
		   gpointer            user_data)
{
	BluetoothClient *client = user_data;

	if (IS_ADAPTER1 (object)) {
		adapter_removed (manager,
				 g_dbus_proxy_get_object_path (G_DBUS_PROXY (object)),
				 client);
	} else if (IS_DEVICE1 (object)) {
		device_removed (g_dbus_proxy_get_object_path (G_DBUS_PROXY (object)),
				client);
	}
}

static void
object_manager_new_callback(GObject      *source_object,
			    GAsyncResult *res,
			    void         *user_data)
{
	BluetoothClient  *client = BLUETOOTH_CLIENT (user_data);
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GList *object_list, *l;
	GError *error = NULL;

	priv->manager = g_dbus_object_manager_client_new_for_bus_finish (res, &error);
	if (error) {
		g_warning ("Could not create bluez object manager: %s", error->message);
		g_error_free (error);
		return;
	}

	g_signal_connect (G_OBJECT (priv->manager), "interface-added", (GCallback) interface_added, client);
	g_signal_connect (G_OBJECT (priv->manager), "interface-removed", (GCallback) interface_removed, client);

	object_list = g_dbus_object_manager_get_objects (priv->manager);

	/* We need to add the adapters first, otherwise the devices will
	 * be dropped to the floor, as they wouldn't have a parent in
	 * the treestore */
	for (l = object_list; l != NULL; l = l->next) {
		GDBusObject *object = l->data;
		GDBusInterface *iface;

		iface = g_dbus_object_get_interface (object, BLUEZ_ADAPTER_INTERFACE);
		if (!iface)
			continue;

		adapter_added (priv->manager,
			       g_dbus_object_get_object_path (object),
			       client);
	}

	for (l = object_list; l != NULL; l = l->next) {
		GDBusObject *object = l->data;
		GDBusInterface *iface;

		iface = g_dbus_object_get_interface (object, BLUEZ_DEVICE_INTERFACE);
		if (!iface)
			continue;

		device_added (priv->manager,
			      g_dbus_object_get_object_path (object),
			      client);
	}
	g_list_free_full (object_list, g_object_unref);
}

static void bluetooth_client_init(BluetoothClient *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);

	priv->store = gtk_tree_store_new(_BLUETOOTH_NUM_COLUMNS,
					 G_TYPE_OBJECT,     /* BLUETOOTH_COLUMN_PROXY */
					 G_TYPE_OBJECT,     /* BLUETOOTH_COLUMN_PROPERTIES */
					 G_TYPE_STRING,     /* BLUETOOTH_COLUMN_ADDRESS */
					 G_TYPE_STRING,     /* BLUETOOTH_COLUMN_ALIAS */
					 G_TYPE_STRING,     /* BLUETOOTH_COLUMN_NAME */
					 G_TYPE_UINT,       /* BLUETOOTH_COLUMN_TYPE */
					 G_TYPE_STRING,     /* BLUETOOTH_COLUMN_ICON */
					 G_TYPE_BOOLEAN,    /* BLUETOOTH_COLUMN_DEFAULT */
					 G_TYPE_BOOLEAN,    /* BLUETOOTH_COLUMN_PAIRED */
					 G_TYPE_BOOLEAN,    /* BLUETOOTH_COLUMN_TRUSTED */
					 G_TYPE_BOOLEAN,    /* BLUETOOTH_COLUMN_CONNECTED */
					 G_TYPE_BOOLEAN,    /* BLUETOOTH_COLUMN_DISCOVERABLE */
					 G_TYPE_BOOLEAN,    /* BLUETOOTH_COLUMN_DISCOVERING */
					 G_TYPE_INT,        /* BLUETOOTH_COLUMN_LEGACYPAIRING */
					 G_TYPE_BOOLEAN,    /* BLUETOOTH_COLUMN_POWERED */
					 G_TYPE_HASH_TABLE, /* BLUETOOTH_COLUMN_SERVICES */
					 G_TYPE_STRV);      /* BLUETOOTH_COLUMN_UUIDS */

	g_dbus_object_manager_client_new_for_bus (G_BUS_TYPE_SYSTEM,
						  G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
						  BLUEZ_SERVICE,
						  BLUEZ_MANAGER_PATH,
						  object_manager_get_proxy_type_func,
						  NULL, NULL,
						  NULL,
						  object_manager_new_callback, client);
}

static GDBusProxy *
_bluetooth_client_get_default_adapter(BluetoothClient *client)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GtkTreePath *path;
	GtkTreeIter iter;
	GDBusProxy *adapter;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), NULL);

	if (priv->default_adapter == NULL)
		return NULL;

	path = gtk_tree_row_reference_get_path (priv->default_adapter);
	gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path);
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
			    BLUETOOTH_COLUMN_PROXY, &adapter, -1);
	gtk_tree_path_free (path);

	return adapter;
}

static const char*
_bluetooth_client_get_default_adapter_path (BluetoothClient *self)
{
	GDBusProxy *adapter = _bluetooth_client_get_default_adapter (self);

	if (adapter != NULL) {
		const char *ret = g_dbus_proxy_get_object_path (adapter);
		g_object_unref (adapter);
		return ret;
	}
	return NULL;
}

static gboolean
_bluetooth_client_get_default_adapter_powered (BluetoothClient *self)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE (self);
	GtkTreePath *path;
	GtkTreeIter iter;
	gboolean ret;

	if (priv->default_adapter == NULL)
		return FALSE;

	path = gtk_tree_row_reference_get_path (priv->default_adapter);
	gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path);
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter, BLUETOOTH_COLUMN_POWERED, &ret, -1);
	gtk_tree_path_free (path);

	return ret;
}

static char *
_bluetooth_client_get_default_adapter_name (BluetoothClient *self)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE (self);
	GtkTreePath *path;
	GtkTreeIter iter;
	char *ret;

	if (priv->default_adapter == NULL)
		return NULL;

	path = gtk_tree_row_reference_get_path (priv->default_adapter);
	gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path);
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter, BLUETOOTH_COLUMN_NAME, &ret, -1);
	gtk_tree_path_free (path);

	return ret;
}

/**
 * _bluetooth_client_get_discoverable:
 * @client: a #BluetoothClient
 *
 * Gets the default adapter's discoverable status, cached in the adapter model.
 *
 * Returns: the discoverable status, or FALSE if no default adapter exists
 */
static gboolean
_bluetooth_client_get_discoverable (BluetoothClient *client)
{
	BluetoothClientPrivate *priv;
	GtkTreePath *path;
	GtkTreeIter iter;
	gboolean ret;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), FALSE);

	priv = BLUETOOTH_CLIENT_GET_PRIVATE (client);
	if (priv->default_adapter == NULL)
		return FALSE;

	path = gtk_tree_row_reference_get_path (priv->default_adapter);
	gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path);
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
                            BLUETOOTH_COLUMN_DISCOVERABLE, &ret, -1);

	return ret;
}

/**
 * _bluetooth_client_set_discoverable:
 * @client: a #BluetoothClient object
 * @discoverable: whether the device should be discoverable
 * @timeout: timeout in seconds for making undiscoverable, or 0 for never
 *
 * Sets the default adapter's discoverable status.
 *
 * Return value: Whether setting the state on the default adapter was successful.
 **/
static gboolean
_bluetooth_client_set_discoverable (BluetoothClient *client,
				    gboolean discoverable,
				    guint timeout)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE (client);
	GError *error = NULL;
	GtkTreePath *path;
	Properties *properties;
	gboolean ret;
	GtkTreeIter iter;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), FALSE);

	if (priv->default_adapter == NULL)
		return FALSE;

	path = gtk_tree_row_reference_get_path (priv->default_adapter);
	gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path);
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
                            BLUETOOTH_COLUMN_PROPERTIES, &properties, -1);
        gtk_tree_path_free (path);

	if (properties == NULL)
		return FALSE;

	ret = properties_call_set_sync (properties,
					BLUEZ_ADAPTER_INTERFACE,
					"Discoverable",
					g_variant_new_variant (g_variant_new_boolean (discoverable)),
					NULL, &error);
	if (ret == FALSE) {
		g_warning ("Failed to set Discoverable to %d: %s", discoverable, error->message);
		g_error_free (error);
	} else if (discoverable) {
		ret = properties_call_set_sync (properties,
						BLUEZ_ADAPTER_INTERFACE,
						"DiscoverableTimeout",
						g_variant_new_variant (g_variant_new_uint32 (timeout)),
						NULL, &error);
		if (ret == FALSE) {
			g_warning ("Failed to set DiscoverableTimeout to %d: %s", timeout, error->message);
			g_error_free (error);
		}
	}

	g_object_unref (properties);

	return ret;
}

static void
_bluetooth_client_set_default_adapter_discovering (BluetoothClient *client,
						   gboolean         discover)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE (client);
	GtkTreeIter iter;
	GDBusProxy *adapter;
	gboolean current;

	adapter = _bluetooth_client_get_default_adapter (client);
	if (adapter == NULL)
		return;

	get_iter_from_proxy (priv->store, &iter, adapter);
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
			    BLUETOOTH_COLUMN_DISCOVERING, &current, -1);

	if (current == discover) {
		g_object_unref(adapter);
		return;
	}

	if (discover)
		adapter1_call_start_discovery_sync (ADAPTER1 (adapter), NULL, NULL);
	else
		adapter1_call_stop_discovery_sync (ADAPTER1 (adapter), NULL, NULL);

	g_object_unref(adapter);
}

static gboolean
_bluetooth_client_get_default_adapter_discovering (BluetoothClient *self)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE (self);
	GtkTreePath *path;
	GtkTreeIter iter;
	gboolean ret;

	if (priv->default_adapter == NULL)
		return FALSE;

	path = gtk_tree_row_reference_get_path (priv->default_adapter);
	gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path);
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter, BLUETOOTH_COLUMN_DISCOVERING, &ret, -1);
	gtk_tree_path_free (path);

	return ret;
}

static void
bluetooth_client_get_property (GObject        *object,
			       guint           property_id,
			       GValue         *value,
			       GParamSpec     *pspec)
{
	BluetoothClient *self = BLUETOOTH_CLIENT (object);

	switch (property_id) {
	case PROP_DEFAULT_ADAPTER:
		g_value_set_string (value, _bluetooth_client_get_default_adapter_path (self));
		break;
	case PROP_DEFAULT_ADAPTER_POWERED:
		g_value_set_boolean (value, _bluetooth_client_get_default_adapter_powered (self));
		break;
	case PROP_DEFAULT_ADAPTER_NAME:
		g_value_take_string (value, _bluetooth_client_get_default_adapter_name (self));
		break;
	case PROP_DEFAULT_ADAPTER_DISCOVERABLE:
		g_value_set_boolean (value, _bluetooth_client_get_discoverable (self));
		break;
	case PROP_DEFAULT_ADAPTER_DISCOVERING:
		g_value_set_boolean (value, _bluetooth_client_get_default_adapter_discovering (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
bluetooth_client_set_property (GObject        *object,
			       guint           property_id,
			       const GValue   *value,
			       GParamSpec     *pspec)
{
	BluetoothClient *self = BLUETOOTH_CLIENT (object);

	switch (property_id) {
	case PROP_DEFAULT_ADAPTER_DISCOVERABLE:
	        _bluetooth_client_set_discoverable (self, g_value_get_boolean (value), 0);
		break;
	case PROP_DEFAULT_ADAPTER_DISCOVERING:
		_bluetooth_client_set_default_adapter_discovering (self, g_value_get_boolean (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void bluetooth_client_finalize(GObject *object)
{
	BluetoothClient *client = BLUETOOTH_CLIENT (object);
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE (client);

	g_clear_object (&priv->manager);
	g_object_unref (priv->store);

	g_clear_pointer (&priv->default_adapter, gtk_tree_row_reference_free);

	G_OBJECT_CLASS(bluetooth_client_parent_class)->finalize (object);
}

static void bluetooth_client_class_init(BluetoothClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = bluetooth_client_finalize;
	object_class->get_property = bluetooth_client_get_property;
	object_class->set_property = bluetooth_client_set_property;

	/**
	 * BluetoothClient::device-removed:
	 * @client: a #BluetoothClient object which received the signal
	 * @device: the D-Bus object path for the now-removed device
	 *
	 * The #BluetoothClient::device-removed signal is launched when a
	 * device gets removed from the model.
	 **/
	signals[DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      0,
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * BluetoothClient:default-adapter:
	 *
	 * The D-Bus path of the default Bluetooth adapter or %NULL.
	 */
	g_object_class_install_property (object_class, PROP_DEFAULT_ADAPTER,
					 g_param_spec_string ("default-adapter", NULL,
							      "The D-Bus path of the default adapter",
							      NULL, G_PARAM_READABLE));
	/**
	 * BluetoothClient:default-adapter-powered:
	 *
	 * %TRUE if the default Bluetooth adapter is powered.
	 */
	g_object_class_install_property (object_class, PROP_DEFAULT_ADAPTER_POWERED,
					 g_param_spec_boolean ("default-adapter-powered", NULL,
							      "Whether the default adapter is powered",
							       FALSE, G_PARAM_READABLE));
	/**
	 * BluetoothClient:default-adapter-discoverable:
	 *
	 * %TRUE if the default Bluetooth adapter is discoverable.
	 */
	g_object_class_install_property (object_class, PROP_DEFAULT_ADAPTER_DISCOVERABLE,
					 g_param_spec_boolean ("default-adapter-discoverable", NULL,
							      "Whether the default adapter is visible by other devices",
							       FALSE, G_PARAM_READWRITE));
	/**
	 * BluetoothClient:default-adapter-name:
	 *
	 * The name of the default Bluetooth adapter or %NULL.
	 */
	g_object_class_install_property (object_class, PROP_DEFAULT_ADAPTER_NAME,
					 g_param_spec_string ("default-adapter-name", NULL,
							      "The human readable name of the default adapter",
							      NULL, G_PARAM_READABLE));
	/**
	 * BluetoothClient:default-adapter-discovering:
	 *
	 * %TRUE if the default Bluetooth adapter is discovering.
	 */
	g_object_class_install_property (object_class, PROP_DEFAULT_ADAPTER_DISCOVERING,
					 g_param_spec_boolean ("default-adapter-discovering", NULL,
							      "Whether the default adapter is searching for devices",
							       FALSE, G_PARAM_READWRITE));
}

/**
 * bluetooth_client_new:
 *
 * Returns a reference to the #BluetoothClient singleton. Use g_object_unref() when done with the object.
 *
 * Return value: (transfer full): a #BluetoothClient object.
 **/
BluetoothClient *bluetooth_client_new(void)
{
	static BluetoothClient *bluetooth_client = NULL;

	if (bluetooth_client != NULL)
		return g_object_ref(bluetooth_client);

	bluetooth_client = BLUETOOTH_CLIENT (g_object_new (BLUETOOTH_TYPE_CLIENT, NULL));
	g_object_add_weak_pointer (G_OBJECT (bluetooth_client),
				   (gpointer) &bluetooth_client);

	return bluetooth_client;
}

/**
 * bluetooth_client_get_model:
 * @client: a #BluetoothClient object
 *
 * Returns an unfiltered #GtkTreeModel representing the adapter and devices available on the system.
 *
 * Return value: (transfer full): a #GtkTreeModel object.
 **/
GtkTreeModel *bluetooth_client_get_model (BluetoothClient *client)
{
	BluetoothClientPrivate *priv;
	GtkTreeModel *model;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), NULL);

	priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	model = g_object_ref(priv->store);

	return model;
}

/**
 * bluetooth_client_get_filter_model:
 * @client: a #BluetoothClient object
 * @func: a #GtkTreeModelFilterVisibleFunc
 * @data: user data to pass to gtk_tree_model_filter_set_visible_func()
 * @destroy: a destroy function for gtk_tree_model_filter_set_visible_func()
 *
 * Returns a #GtkTreeModelFilter of devices filtered using the @func, @data and @destroy arguments to pass to gtk_tree_model_filter_set_visible_func().
 *
 * Return value: (transfer full): a #GtkTreeModel object.
 **/
GtkTreeModel *bluetooth_client_get_filter_model (BluetoothClient *client,
						 GtkTreeModelFilterVisibleFunc func,
						 gpointer data, GDestroyNotify destroy)
{
	BluetoothClientPrivate *priv;
	GtkTreeModel *model;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), NULL);

	priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	model = gtk_tree_model_filter_new(GTK_TREE_MODEL(priv->store), NULL);

	gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(model),
							func, data, destroy);

	return model;
}

static gboolean adapter_filter(GtkTreeModel *model,
					GtkTreeIter *iter, gpointer user_data)
{
	GDBusProxy *proxy;
	gboolean active;

	gtk_tree_model_get(model, iter, BLUETOOTH_COLUMN_PROXY, &proxy, -1);

	if (proxy == NULL)
		return FALSE;

	active = g_str_equal(BLUEZ_ADAPTER_INTERFACE,
					g_dbus_proxy_get_interface_name(proxy));

	g_object_unref(proxy);

	return active;
}

/**
 * bluetooth_client_get_adapter_model:
 * @client: a #BluetoothClient object
 *
 * Returns a #GtkTreeModelFilter with only adapters present.
 *
 * Return value: (transfer full): a #GtkTreeModel object.
 **/
GtkTreeModel *bluetooth_client_get_adapter_model (BluetoothClient *client)
{
	return bluetooth_client_get_filter_model (client, adapter_filter,
						  NULL, NULL);
}

/**
 * bluetooth_client_get_device_model:
 * @client: a #BluetoothClient object
 *
 * Returns a #GtkTreeModelFilter with only devices belonging to the default adapter listed.
 * Note that the model will follow a specific adapter, and will not follow the default adapter.
 * Also note that due to the way #GtkTreeModelFilter works, you will probably want to
 * monitor signals on the "child-model" #GtkTreeModel to monitor for changes.
 *
 * Return value: (transfer full): a #GtkTreeModel object.
 **/
GtkTreeModel *bluetooth_client_get_device_model (BluetoothClient *client)
{
	BluetoothClientPrivate *priv;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;
	gboolean cont, found = FALSE;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), NULL);

	priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	cont = gtk_tree_model_get_iter_first (GTK_TREE_MODEL(priv->store), &iter);

	while (cont == TRUE) {
		gboolean is_default;

		gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
				    BLUETOOTH_COLUMN_DEFAULT, &is_default, -1);

		if (is_default == TRUE) {
			found = TRUE;
			break;
		}

		cont = gtk_tree_model_iter_next (GTK_TREE_MODEL(priv->store), &iter);
	}

	if (found == TRUE) {
		path = gtk_tree_model_get_path (GTK_TREE_MODEL(priv->store), &iter);
		model = gtk_tree_model_filter_new (GTK_TREE_MODEL(priv->store), path);
		gtk_tree_path_free (path);
	} else
		model = NULL;

	return model;
}

typedef struct {
	BluetoothClientSetupFunc func;
	BluetoothClient *client;
} CreateDeviceData;

static void
device_pair_callback (GDBusProxy         *proxy,
		      GAsyncResult       *res,
		      GSimpleAsyncResult *simple)
{
	GError *error = NULL;

	if (device1_call_pair_finish (DEVICE1(proxy), res, &error) == FALSE) {
		g_debug ("Pair() failed for %s: %s",
			 g_dbus_proxy_get_object_path (proxy),
			 error->message);
		g_simple_async_result_take_error (simple, error);
	} else {
		g_simple_async_result_set_op_res_gboolean (simple, TRUE);
	}

	g_simple_async_result_complete_in_idle (simple);

	g_object_unref (simple);
}

gboolean
bluetooth_client_setup_device_finish (BluetoothClient  *client,
				      GAsyncResult     *res,
				      char            **path,
				      GError          **error)
{
	GSimpleAsyncResult *simple;

	simple = (GSimpleAsyncResult *) res;

	g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == bluetooth_client_setup_device);

	if (path != NULL)
		*path = g_object_get_data (G_OBJECT (res), "device-object-path");

	if (g_simple_async_result_get_op_res_gboolean (simple))
		return TRUE;
	g_simple_async_result_propagate_error (simple, error);
	return FALSE;
}

void
bluetooth_client_setup_device (BluetoothClient          *client,
			       const char               *path,
			       gboolean                  pair,
			       GCancellable             *cancellable,
			       GAsyncReadyCallback       callback,
			       gpointer                  user_data)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GSimpleAsyncResult *simple;
	GDBusProxy *device;
	GtkTreeIter iter, adapter_iter;
	gboolean paired;
	GError *err = NULL;

	g_return_if_fail (BLUETOOTH_IS_CLIENT (client));

	simple = g_simple_async_result_new (G_OBJECT (client),
					    callback,
					    user_data,
					    bluetooth_client_setup_device);
	g_simple_async_result_set_check_cancellable (simple, cancellable);
	g_object_set_data (G_OBJECT (simple), "device-object-path", g_strdup (path));

	if (get_iter_from_path (priv->store, &iter, path) == FALSE) {
		g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
						 "Device with object path %s does not exist",
						 path);
		g_simple_async_result_complete_in_idle (simple);
		g_object_unref (simple);
		return;
	}

	gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
			    BLUETOOTH_COLUMN_PROXY, &device,
			    BLUETOOTH_COLUMN_PAIRED, &paired, -1);

	if (paired != FALSE &&
	    gtk_tree_model_iter_parent (GTK_TREE_MODEL(priv->store), &adapter_iter, &iter)) {
		GDBusProxy *adapter;

		gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &adapter_iter,
				    BLUETOOTH_COLUMN_PROXY, &adapter,
				    -1);
		adapter1_call_remove_device_sync (ADAPTER1 (adapter),
						  path,
						  NULL, &err);
		if (err != NULL) {
			g_warning ("Failed to remove device: %s", err->message);
			g_error_free (err);
		}
		g_object_unref (adapter);
	}

	if (pair == TRUE) {
		device1_call_pair (DEVICE1(device),
				   cancellable,
				   (GAsyncReadyCallback) device_pair_callback,
				   simple);
	} else {
		g_simple_async_result_set_op_res_gboolean (simple, TRUE);
		g_simple_async_result_complete_in_idle (simple);
		g_object_unref (simple);
	}

	g_object_unref (device);
}

gboolean
bluetooth_client_set_trusted (BluetoothClient *client,
			      const char      *device,
			      gboolean         trusted)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	Properties *properties;
	GError     *error = NULL;
	GtkTreeIter iter;
	gboolean   ret;

	g_return_val_if_fail (BLUETOOTH_IS_CLIENT (client), FALSE);
	g_return_val_if_fail (device != NULL, FALSE);

	if (get_iter_from_path (priv->store, &iter, device) == FALSE) {
		g_debug ("Couldn't find device '%s' in tree to mark it as trusted", device);
		return FALSE;
	}

	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
			    BLUETOOTH_COLUMN_PROPERTIES, &properties, -1);

	if (properties == NULL) {
		g_debug ("Couldn't find properties for device '%s' in tree to mark it as trusted", device);
		return FALSE;
	}

	ret = properties_call_set_sync (properties, BLUEZ_DEVICE_INTERFACE, "Trusted",
					g_variant_new_variant (g_variant_new_boolean (trusted)),
					NULL, &error);
	if (ret == FALSE) {
		g_warning ("Failed to set Trusted to %d: %s", trusted, error->message);
		g_error_free (error);
	}

	g_object_unref (properties);
	return ret;
}

GDBusProxy *
bluetooth_client_get_device (BluetoothClient *client,
			     const char       *path)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GtkTreeIter iter;
	GDBusProxy *proxy;

	if (get_iter_from_path (priv->store, &iter, path) == FALSE) {
		return NULL;
	}

	gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
			                BLUETOOTH_COLUMN_PROXY, &proxy,
			                -1);
	return proxy;
}

static void
connect_callback (GDBusProxy         *proxy,
		  GAsyncResult       *res,
		  GSimpleAsyncResult *simple)
{
	gboolean retval;
	GError *error = NULL;

	retval = device1_call_connect_finish (DEVICE1(proxy), res, &error);
	if (retval == FALSE) {
		g_debug ("Connect failed for %s: %s",
			 g_dbus_proxy_get_object_path (proxy), error->message);
		g_simple_async_result_take_error (simple, error);
	} else {
		g_debug ("Connect succeeded for %s",
			 g_dbus_proxy_get_object_path (proxy));
		g_simple_async_result_set_op_res_gboolean (simple, retval);
	}

	g_simple_async_result_complete_in_idle (simple);

	g_object_unref (simple);
}

static void
disconnect_callback (GDBusProxy   *proxy,
		     GAsyncResult *res,
		     GSimpleAsyncResult *simple)
{
	gboolean retval;
	GError *error = NULL;

	retval = device1_call_disconnect_finish (DEVICE1(proxy), res, &error);
	if (retval == FALSE) {
		g_debug ("Disconnect failed for %s: %s",
			 g_dbus_proxy_get_object_path (proxy),
			 error->message);
		g_simple_async_result_take_error (simple, error);
	} else {
		g_debug ("Disconnect succeeded for %s",
			 g_dbus_proxy_get_object_path (proxy));
		g_simple_async_result_set_op_res_gboolean (simple, retval);
	}

	g_simple_async_result_complete_in_idle (simple);

	g_object_unref (simple);
}

/**
 * bluetooth_client_connect_service:
 * @client: a #BluetoothClient
 * @path: the object path on which to operate
 * @connect: Whether try to connect or disconnect from services on a device
 * @cancellable: optional #GCancellable object, %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the connection is complete
 * @user_data: the data to pass to callback function
 *
 * When the connection operation is finished, @callback will be called. You can
 * then call bluetooth_client_connect_service_finish() to get the result of the
 * operation.
 **/
void
bluetooth_client_connect_service (BluetoothClient     *client,
				  const char          *path,
				  gboolean             connect,
				  GCancellable        *cancellable,
				  GAsyncReadyCallback  callback,
				  gpointer             user_data)
{
	BluetoothClientPrivate *priv = BLUETOOTH_CLIENT_GET_PRIVATE(client);
	GtkTreeIter iter;
	GSimpleAsyncResult *simple;
	GDBusProxy *device;

	g_return_if_fail (BLUETOOTH_IS_CLIENT (client));
	g_return_if_fail (path != NULL);

	if (get_iter_from_path (priv->store, &iter, path) == FALSE)
		return;

	gtk_tree_model_get (GTK_TREE_MODEL(priv->store), &iter,
			    BLUETOOTH_COLUMN_PROXY, &device,
			    -1);

	simple = g_simple_async_result_new (G_OBJECT (client),
					    callback,
					    user_data,
					    bluetooth_client_connect_service);
	g_simple_async_result_set_check_cancellable (simple, cancellable);

	if (connect) {
		device1_call_connect (DEVICE1(device),
				      cancellable,
				      (GAsyncReadyCallback) connect_callback,
				      simple);
	} else {
		device1_call_disconnect (DEVICE1(device),
					 cancellable,
					 (GAsyncReadyCallback) disconnect_callback,
					 simple);
	}

	g_object_unref (device);
}

/**
 * bluetooth_client_connect_service_finish:
 * @client: a #BluetoothClient
 * @res: a #GAsyncResult
 * @error: a #GError
 *
 * Finishes the connection operation. See bluetooth_client_connect_service().
 *
 * Returns: %TRUE if the connection operation succeeded, %FALSE otherwise.
 **/
gboolean
bluetooth_client_connect_service_finish (BluetoothClient *client,
					 GAsyncResult    *res,
					 GError         **error)
{
	GSimpleAsyncResult *simple;

	simple = (GSimpleAsyncResult *) res;

	g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == bluetooth_client_connect_service);

	if (g_simple_async_result_get_op_res_gboolean (simple))
		return TRUE;
	g_simple_async_result_propagate_error (simple, error);
	return FALSE;
}

#define BOOL_STR(x) (x ? "True" : "False")

void
bluetooth_client_dump_device (GtkTreeModel *model,
			      GtkTreeIter *iter)
{
	GDBusProxy *proxy;
	char *address, *alias, *name, *icon, **uuids;
	gboolean is_default, paired, trusted, connected, discoverable, discovering, powered, is_adapter;
	GtkTreeIter parent;
	BluetoothType type;

	gtk_tree_model_get (model, iter,
			    BLUETOOTH_COLUMN_ADDRESS, &address,
			    BLUETOOTH_COLUMN_ALIAS, &alias,
			    BLUETOOTH_COLUMN_NAME, &name,
			    BLUETOOTH_COLUMN_TYPE, &type,
			    BLUETOOTH_COLUMN_ICON, &icon,
			    BLUETOOTH_COLUMN_DEFAULT, &is_default,
			    BLUETOOTH_COLUMN_PAIRED, &paired,
			    BLUETOOTH_COLUMN_TRUSTED, &trusted,
			    BLUETOOTH_COLUMN_CONNECTED, &connected,
			    BLUETOOTH_COLUMN_DISCOVERABLE, &discoverable,
			    BLUETOOTH_COLUMN_DISCOVERING, &discovering,
			    BLUETOOTH_COLUMN_POWERED, &powered,
			    BLUETOOTH_COLUMN_UUIDS, &uuids,
			    BLUETOOTH_COLUMN_PROXY, &proxy,
			    -1);
	if (proxy) {
		char *basename;
		basename = g_path_get_basename(g_dbus_proxy_get_object_path(proxy));
		is_adapter = !g_str_has_prefix (basename, "dev_");
		g_free (basename);
	} else {
		is_adapter = !gtk_tree_model_iter_parent (model, &parent, iter);
	}

	if (is_adapter != FALSE) {
		/* Adapter */
		g_print ("Adapter: %s (%s)\n", name, address);
		if (is_default)
			g_print ("\tDefault adapter\n");
		g_print ("\tD-Bus Path: %s\n", proxy ? g_dbus_proxy_get_object_path (proxy) : "(none)");
		g_print ("\tDiscoverable: %s\n", BOOL_STR (discoverable));
		if (discovering)
			g_print ("\tDiscovery in progress\n");
		g_print ("\t%s\n", powered ? "Is powered" : "Is not powered");
	} else {
		/* Device */
		g_print ("Device: %s (%s)\n", alias, address);
		g_print ("\tD-Bus Path: %s\n", proxy ? g_dbus_proxy_get_object_path (proxy) : "(none)");
		g_print ("\tType: %s Icon: %s\n", bluetooth_type_to_string (type), icon);
		g_print ("\tPaired: %s Trusted: %s Connected: %s\n", BOOL_STR(paired), BOOL_STR(trusted), BOOL_STR(connected));
		if (uuids != NULL) {
			guint i;
			g_print ("\tUUIDs: ");
			for (i = 0; uuids[i] != NULL; i++)
				g_print ("%s ", uuids[i]);
			g_print ("\n");
		}
	}
	g_print ("\n");

	g_free (alias);
	g_free (address);
	g_free (icon);
	g_clear_object (&proxy);
	g_strfreev (uuids);
}

