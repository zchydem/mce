/**
 * @file mce-hal.c
 * Hardware Abstraction Layer for MCE
 * <p>
 * Copyright © 2009-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <glib.h>

#include <string.h>			/* strstr() */
#include <stdlib.h>			/* free() */
#include <unistd.h>
#include <fcntl.h>

#include "mce-hal.h"

#include "mce-lib.h"			/* strmemcmp() */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-dbus.h"			/* dbus_send(),
					 * dbus_message_iter_init(),
					 * dbus_message_iter_get_arg_type(),
					 * dbus_message_iter_get_basic(),
					 * dbus_message_iter_next(),
					 * dbus_error_init(),
					 * dbus_error_free(),
					 * DBUS_TYPE_BYTE,
					 * DBUS_TYPE_ARRAY,
					 * DBUS_TYPE_STRING,
					 * DBUS_TYPE_INVALID,
					 * DBusMessage, DBusError,
					 * dbus_bool_t,
					 */

#ifndef SYSINFOD_SERVICE
/** SYSINFOD D-Bus service */
#define SYSINFOD_SERVICE                "com.nokia.SystemInfo"
#endif /* SYSINFOD_SERVICE */

#ifndef SYSINFOD_INTERFACE
/** SYSINFOD D-Bus interface */
#define SYSINFOD_INTERFACE              "com.nokia.SystemInfo"
#endif /* SYSINFOD_INTERFACE */

#ifndef SYSINFOD_PATH
/** SYSINFOD D-Bus object path */
#define SYSINFOD_PATH                   "/com/nokia/SystemInfo"
#endif /* SYSINFOD_PATH */

#ifndef SYSINFOD_GET_CONFIG_VALUE
/** Query value of a sysinfo key */
#define SYSINFOD_GET_CONFIG_VALUE       "GetConfigValue"
#endif /* SYSINFOD_GET_CONFIG_VALUE */

/** The sysinfo key to request */
#define PRODUCT_SYSINFO_KEY		"/component/product"

/**
 * The product ID of the device
 */
static product_id_t product_id = PRODUCT_UNSET;


/** Get init process environment value
 *
 * If mce is started manually, some environment variables are not
 * inherited from systemd. This function attempts to retrieve them
 * from the context of init process itself.
 *
 * @param key name of environment value
 *
 * @return string, or NULL
 */
static char *getenv_from_init_process(const char *key)
{
	static const char path[] = "/proc/1/environ";

	char   *res  = 0;
	int     file = -1;
	char   *data = 0;
	int     size = 0x2000;

	if( (file = open(path, O_RDONLY)) == -1 ) {
		mce_log(LL_WARN, "%s: %m", path);
		goto EXIT;
	}

	if( !(data = malloc(size)) )
		goto EXIT;

	if( (size = read(file, data, size-1)) < 0 ) {
		mce_log(LL_WARN, "%s: %m", path);
		goto EXIT;
	}

	data[size] = 0;

	for( char *now = data; now < data + size;  ) {
		char *val = strchr(now, '=');

		if( !val )
			break;

		*val++ = 0;

		if( !strcmp(now, key) ) {
			res = strdup(val);
			break;
		}
		now = strchr(val, 0) + 1;
	}
EXIT:
	free(data);
	if( file != -1 ) close(file);

	mce_log(LL_NOTICE, "key=%s -> val=%s", key, res);

	return res;
}

/**
 * Retrieve a sysinfo value via D-Bus
 *
 * @param key The sysinfo key to retrieve
 * @param[out] array A newly allocated byte array with the result;
 *                   this array should to be freed when no longer used
 * @param[out] len The length of the newly allocated string
 * @return TRUE on success, FALSE on failure
 */
gboolean get_sysinfo_value(const gchar *const key, guint8 **array, gulong *len)
{
#ifdef ENABLE_SYSINFOD_QUERIES
	DBusMessage *reply;
	guint8 *tmp = NULL;
	gboolean status = FALSE;

	if ((reply = dbus_send_with_block(SYSINFOD_SERVICE, SYSINFOD_PATH,
					  SYSINFOD_INTERFACE,
					  SYSINFOD_GET_CONFIG_VALUE,
					  -1,
					  DBUS_TYPE_STRING, &key,
					  DBUS_TYPE_INVALID)) != NULL) {
		dbus_message_get_args(reply, NULL,
				      DBUS_TYPE_ARRAY,
				      DBUS_TYPE_BYTE,
				      &tmp, len,
				      DBUS_TYPE_INVALID);

		if (*len > 0) {
			if ((*array = malloc(*len)) != NULL) {
				*array = memcpy(*array, tmp, *len);
			} else {
				*len = 0;
			}
		}

		dbus_message_unref(reply);
		status = TRUE;
	}

	return status;
#else
	/* Try to provide some values from environment */
	const char *env = 0;
	const char *val = 0;
	char       *res = 0;
	gulong      cnt = 0;

	if( !strcmp(key, PRODUCT_SYSINFO_KEY) )
		env = "product_name";

	if( env ) {
		if( (val = getenv(env)) )
			res = strdup(val);
		else
			res = getenv_from_init_process(env);
	}
	if( res ) cnt = strlen(res);

	mce_log(LL_INFO, "key=%s, env=%s, val=%s, len=%d",
		key, env, res, (int)cnt);

	return *array = (guint8*)res, *len = cnt, (res != 0);
#endif /* ENABLE_SYSINFOD_QUERIES */

}

/**
 * Get product ID
 *
 * @return The product ID
 */
product_id_t get_product_id(void)
{
	guint8 *tmp = NULL;
	gulong len = 0;

	if (product_id != PRODUCT_UNSET)
		goto EXIT;

	product_id = PRODUCT_UNKNOWN;

	if( !get_sysinfo_value(PRODUCT_SYSINFO_KEY, &tmp, &len) ) {
		// nothing
	}
	else if (strmemcmp(tmp, PRODUCT_SU18_STR, len) == TRUE) {
		product_id = PRODUCT_SU18;
	} else if (strmemcmp(tmp, PRODUCT_RX34_STR, len) == TRUE) {
		product_id = PRODUCT_RX34;
	} else if (strmemcmp(tmp, PRODUCT_RX44_STR, len) == TRUE) {
		product_id = PRODUCT_RX44;
	} else if (strmemcmp(tmp, PRODUCT_RX48_STR, len) == TRUE) {
		product_id = PRODUCT_RX48;
	} else if (strmemcmp(tmp, PRODUCT_RX51_STR, len) == TRUE) {
		product_id = PRODUCT_RX51;
	} else if (strmemcmp(tmp, PRODUCT_RX71_STR, len) == TRUE) {
		product_id = PRODUCT_RX71;
	} else if (strmemcmp(tmp, PRODUCT_RM680_STR, len) == TRUE) {
		product_id = PRODUCT_RM680;
	} else if (strmemcmp(tmp, PRODUCT_RM690_STR, len) == TRUE) {
		product_id = PRODUCT_RM690;
	} else if (strmemcmp(tmp, PRODUCT_RM696_STR, len) == TRUE) {
		product_id = PRODUCT_RM696;
	} else if (strmemcmp(tmp, PRODUCT_RM716_STR, len) == TRUE) {
		product_id = PRODUCT_RM716;
	}
	free(tmp);

	if ( product_id == PRODUCT_UNKNOWN ) {
		mce_log(LL_ERR,	"Failed to get the product ID");
	}

EXIT:
	return product_id;
}
