/**
 * @file camera.c
 * Camera module -- this handles the camera LED-indicator for MCE
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#include <gmodule.h>

#include <string.h>			/* strncmp(), strlen() */

#include "mce.h"
#include "camera.h"

#include "mce-io.h"			/* mce_register_io_monitor_string(),
					 * mce_unregister_io_monitor()
					 */
#include "mce-conf.h"			/* mce_conf_get_bool() */
#include "datapipe.h"			/* execute_datapipe(),
					 * execute_datapipe_output_triggers()
					 */

/** Unlock the tklock if the camera is popped out? */
static gboolean popout_unlock = DEFAULT_CAMERA_POPOUT_UNLOCK;

/** Module name */
#define MODULE_NAME		"camera"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Functionality that this module depends on */
static const gchar *const depends[] = { "tklock", NULL };

/** Functionality that this module recommends */
static const gchar *const recommends[] = { "led", NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module dependencies */
	.depends = depends,
	/** Module recommends */
	.recommends = recommends,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 250
};

/** ID for the camera active state I/O monitor */
static gconstpointer camera_active_state_iomon_id = NULL;

/** ID for the camera pop-out state I/O monitor */
static gconstpointer camera_popout_state_iomon_id = NULL;

/** Camera pop-out state I/O monitor deleted callback
 *
 * @param iomon io monitor that is about to get deleted
 */
static void camera_popout_state_iomon_delete_cb(gconstpointer iomon)
{
	if( iomon == camera_popout_state_iomon_id )
		camera_popout_state_iomon_id = 0;
}

/** Camera active state I/O monitor deleted callback
 *
 * @param iomon io monitor that is about to get deleted
 */
static void camera_active_state_iomon_delete_cb(gconstpointer iomon)
{
	if( iomon == camera_active_state_iomon_id )
		camera_active_state_iomon_id = 0;
}

/**
 * I/O monitor callback for the camera active state
 *
 * @param data The new data
 * @param bytes_read Unused
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean camera_active_state_iomon_input_cb(gpointer data, gsize bytes_read)
{
	(void)bytes_read;

	if (!strncmp(data, MCE_CAMERA_ACTIVE, strlen(MCE_CAMERA_ACTIVE))) {
		execute_datapipe_output_triggers(&led_pattern_activate_pipe,
						 MCE_LED_PATTERN_CAMERA,
						 USE_INDATA);
	} else {
		execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
						 MCE_LED_PATTERN_CAMERA,
						 USE_INDATA);
	}

	return FALSE;
}

/**
 * I/O monitor callback for the camera pop-out state
 *
 * @param data The new data
 * @param bytes_read Unused
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean camera_popout_state_iomon_input_cb(gpointer data, gsize bytes_read)
{
	(void)bytes_read;

	/* Generate activity */
	(void)execute_datapipe(&device_inactive_pipe, GINT_TO_POINTER(FALSE),
			       USE_INDATA, CACHE_INDATA);

	if (popout_unlock == FALSE)
		goto EXIT;

	/* Unlock tklock if camera is popped out */
	if (!strncmp(data, MCE_CAMERA_POPPED_OUT,
		     strlen(MCE_CAMERA_POPPED_OUT))) {
		/* Request delayed unlock of touchscreen/keypad lock */
		(void)execute_datapipe(&tk_lock_pipe,
				       GINT_TO_POINTER(LOCK_OFF_DELAYED),
				       USE_INDATA, CACHE_INDATA);
	}

EXIT:
	return FALSE;
}

/**
 * Init function for the camera module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	(void)module;

	/* Get configuration options */
	popout_unlock = mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
					  MCE_CONF_CAMERA_POPOUT_UNLOCK,
					  DEFAULT_CAMERA_POPOUT_UNLOCK);

	/* Register I/O monitors */
	camera_active_state_iomon_id =
		mce_register_io_monitor_string(-1, CAMERA_ACTIVE_STATE_PATH,
					       MCE_IO_ERROR_POLICY_IGNORE,
					       TRUE,
					       camera_active_state_iomon_input_cb,
					       camera_active_state_iomon_delete_cb);

	camera_popout_state_iomon_id =
		mce_register_io_monitor_string(-1, CAMERA_POPOUT_STATE_PATH,
					       MCE_IO_ERROR_POLICY_IGNORE,
					       TRUE,
					       camera_popout_state_iomon_input_cb,
					       camera_popout_state_iomon_delete_cb);
	return NULL;
}

/**
 * Exit function for the camera module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Unregister I/O monitors */
	mce_unregister_io_monitor(camera_popout_state_iomon_id);
	mce_unregister_io_monitor(camera_active_state_iomon_id);

	return;
}
