/** @file mcetool.c
 * Tool to test and remote control the Mode Control Entity
 * <p>
 * Copyright © 2005-2011 Nokia Corporation and/or its subsidiary(-ies).
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

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <glib.h>
#include <glib-object.h>

#include <dbus/dbus.h>
#include <gconf/gconf-client.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include "../tklock.h"
#include "../event-input.h"
#include "../modules/display.h"
#include "../modules/doubletap.h"
#include "../modules/powersavemode.h"
#include "../modules/filter-brightness-als.h"
#include "../modules/proximity.h"
#include "../systemui/tklock-dbus-names.h"
#include "../systemui/dbus-names.h"

/** Whether to enable development time debugging */
#define MCETOOL_ENABLE_EXTRA_DEBUG 0

/** Name shown by --help etc. */
#define PROG_NAME "mcetool"

/** Define get config DBUS method */
#define MCE_DBUS_GET_CONFIG_REQ                 "get_config"

/** Define set config DBUS method */
#define MCE_DBUS_SET_CONFIG_REQ                 "set_config"

/** Default padding for left column of status reports */
#define PAD1 "28"

/** Padding used for radio state bits */
#define PAD2 "20"

#if MCETOOL_ENABLE_EXTRA_DEBUG
# define debugf(FMT, ARGS...) fprintf(stderr, PROG_NAME": D: "FMT, ##ARGS)
#else
# define debugf(FMT, ARGS...) do { }while(0)
#endif

# define errorf(FMT, ARGS...) fprintf(stderr, PROG_NAME": E: "FMT, ##ARGS)

/* ------------------------------------------------------------------------- *
 * GENERIC DBUS HELPERS
 * ------------------------------------------------------------------------- */

/** Cached D-Bus connection */
static DBusConnection *xdbus_con = NULL;

/** Initialize D-Bus system bus connection
 *
 * Makes a cached connection to system bus and checks if mce is present
 *
 * @return System bus connection on success, terminates on failure
 */
static DBusConnection *xdbus_init(void)
{
        if( !xdbus_con ) {
                DBusError err = DBUS_ERROR_INIT;
                DBusBusType bus_type = DBUS_BUS_SYSTEM;

                if( !(xdbus_con = dbus_bus_get(bus_type, &err)) ) {
                        errorf("Failed to open connection to message bus; %s: %s\n",
                               err.name, err.message);
                        dbus_error_free(&err);
                        exit(EXIT_FAILURE);
                }
                debugf("connected to system bus\n");

                if( !dbus_bus_name_has_owner(xdbus_con, MCE_SERVICE, &err) ) {
                        if( dbus_error_is_set(&err) ) {
                                errorf("%s: %s: %s\n", MCE_SERVICE,
                                        err.name, err.message);
                        }
                        errorf("MCE not running, terminating\n");
                        exit(EXIT_FAILURE);
                }

                debugf("mce is running\n");
        }
        return xdbus_con;
}

/** Disconnect from D-Bus system bus
 */
static void xdbus_exit(void)
{
        /* If there is an established D-Bus connection, unreference it */
        if (xdbus_con != NULL) {
                dbus_connection_unref(xdbus_con);
                xdbus_con = NULL;
                debugf("disconnected from system bus\n");
        }
}

/** Make sure the cached dbus connection is not used directly */
#define xdbus_con something_that_will_generate_error

/** Generic synchronous D-Bus method call wrapper function
 *
 * If reply pointer is NULL, the method call is sent without
 * waiting for method return message.
 *
 * @param service   [IN]  D-Bus service name
 * @param path      [IN]  D-Bus object path
 * @param interface [IN]  D-Bus interface name
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store method_return message, or NULL
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param va        [IN]  va_list of D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent (and optionally reply received),
 *         or FALSE in case of errors or if error reply is received
 */
static gboolean xdbus_call_va(const gchar *const service, const gchar *const path,
                              const gchar *const interface, const gchar *const name,
                              DBusMessage **reply, int arg_type, va_list va)
{
        debugf("%s(%s,%s,%s,%s)\n", __FUNCTION__, service, path, interface, name);
        gboolean        ack = FALSE;
        DBusMessage    *msg = 0;
        DBusMessage    *rsp = 0;
        DBusConnection *bus = xdbus_init();
        DBusError       err = DBUS_ERROR_INIT;

        msg = dbus_message_new_method_call(service, path, interface, name);

        if( !dbus_message_append_args_valist(msg, arg_type, va) ) {
                errorf("%s.%s: failed to construct message\n", interface, name);
                goto EXIT;
        }

        dbus_message_set_auto_start(msg, FALSE);

        if( reply ) {
                rsp = dbus_connection_send_with_reply_and_block(bus, msg, -1, &err);
                if( rsp == 0 ) {
                        errorf("%s.%s send message: %s: %s\n",
                               interface, name, err.name, err.message);
                        goto EXIT;
                }

                if( dbus_set_error_from_message(&err, rsp) ) {
                        errorf("%s.%s call failed: %s: %s\n",
                               interface, name, err.name, err.message);
                        dbus_message_unref(rsp), rsp = 0;
                        goto EXIT;
                }

        }
        else {
                dbus_message_set_no_reply(msg, TRUE);

                if( !dbus_connection_send(bus, msg, NULL) ) {
                        errorf("Failed to send method call\n");
                        goto EXIT;
                }
                dbus_connection_flush(bus);
        }

        ack = TRUE;

EXIT:
        if( reply ) *reply = rsp, rsp = 0;

        dbus_error_free(&err);

        if( rsp ) dbus_message_unref(rsp);
        if( msg ) dbus_message_unref(msg);

        return ack;
}

/* ------------------------------------------------------------------------- *
 * MCE DBUS IPC HELPERS
 * ------------------------------------------------------------------------- */

/** Wrapper for making synchronous D-Bus method calls to MCE
 *
 * If reply pointer is NULL, the method call is sent without
 * waiting for method return message.
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store method_return message, or NULL
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param va        [IN]  va_list of D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent (and optionally reply received),
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_va(const gchar *const name, DBusMessage **reply,
                            int arg_type, va_list va)
{
        return xdbus_call_va(MCE_SERVICE,
                             MCE_REQUEST_PATH,
                             MCE_REQUEST_IF,
                             name, reply,
                             arg_type, va);
}

/** Wrapper for making MCE D-Bus method calls without waiting for reply
 *
 * @param name      [IN]  D-Bus method call name
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent, or FALSE in case of errors
 */
static gboolean xmce_ipc_no_reply(const gchar *const name,
                                  int arg_type, ...)
{
        va_list va;
        va_start(va, arg_type);
        gboolean ack = xmce_ipc_va(name, 0, arg_type, va);
        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store method_return message
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_message_reply(const gchar *const name, DBusMessage **reply,
                                       int arg_type, ...)
{
        va_list va;
        va_start(va, arg_type);
        gboolean ack = xmce_ipc_va(name, reply, arg_type, va);
        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls that return STRING
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store string from method return
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received and parsed,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_string_reply(const gchar *const name,
                                      char **result,
                                      int arg_type, ...)
{
        gboolean     ack = FALSE;
        DBusMessage *rsp = 0;
        DBusError    err = DBUS_ERROR_INIT;
        const char  *dta = 0;

        va_list va;
        va_start(va, arg_type);

        if( !xmce_ipc_va(name, &rsp, arg_type, va) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_STRING, &dta,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        *result = strdup(dta);

        ack = TRUE;
EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", name, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls that return UINT32
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store uint from method return
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received and parsed,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_uint_reply(const gchar *const name,
                                    guint *result,
                                    int arg_type, ...)
{
        gboolean      ack = FALSE;
        DBusMessage  *rsp = 0;
        DBusError     err = DBUS_ERROR_INIT;
        dbus_uint32_t dta = 0;

        va_list va;
        va_start(va, arg_type);

        if( !xmce_ipc_va(name, &rsp, arg_type, va) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_UINT32, &dta,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        *result = dta;

        ack = TRUE;
EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", name, err.name, err.message);
                dbus_error_free(&err);
        }
        if( rsp ) dbus_message_unref(rsp);

        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls that return BOOLEAN
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store bool from method return
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received and parsed,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_bool_reply(const gchar *const name,
                                    gboolean *result,
                                    int arg_type, ...)
{
        gboolean      ack = FALSE;
        DBusMessage  *rsp = 0;
        DBusError     err = DBUS_ERROR_INIT;
        dbus_bool_t   dta = 0;

        va_list va;
        va_start(va, arg_type);

        if( !xmce_ipc_va(name, &rsp, arg_type, va) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_BOOLEAN, &dta,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        *result = dta;

        ack = TRUE;
EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", name, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        va_end(va);
        return ack;
}

/* ------------------------------------------------------------------------- *
 * MCE IPC HELPERS
 * ------------------------------------------------------------------------- */

/** Helper for getting dbus data type as string
 *
 * @param type dbus data type (DBUS_TYPE_BOOLEAN etc)
 *
 * @return type name with out common prefix (BOOLEAN etc)
 */
static const char *dbushelper_get_type_name(int type)
{
        const char *res = "UNKNOWN";
        switch( type ) {
        case DBUS_TYPE_INVALID:     res = "INVALID";     break;
        case DBUS_TYPE_BYTE:        res = "BYTE";        break;
        case DBUS_TYPE_BOOLEAN:     res = "BOOLEAN";     break;
        case DBUS_TYPE_INT16:       res = "INT16";       break;
        case DBUS_TYPE_UINT16:      res = "UINT16";      break;
        case DBUS_TYPE_INT32:       res = "INT32";       break;
        case DBUS_TYPE_UINT32:      res = "UINT32";      break;
        case DBUS_TYPE_INT64:       res = "INT64";       break;
        case DBUS_TYPE_UINT64:      res = "UINT64";      break;
        case DBUS_TYPE_DOUBLE:      res = "DOUBLE";      break;
        case DBUS_TYPE_STRING:      res = "STRING";      break;
        case DBUS_TYPE_OBJECT_PATH: res = "OBJECT_PATH"; break;
        case DBUS_TYPE_SIGNATURE:   res = "SIGNATURE";   break;
        case DBUS_TYPE_UNIX_FD:     res = "UNIX_FD";     break;
        case DBUS_TYPE_ARRAY:       res = "ARRAY";       break;
        case DBUS_TYPE_VARIANT:     res = "VARIANT";     break;
        case DBUS_TYPE_STRUCT:      res = "STRUCT";      break;
        case DBUS_TYPE_DICT_ENTRY:  res = "DICT_ENTRY";  break;
        default: break;
        }
        return res;
}

/** Helper for testing that iterator points to expected data type
 *
 * @param iter D-Bus message iterator
 * @param want_type D-Bus data type
 *
 * @return TRUE if iterator points to give data type, FALSE otherwise
 */
static gboolean dbushelper_require_type(DBusMessageIter *iter,
                                        int want_type)
{
        int have_type = dbus_message_iter_get_arg_type(iter);

        if( want_type != have_type ) {
                errorf("expected DBUS_TYPE_%s, got %s\n",
                       dbushelper_get_type_name(want_type),
                       dbushelper_get_type_name(have_type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for testing that iterator points to array of expected data type
 *
 * @param iter D-Bus message iterator
 * @param want_type D-Bus data type
 *
 * @return TRUE if iterator points to give data type, FALSE otherwise
 */
static gboolean dbushelper_require_array_type(DBusMessageIter *iter,
                                              int want_type)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_ARRAY) )
                return FALSE;

        int have_type = dbus_message_iter_get_element_type(iter);

        if( want_type != have_type ) {
                errorf("expected array of DBUS_TYPE_%s, got %s\n",
                       dbushelper_get_type_name(want_type),
                       dbushelper_get_type_name(have_type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for making blocking D-Bus method calls
 *
 * @param req D-Bus method call message to send
 *
 * @return D-Bus method reply message, or NULL on failure
 */
static DBusMessage *dbushelper_call_method(DBusMessage *req)
{
        DBusMessage *rsp = 0;
        DBusError    err = DBUS_ERROR_INIT;

        rsp = dbus_connection_send_with_reply_and_block(xdbus_init(),
                                                        req, -1, &err);

        if( !rsp ) {
                errorf("%s.%s: %s: %s\n",
                       dbus_message_get_interface(req),
                       dbus_message_get_member(req),
                       err.name, err.message);
                goto EXIT;
        }

EXIT:
        dbus_error_free(&err);

        return rsp;
}

/** Helper for parsing int value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_int(DBusMessageIter *iter, gint *value)
{
        dbus_int32_t data = 0;

        if( !dbushelper_require_type(iter, DBUS_TYPE_INT32) )
                return FALSE;

        dbus_message_iter_get_basic(iter, &data);
        dbus_message_iter_next(iter);

        return *value = data, TRUE;
}

/** Helper for parsing boolean value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_boolean(DBusMessageIter *iter, gboolean *value)
{
        dbus_bool_t data = 0;

        if( !dbushelper_require_type(iter, DBUS_TYPE_BOOLEAN) )
                return FALSE;

        dbus_message_iter_get_basic(iter, &data);
        dbus_message_iter_next(iter);

        return *value = data, TRUE;
}

/** Helper for entering variant container from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param sub  D-Bus message iterator for variant (not modified on failure)
 *
 * @return TRUE if container could be entered, FALSE on failure
 */
static gboolean dbushelper_read_variant(DBusMessageIter *iter, DBusMessageIter *sub)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_VARIANT) )
                return FALSE;

        dbus_message_iter_recurse(iter, sub);
        dbus_message_iter_next(iter);

        return TRUE;
}

/** Helper for entering array container from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param sub  D-Bus message iterator for array (not modified on failure)
 *
 * @return TRUE if container could be entered, FALSE on failure
 */
static gboolean dbushelper_read_array(DBusMessageIter *iter, DBusMessageIter *sub)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_ARRAY) )
                return FALSE;

        dbus_message_iter_recurse(iter, sub);
        dbus_message_iter_next(iter);

        return TRUE;
}

/** Helper for parsing int array from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the array pointer (not modified on failure)
 * @param count Where to store the array length (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_int_array(DBusMessageIter *iter,
                                          gint **value, gint *count)
{
        debugf("@%s()\n", __FUNCTION__);

        dbus_int32_t *arr_dbus = 0;
        gint         *arr_glib = 0;
        int           cnt = 0;
        DBusMessageIter tmp;

        if( !dbushelper_require_array_type(iter, DBUS_TYPE_INT32) )
                return FALSE;

        if( !dbushelper_read_array(iter, &tmp) )
                return FALSE;

        dbus_message_iter_get_fixed_array(&tmp, &arr_dbus, &cnt);
        dbus_message_iter_next(iter);

        arr_glib = g_malloc0(cnt * sizeof *arr_glib);
        for( gint i = 0; i < cnt; ++i )
                arr_glib[i] = arr_dbus[i];

        return *value = arr_glib, *count = cnt, TRUE;
}

/** Helper for initializing D-Bus message read iterator
 *
 * @param rsp  D-Bus message
 * @param iter D-Bus iterator for parsing message (not modified on failure)
 *
 * @return TRUE if read iterator could be initialized, FALSE on failure
 */
static gboolean dbushelper_init_read_iterator(DBusMessage *rsp,
                                              DBusMessageIter *iter)
{
        if( !dbus_message_iter_init(rsp, iter) ) {
                errorf("failed to initialize dbus read iterator\n");
                return FALSE;
        }
        return TRUE;
}

/** Helper for initializing D-Bus message write iterator
 *
 * @param rsp  D-Bus message
 * @param iter D-Bus iterator for appending message (not modified on failure)
 *
 * @return TRUE if append iterator could be initialized, FALSE on failure
 */
static gboolean dbushelper_init_write_iterator(DBusMessage *req,
                                               DBusMessageIter *iter)
{
        dbus_message_iter_init_append(req, iter);
        return TRUE;
}

/** Helper for adding int value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_int(DBusMessageIter *iter, gint value)
{
        dbus_int32_t data = value;
        int          type = DBUS_TYPE_INT32;

        if( !dbus_message_iter_append_basic(iter, type, &data) ) {
                errorf("failed to add %s data\n",
                       dbushelper_get_type_name(type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for adding int value array to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_int_array(DBusMessageIter *iter,
                                           const gint *value, gint count)
{
        gboolean      res  = FALSE;
        int           type = DBUS_TYPE_INT32;
        dbus_int32_t *data = g_malloc0(count * sizeof *data);

        for( gint i = 0; i < count; ++i )
                data[i] = value[i];

        if( !dbus_message_iter_append_fixed_array(iter, type, &data, count) ) {
                errorf("failed to add array of %s data\n",
                       dbushelper_get_type_name(type));
                goto cleanup;
        }

        res = TRUE;

cleanup:
        g_free(data);

        return res;
}

/** Helper for adding boolean value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_boolean(DBusMessageIter *iter, gboolean value)
{
        dbus_bool_t data = value;
        int         type = DBUS_TYPE_BOOLEAN;

        if( !dbus_message_iter_append_basic(iter, type, &data) ) {
                errorf("failed to add %s data\n",
                       dbushelper_get_type_name(type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for adding object path value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_path(DBusMessageIter *iter, const gchar *value)
{
        const char *data = value;
        int         type = DBUS_TYPE_OBJECT_PATH;

        if( !dbus_message_iter_append_basic(iter, type, &data) ) {
                errorf("failed to add %s data\n",
                       dbushelper_get_type_name(type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for opening a variant container
 *
 * @param stack pointer to D-Bus message iterator pointer (not
 modified on failure)
 *
 * @param signature signature string of the data that will be added to the
 *                  variant container
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_push_variant(DBusMessageIter **stack,
                                        const char *signature)
{
        DBusMessageIter *iter = *stack;
        DBusMessageIter *sub  = iter + 1;

        if( !dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
                                              signature, sub) ) {
                errorf("failed to initialize variant write iterator\n");
                return FALSE;
        }

        *stack = sub;
        return TRUE;
}

/** Helper for opening a array container
 *
 * @param stack pointer to D-Bus message iterator pointer (not
 modified on failure)
 *
 * @param signature signature string of the data that will be added to the
 *                  variant container
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_push_array(DBusMessageIter **stack,
                                      const char *signature)
{
        DBusMessageIter *iter = *stack;
        DBusMessageIter *sub  = iter + 1;

        if( !dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                                              signature, sub) ) {
                errorf("failed to initialize array write iterator\n");
                return FALSE;
        }

        *stack = sub;
        return TRUE;
}

/** Helper for closing a container
 *
 * @param stack pointer to D-Bus message iterator pointer
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_pop_container(DBusMessageIter **stack)
{
        DBusMessageIter *sub  = *stack;
        DBusMessageIter *iter = sub - 1;

        gboolean res = dbus_message_iter_close_container(iter, sub);

        *stack = iter;
        return res;
}

/** Helper for abandoning message iterator stack
 *
 * @param stack Start of iterator stack
 * @param iter  Current iterator within the stack
 */
static void dbushelper_abandon_stack(DBusMessageIter *stack,
                                     DBusMessageIter *iter)
{
        while( iter-- > stack )
                dbus_message_iter_abandon_container(iter, iter+1);
}

/* ------------------------------------------------------------------------- *
 * MCE CONFIG IPC HELPERS
 * ------------------------------------------------------------------------- */

/** Helper for making MCE D-Bus method calls
 *
 * @param method name of the method in mce request interface
 * @param arg_type as with dbus_message_append_args()
 * @param ... must be terminated with DBUS_TYPE_INVALID
 */
static DBusMessage *mcetool_config_request(const gchar *const method)
{
        DBusMessage *req = 0;

        req = dbus_message_new_method_call(MCE_SERVICE,
                                           MCE_REQUEST_PATH,
                                           MCE_REQUEST_IF,
                                           method);
        if( !req ) {
                errorf("%s.%s: can't allocate method call\n",
                       MCE_REQUEST_IF, method);
                goto EXIT;
        }

        dbus_message_set_auto_start(req, FALSE);

EXIT:
        return req;
}

/** Return a boolean from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_get_bool(const gchar *const key, gboolean *value)
{
        debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter body, variant;

        if( !(req = mcetool_config_request(MCE_DBUS_GET_CONFIG_REQ)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, &body) )
                goto EXIT;
        if( !dbushelper_write_path(&body, key) )
                goto EXIT;

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;
        if( !dbushelper_read_variant(&body, &variant) )
                goto EXIT;

        res = dbushelper_read_boolean(&variant, value);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Return an integer from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_get_int(const gchar *const key, gint *value)
{
        debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter body, variant;

        if( !(req = mcetool_config_request(MCE_DBUS_GET_CONFIG_REQ)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, &body) )
                goto EXIT;
        if( !dbushelper_write_path(&body, key) )
                goto EXIT;

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;
        if( !dbushelper_read_variant(&body, &variant) )
                goto EXIT;

        res = dbushelper_read_int(&variant, value);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Return an integer array from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param values Will contain the array of values on return
 * @param count  Will contain the array size on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_get_int_array(const gchar *const key, gint **values, gint *count)
{
        debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter body, variant;

        if( !(req = mcetool_config_request(MCE_DBUS_GET_CONFIG_REQ)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, &body) )
                goto EXIT;
        if( !dbushelper_write_path(&body, key) )
                goto EXIT;

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;

        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;
        if( !dbushelper_read_variant(&body, &variant) )
                goto EXIT;

        res = dbushelper_read_int_array(&variant, values, count);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Set a boolean GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_set_bool(const gchar *const key,
                                       const gboolean value)
{
        debugf("@%s(%s, %d)\n", __FUNCTION__, key, value);

        static const char sig[] = DBUS_TYPE_BOOLEAN_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter stack[2];
        DBusMessageIter *wpos = stack;
        DBusMessageIter *rpos = stack;

        if( !(req = mcetool_config_request(MCE_DBUS_SET_CONFIG_REQ)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, wpos) )
                goto EXIT;
        if( !dbushelper_write_path(wpos, key) )
                goto EXIT;
        if( !dbushelper_push_variant(&wpos, sig) )
                goto EXIT;
        if( !dbushelper_write_boolean(wpos, value) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( wpos != stack )
                abort();

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, rpos) )
                goto EXIT;
        if( !dbushelper_read_boolean(rpos, &res) )
                res = FALSE;

EXIT:
        // make sure write iterator stack is collapsed
        dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Set an integer GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_set_int(const gchar *const key, const gint value)
{
        debugf("@%s(%s, %d)\n", __FUNCTION__, key, value);

        static const char sig[] = DBUS_TYPE_INT32_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter stack[2];
        DBusMessageIter *wpos = stack;
        DBusMessageIter *rpos = stack;

        // construct request
        if( !(req = mcetool_config_request(MCE_DBUS_SET_CONFIG_REQ)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, wpos) )
                goto EXIT;
        if( !dbushelper_write_path(wpos, key) )
                goto EXIT;
        if( !dbushelper_push_variant(&wpos, sig) )
                goto EXIT;
        if( !dbushelper_write_int(wpos, value) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( wpos != stack )
                abort();

        // get reply and process it
        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, rpos) )
                goto EXIT;
        if( !dbushelper_read_boolean(rpos, &res) )
                res = FALSE;

EXIT:
        // make sure write iterator stack is collapsed
        dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Set an integer array GConf key to the specified values
 *
 * @param key The GConf key to set the value of
 * @param values The array of values to set the key to
 * @param count  The number of values in the array
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_set_int_array(const gchar *const key,
                                            const gint *values,
                                            gint count)
{
        debugf("@%s(%s, num x %d)\n", __FUNCTION__, key, count);

        static const char vsig[] = DBUS_TYPE_ARRAY_AS_STRING
                DBUS_TYPE_INT32_AS_STRING;
        static const char asig[] = DBUS_TYPE_INT32_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter stack[3];
        DBusMessageIter *wpos = stack;
        DBusMessageIter *rpos = stack;

        // construct request
        if( !(req = mcetool_config_request(MCE_DBUS_SET_CONFIG_REQ)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, wpos) )
                goto EXIT;
        if( !dbushelper_write_path(wpos, key) )
                goto EXIT;
        if( !dbushelper_push_variant(&wpos, vsig) )
                goto EXIT;
        if( !dbushelper_push_array(&wpos, asig) )
                goto EXIT;
        if( !dbushelper_write_int_array(wpos, values, count) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( wpos != stack )
                abort();

        // get reply and process it
        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, rpos) )
                goto EXIT;
        if( !dbushelper_read_boolean(rpos, &res) )
                res = FALSE;

EXIT:
        // make sure write iterator stack is collapsed
        dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/* ------------------------------------------------------------------------- *
 * SYMBOL LOOKUP TABLES
 * ------------------------------------------------------------------------- */

/** Simple string key to integer value symbol */
typedef struct
{
        /** Name of the symbol, or NULL to mark end of symbol table */
        const char *key;

        /** Value of the symbol */
        int         val;
} symbol_t;

/** Lookup symbol by name and return value
 *
 * @param stab array of symbol_t objects
 * @param key name of the symbol to find
 *
 * @return Value matching the name. Or if not found, the
 *         value of the end-of-table marker symbol */
static int lookup(const symbol_t *stab, const char *key)
{
        for( ; ; ++stab ) {
                if( !stab->key || !strcmp(stab->key, key) )
                        return stab->val;
        }
}

/** Lookup symbol by value and return name
 *
 * @param stab array of symbol_t objects
 * @param val value of the symbol to find
 *
 * @return name of the first matching value, or NULL
 */
static const char *rlookup(const symbol_t *stab, int val)
{
        for( ; ; ++stab ) {
                if( !stab->key || stab->val == val )
                        return stab->key;
        }
}

/** Lookup table for autosuspend policies
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t suspendpol_values[] = {
        { "disabled",  0 },
        { "enabled",   1 },
        { "early",     2 },
        { NULL, -1 }
};

/** Lookup table for cpu scaling governor overrides
 */
static const symbol_t governor_values[] = {
        { "automatic",    GOVERNOR_UNSET       },
        { "performance",  GOVERNOR_DEFAULT     },
        { "interactive",  GOVERNOR_INTERACTIVE },
        { NULL, -1 }
};

/** Lookup table for never blank options
 */
static const symbol_t never_blank_values[] = {
        { "enabled",   1 },
        { "disabled",  0 },
        { NULL, -1 }
};

/** Lookup table for fake doubletap policies
 */
static const symbol_t fake_doubletap_values[] = {
        { "disabled",  0 },
        { "enabled",   1 },
        { NULL, -1 }
};

/** Lookup table for tklock autoblank policy values
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t tklockblank_values[] = {
        { "disabled",  1 },
        { "enabled",   0 },
        { NULL, -1 }
};

/** Lookup table for power key event values
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t powerkeyevent_lut[] =
{
        { "short",  0 },
        { "long",   1 },
        { "double", 2 },
        { 0, -1 }
};

/** Convert power key event name to number passable to mce
 *
 * @param args string from user
 *
 * @return number passable to MCE, or terminate on error
 */
static int xmce_parse_powerkeyevent(const char *args)
{
        int res = lookup(powerkeyevent_lut, args);
        if( res < 0 ) {
                errorf("%s: not a valid power key event\n", args);
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Lookup table for blanking inhibit modes
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t inhibitmode_lut[] =
{
        { "disabled",              0 },
        { "stay-on-with-charger",  1 },
        { "stay-dim-with-charger", 2 },
        { "stay-on",               3 },
        { "stay-dim",              4 },
        { 0, -1 }

};

/** Convert blanking inhibit mode name to number passable to MCE
 *
 * @param args string from user
 *
 * @return number passable to MCE, or terminate on error
 */
static int parse_inhibitmode(const char *args)
{
        int res = lookup(inhibitmode_lut, args);
        if( res < 0 ) {
                errorf("%s: not a valid inhibit mode value\n", args);
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Convert blanking inhibit mode to human readable string
 *
 * @param value blanking inhibit mode from mce
 *
 * @return mode name, or NULL in case of errors
 */
static const char *repr_inhibitmode(int value)
{
        return rlookup(inhibitmode_lut, value);
}

/** Lookuptable for mce radio state bits */
static const symbol_t radio_states_lut[] =
{
        { "master",    MCE_RADIO_STATE_MASTER },
        { "cellular",  MCE_RADIO_STATE_CELLULAR },
        { "wlan",      MCE_RADIO_STATE_WLAN },
        { "bluetooth", MCE_RADIO_STATE_BLUETOOTH },
        { "nfc",       MCE_RADIO_STATE_NFC },
        { "fmtx",      MCE_RADIO_STATE_FMTX },
        { 0,           0 }
};

/** Convert comma separated list of radio state names into bitmask
 *
 * @param args radio state list from user
 *
 * @return bitmask passable to mce, or terminate on errors
 */
static unsigned xmce_parse_radio_states(const char *args)
{
        int   res = 0;
        char *tmp = strdup(args);
        int   bit;
        char *end;

        for( char *pos = tmp; pos; pos = end )
        {
                if( (end = strchr(pos, ',')) )
                        *end++ = 0;

                if( !(bit = lookup(radio_states_lut, pos)) ) {
                        errorf("%s: not a valid radio state\n", pos);
                        exit(EXIT_FAILURE);
                }

                res |= bit;
        }
        free(tmp);
        return (unsigned)res;
}

/** Lookuptable for enabled/disabled truth values */
static const symbol_t enabled_lut[] =
{
        { "enabled",   TRUE  },
        { "disabled",  FALSE },
        { 0,           -1    }
};

/** Convert enable/disable string to boolean
 *
 * @param args string from user
 *
 * @return boolean passable to mce, or terminate on errors
 */
static gboolean xmce_parse_enabled(const char *args)
{
        int res = lookup(enabled_lut, args);
        if( res < 0 ) {
                errorf("%s: not a valid enable value\n", args);
                exit(EXIT_FAILURE);
        }
        return res != 0;
}

/** Convert string to integer
 *
 * @param args string from user
 *
 * @return integer number, or terminate on errors
 */
static int xmce_parse_integer(const char *args)
{
        char *end = 0;
        int   res = strtol(args, &end, 0);
        if( end <= args ) {
                errorf("%s: not a valid integer value\n", args);
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Convert a comma separated string in to gint array
 *
 * @param text string to split
 * @param len where to store number of elements in array
 *
 * @return array of gint type numbers
 */
static gint *parse_gint_array(const char *text, gint *len)
{
        gint   used = 0;
        gint   size = 0;
        gint  *data = 0;
        gchar *temp = 0;

        gchar *now, *zen;
        gint val;

        if( !text )
                goto EXIT;

        temp = g_strdup(text);
        size = 16;
        data = g_malloc(size * sizeof *data);

        for( now = zen = temp; *now; now = zen ) {
                val = strtol(now, &zen, 0);

                if( now == zen )
                        break;

                if( used == size )
                        data = g_realloc(data, (size *= 2) * sizeof *data);

                data[used++] = val;

                if( *zen == ',' )
                        ++zen;
        }

        size = used ? used : 1;
        data = g_realloc(data, size * sizeof *data);

EXIT:
        g_free(temp);

        return *len = used, data;
}

/** Convert string to struct timespec
 *
 * @param ts   [OUT] where to store time value
 * @param args [IN]  string from user
 *
 * @return TRUE if args was valid time value, or FALSE if not
 */
static gboolean mcetool_parse_timspec(struct timespec *ts, const char *args)
{
        gboolean ack = FALSE;
        double   tmp = 0;

        if( args && (tmp = strtod(args, 0)) > 0 ) {
                double s  = 0;
                double ns = modf(tmp, &s) * 1e9;
                ts->tv_sec  = (time_t)s;
                ts->tv_nsec = (long)ns;
                ack = TRUE;
        }

        return ack;
}

/* ------------------------------------------------------------------------- *
 * leds
 * ------------------------------------------------------------------------- */

/** Enable/Disable the LED
 *
 * @param enable TRUE to enable the LED, FALSE to disable the LED
 */
static void set_led_state(const gboolean enable)
{
        debugf("%s(%s)\n", __FUNCTION__, enable ? "enable" : "disable");
        xmce_ipc_no_reply(enable ? MCE_ENABLE_LED : MCE_DISABLE_LED,
                          DBUS_TYPE_INVALID);
}

/** Trigger a powerkey event
 *
 * @param type The type of event to trigger; valid types:
 *             "short", "double", "long"
 */
static void xmce_powerkey_event(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = xmce_parse_powerkeyevent(args);
        if( val < 0 ) {
                errorf("%s: invalid power key event\n", args);
                exit(EXIT_FAILURE);
        }
        /* com.nokia.mce.request.req_trigger_powerkey_event */
        dbus_uint32_t data = val;
        xmce_ipc_no_reply(MCE_TRIGGER_POWERKEY_EVENT_REQ,
                          DBUS_TYPE_UINT32, &data,
                          DBUS_TYPE_INVALID);
}

/** Activate/Deactivate a LED pattern
 *
 * @param pattern The name of the pattern to activate/deactivate
 * @param activate TRUE to activate pattern, FALSE to deactivate pattern
 */
static void set_led_pattern_state(const gchar *const pattern,
                                      const gboolean activate)
{
        debugf("%s(%s, %s)\n", __FUNCTION__, pattern, activate ? "enable" : "disable");

        xmce_ipc_no_reply(activate ? MCE_ACTIVATE_LED_PATTERN : MCE_DEACTIVATE_LED_PATTERN,
                          DBUS_TYPE_STRING, &pattern,
                          DBUS_TYPE_INVALID);
}

/* ------------------------------------------------------------------------- *
 * color profile
 * ------------------------------------------------------------------------- */

/** Get and print available color profile ids
 */
static void xmce_get_color_profile_ids(void)
{
        DBusMessage *rsp = NULL;
        DBusError    err = DBUS_ERROR_INIT;
        char       **arr = 0;
        int          len = 0;

        if( !xmce_ipc_message_reply(MCE_COLOR_PROFILE_IDS_GET, &rsp, DBUS_TYPE_INVALID) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &arr, &len,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        printf("Available color profiles ids are: \n");
        for( int i = 0; i < len; ++i )
                printf("\t%s\n", arr[i]);

EXIT:

        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", MCE_COLOR_PROFILE_IDS_GET,
                        err.name, err.message);
                dbus_error_free(&err);
        }

        if( arr ) dbus_free_string_array(arr);
        if( rsp ) dbus_message_unref(rsp);
}

/** Set color profile id
 *
 * Valid ids are printed by xmce_get_color_profile_ids(),
 * or --get-color-profile-ids option
 *
 * @param id The color profile id;
 */
static void xmce_set_color_profile(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        xmce_ipc_no_reply(MCE_COLOR_PROFILE_CHANGE_REQ,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
}

/** Get current color profile from mce and print it out
 */
static void xmce_get_color_profile(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_COLOR_PROFILE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Color profile:", str ?: "unknown");
        free(str);
}

/* ------------------------------------------------------------------------- *
 * radio states
 * ------------------------------------------------------------------------- */

/** Enable radios
 *
 * @param args string of comma separated radio state names
 */
static void xmce_enable_radio(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        dbus_uint32_t mask = xmce_parse_radio_states(args);
        dbus_uint32_t data = mask;

        xmce_ipc_no_reply(MCE_RADIO_STATES_CHANGE_REQ,
                   DBUS_TYPE_UINT32, &data,
                   DBUS_TYPE_UINT32, &mask,
                   DBUS_TYPE_INVALID);
}

/** Disable radios
 *
 * @param args string of comma separated radio state names
 */
static void xmce_disable_radio(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        dbus_uint32_t mask = xmce_parse_radio_states(args);
        dbus_uint32_t data = 0;

        xmce_ipc_no_reply(MCE_RADIO_STATES_CHANGE_REQ,
                   DBUS_TYPE_UINT32, &data,
                   DBUS_TYPE_UINT32, &mask,
                   DBUS_TYPE_INVALID);
}

/** Get current radio state from mce and print it out
 */
static void xmce_get_radio_states(void)
{
        guint mask = 0;

        if( !xmce_ipc_uint_reply(MCE_RADIO_STATES_GET, &mask, DBUS_TYPE_INVALID) ) {
                printf(" %-40s %s\n", "Radio states:", "unknown");
                return;
        }

        printf("Radio states:\n");

        printf("\t%-"PAD2"s %s\n", "Master:",
                mask & MCE_RADIO_STATE_MASTER ? "enabled (Online)" : "disabled (Offline)");

        printf("\t%-"PAD2"s %s\n",  "Cellular:",
                mask & MCE_RADIO_STATE_CELLULAR ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "WLAN:",
                mask & MCE_RADIO_STATE_WLAN ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "Bluetooth:",
                mask & MCE_RADIO_STATE_BLUETOOTH ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "NFC:",
                mask & MCE_RADIO_STATE_NFC ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "FM transmitter:",
                mask & MCE_RADIO_STATE_FMTX ? "enabled" : "disabled");
}

/* ------------------------------------------------------------------------- *
 * call state
 * ------------------------------------------------------------------------- */

/** Set call state
 *
 * Note: Faked call states get cancelled when mcetool exits. The
 *       --block option can be used keep mcetool connected to
 *       system bus.
 *
 * @param args string with callstate and calltype separated with ':'
 */
static void xmce_set_call_state(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);

        char *callstate = strdup(args);
        char *calltype  = strchr(callstate, ':');

        if( !calltype ) {
                errorf("%s: invalid call state value\n", args);
                exit(EXIT_FAILURE);
        }

        *calltype++ = 0;

        xmce_ipc_no_reply(MCE_CALL_STATE_CHANGE_REQ,
                          DBUS_TYPE_STRING, &callstate,
                          DBUS_TYPE_STRING, &calltype,
                          DBUS_TYPE_INVALID);

        free(callstate);
}

/** Get current call state from mce and print it out
 */
static void xmce_get_call_state(void)
{
        const char  *callstate = 0;
        const char  *calltype  = 0;
        DBusMessage *rsp = NULL;
        DBusError    err = DBUS_ERROR_INIT;

        if( !xmce_ipc_message_reply(MCE_CALL_STATE_GET, &rsp, DBUS_TYPE_INVALID) )
                goto EXIT;

        dbus_message_get_args(rsp, &err,
                              DBUS_TYPE_STRING, &callstate,
                              DBUS_TYPE_STRING, &calltype,
                              DBUS_TYPE_INVALID);

EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", MCE_CALL_STATE_GET,
                       err.name, err.message);
                dbus_error_free(&err);
        }

        printf("%-"PAD1"s %s (%s)\n", "Call state (type):",
               callstate ?: "unknown",
               calltype ?:  "unknown");

        if( rsp ) dbus_message_unref(rsp);
}

/* ------------------------------------------------------------------------- *
 * display state
 * ------------------------------------------------------------------------- */

/** Set display state
 *
 * @param args display state; "on", "dim" or "off"
 */
static void xmce_set_display_state(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        if( !strcmp(args, "on") )
                xmce_ipc_no_reply(MCE_DISPLAY_ON_REQ, DBUS_TYPE_INVALID);
        else if( !strcmp(args, "dim") )
                xmce_ipc_no_reply(MCE_DISPLAY_DIM_REQ, DBUS_TYPE_INVALID);
        else if( !strcmp(args, "off") )
                xmce_ipc_no_reply(MCE_DISPLAY_OFF_REQ, DBUS_TYPE_INVALID);
        else
                errorf("%s: invalid display state\n", args);
}

/** Get current display state from mce and print it out
 */
static void xmce_get_display_state(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_DISPLAY_STATUS_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Display state:", str ?: "unknown");
        free(str);
}

/* ------------------------------------------------------------------------- *
 * display keepalive
 * ------------------------------------------------------------------------- */

/** Request display keepalive
 */
static void xmce_prevent_display_blanking(void)
{
        debugf("%s()\n", __FUNCTION__);
        xmce_ipc_no_reply(MCE_PREVENT_BLANK_REQ, DBUS_TYPE_INVALID);
}

/** Cancel display keepalive
 */
static void xmce_allow_display_blanking(void)
{
        debugf("%s()\n", __FUNCTION__);
        xmce_ipc_no_reply(MCE_CANCEL_PREVENT_BLANK_REQ, DBUS_TYPE_INVALID);
}

/* ------------------------------------------------------------------------- *
 * display brightness
 * ------------------------------------------------------------------------- */

/** Set display brightness
 *
 * @param args string that can be parsed to integer in [1 ... 5] range
 */
static void xmce_set_display_brightness(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = xmce_parse_integer(args);

        if( val < 1 || val > 5 ) {
                errorf("%d: invalid brightness value\n", val);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS_PATH, val);
}

/** Get current display brightness from mce and print it out
 */
static void xmce_get_display_brightness(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( mcetool_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_PATH, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (1-5)\n", "Brightness:", txt);
}

/* ------------------------------------------------------------------------- *
 * cabc (content adaptive backlight control)
 * ------------------------------------------------------------------------- */

/** Set display brightness
 *
 * @param args cabc mode name
 */
static void xmce_set_cabc_mode(const char *args)
{
	static const char * const lut[] = {
		MCE_CABC_MODE_OFF,
		MCE_CABC_MODE_UI,
		MCE_CABC_MODE_STILL_IMAGE,
		MCE_CABC_MODE_MOVING_IMAGE,
		0
	};

        debugf("%s(%s)\n", __FUNCTION__, args);

	for( size_t i = 0; ; ++i ) {
		if( !lut[i] ) {
			errorf("%s: invalid cabc mode\n", args);
			exit(EXIT_FAILURE);
		}
		if( !strcmp(lut[i], args) )
			break;
	}

        xmce_ipc_no_reply(MCE_CABC_MODE_REQ,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
}

/** Get current cabc mode from mce and print it out
 */
static void xmce_get_cabc_mode(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_CABC_MODE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","CABC mode:", str ?: "unknown");
        free(str);
}

/* ------------------------------------------------------------------------- *
 * dim timeout
 * ------------------------------------------------------------------------- */

/** Set display dim timeout
 *
 * @param args string that can be parsed to integer
 */
static void xmce_set_dim_timeout(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = xmce_parse_integer(args);
        mcetool_gconf_set_int(MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH, val);
}

/** Get current dim timeout from mce and print it out
 */
static void xmce_get_dim_timeout(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( mcetool_gconf_get_int(MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (seconds)\n", "Dim timeout:", txt);
}

/** Set "allowed" display dim timeouts
 *
 * @param args string of comma separated integer numbers
 */

static void xmce_set_dim_timeouts(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gint  len = 0;
        gint *arr = parse_gint_array(args, &len);

        if( len != 5 ) {
                errorf("%s: invalid dim timeout list\n", args);
                exit(EXIT_FAILURE);
        }
	for( gint i = 1; i < len; ++i ) {
		if( arr[i] <= arr[i-1] ) {
			errorf("%s: dim timeout list not in ascending order\n", args);
			exit(EXIT_FAILURE);
		}
	}

        mcetool_gconf_set_int_array(MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST_PATH,
                                    arr, len);
        g_free(arr);
}

/** Get list of "allowed" dim timeouts from mce and print them out
 */
static void xmce_get_dim_timeouts(void)
{
        gint *vec = 0;
        gint  len = 0;

        mcetool_gconf_get_int_array(MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST_PATH,
                                    &vec, &len);
        printf("%-"PAD1"s [", "Allowed dim timeouts");
        for( gint i = 0; i < len; ++i )
                printf(" %d", vec[i]);
        printf(" ]\n");
        g_free(vec);
}

/* ------------------------------------------------------------------------- *
 * adaptive dimming timeout
 * ------------------------------------------------------------------------- */

/* Set adaptive dimming mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static void xmce_set_adaptive_dimming_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gboolean val = xmce_parse_enabled(args);
        mcetool_gconf_set_bool(MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH, val);
}

/** Get current adaptive dimming mode from mce and print it out
 */
static void xmce_get_adaptive_dimming_mode(void)
{
        gboolean val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( mcetool_gconf_get_bool(MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Adaptive dimming:", txt);
}

/** Set adaptive dimming time
 *
 * @param args string that can be parsed to integer
 */
static void xmce_set_adaptive_dimming_time(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = xmce_parse_integer(args);
        mcetool_gconf_set_int(MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH, val);
}

/** Get current adaptive dimming time from mce and print it out
 */
static void xmce_get_adaptive_dimming_time(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( mcetool_gconf_get_int(MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (milliseconds)\n", "Adaptive dimming threshold:", txt);
}

/* ------------------------------------------------------------------------- *
 * ps
 * ------------------------------------------------------------------------- */

/* Set ps use mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static void xmce_set_ps_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gboolean val = xmce_parse_enabled(args);
        mcetool_gconf_set_bool(MCE_GCONF_PROXIMITY_PS_ENABLED_PATH, val);
}

/** Get current ps mode from mce and print it out
 */
static void xmce_get_ps_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( mcetool_gconf_get_bool(MCE_GCONF_PROXIMITY_PS_ENABLED_PATH, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use ps mode:", txt);
}

/* ------------------------------------------------------------------------- *
 * als
 * ------------------------------------------------------------------------- */

/* Set als use mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static void xmce_set_als_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gboolean val = xmce_parse_enabled(args);
        mcetool_gconf_set_bool(MCE_GCONF_DISPLAY_ALS_ENABLED_PATH, val);
}

/** Get current als mode from mce and print it out
 */
static void xmce_get_als_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( mcetool_gconf_get_bool(MCE_GCONF_DISPLAY_ALS_ENABLED_PATH, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use als mode:", txt);
}

/* ------------------------------------------------------------------------- *
 * autolock
 * ------------------------------------------------------------------------- */

/* Set autolock mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static void xmce_set_autolock_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gboolean val = xmce_parse_enabled(args);
        mcetool_gconf_set_bool(MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH, val);
}

/** Get current autolock mode from mce and print it out
 */
static void xmce_get_autolock_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( mcetool_gconf_get_bool(MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Touchscreen/Keypad autolock:", txt);
}

/* ------------------------------------------------------------------------- *
 * blank timeout
 * ------------------------------------------------------------------------- */

/** Set display blanking timeout
 *
 * @param args string that can be parsed to integer
 */
static void xmce_set_blank_timeout(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = xmce_parse_integer(args);
        mcetool_gconf_set_int(MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH, val);
}

/** Get current blank timeout from mce and print it out
 */
static void xmce_get_blank_timeout(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( mcetool_gconf_get_int(MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (seconds)\n", "Blank timeout:", txt);
}

/* ------------------------------------------------------------------------- *
 * doubletab
 * ------------------------------------------------------------------------- */

/** Lookup table for doubletap gesture policies
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t doubletap_values[] = {
        { "disabled",           0 },
        { "show-unlock-screen", 1 },
        { "unlock",             2 },
        { NULL, -1 }
};

/** Set doubletap mode
 *
 * @param args string that can be parsed to doubletap mode
 */
static void xmce_set_doubletap_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(doubletap_values, args);
        if( val < 0 ) {
                errorf("%s: invalid doubletap policy value\n", args);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH, val);
}

/** Get current doubletap mode from mce and print it out
 */
static void xmce_get_doubletap_mode(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_int(MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH, &val) )
                txt = rlookup(doubletap_values, val);
        printf("%-"PAD1"s %s \n", "Double-tap gesture policy:", txt ?: "unknown");
}

/** Lookup table for doubletap wakeup policies
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t doubletap_wakeup[] = {
        { "never",     DBLTAP_ENABLE_NEVER },
        { "always",    DBLTAP_ENABLE_ALWAYS },
        { "proximity", DBLTAP_ENABLE_NO_PROXIMITY },
        { NULL, -1 }
};

/** Set doubletap wakeup mode
 *
 * @param args string that can be parsed to doubletap wakeup mode
 */
static void xmce_set_doubletap_wakeup(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(doubletap_wakeup, args);
        if( val < 0 ) {
                errorf("%s: invalid doubletap policy value\n", args);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_DOUBLETAP_MODE, val);
}

/** Get current doubletap wakeup mode from mce and print it out
 */
static void xmce_get_doubletap_wakeup(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_int(MCE_GCONF_DOUBLETAP_MODE, &val) )
                txt = rlookup(doubletap_wakeup, val);
        printf("%-"PAD1"s %s \n", "Double-tap wakeup policy:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * psm (power saving mode)
 * ------------------------------------------------------------------------- */

/* Set power saving mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static void xmce_set_power_saving_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gboolean val = xmce_parse_enabled(args);
        mcetool_gconf_set_bool(MCE_GCONF_PSM_PATH, val);
}

/** Get current power saving mode from mce and print it out
 */
static void xmce_get_power_saving_mode(void)
{
        gboolean mode  = 0;
        gboolean state = 0;
        char txt1[32] = "unknown";
        char txt2[32] = "unknown";

        if( mcetool_gconf_get_bool(MCE_GCONF_PSM_PATH, &mode) )
                snprintf(txt1, sizeof txt1, "%s", mode ? "enabled" : "disabled");

        if( xmce_ipc_bool_reply(MCE_PSM_STATE_GET, &state, DBUS_TYPE_INVALID) )
                snprintf(txt2, sizeof txt2, "%s", state ? "active" : "inactive");

        printf("%-"PAD1"s %s (%s)\n", "Power saving mode:", txt1, txt2);
}

/** Set power saving mode threshold
 *
 * @param args string that can be parsed to integer
 */
static void xmce_set_psm_threshold(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = xmce_parse_integer(args);

        if( val < 10 || val > 50 || val % 10 ) {
                errorf("%d: invalid psm threshold value\n", val);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_PSM_THRESHOLD_PATH, val);
}

/** Get current power saving threshold from mce and print it out
 */
static void xmce_get_psm_threshold(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( mcetool_gconf_get_int(MCE_GCONF_PSM_THRESHOLD_PATH, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (%%)\n", "PSM threshold:", txt);
}

/* Set forced power saving mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static void xmce_set_forced_psm(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gboolean val = xmce_parse_enabled(args);
        mcetool_gconf_set_bool(MCE_GCONF_FORCED_PSM_PATH, val);
}

/** Get current forced power saving mode from mce and print it out
 */
static void xmce_get_forced_psm(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( mcetool_gconf_get_bool(MCE_GCONF_FORCED_PSM_PATH, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Forced power saving mode:", txt);
}

/* ------------------------------------------------------------------------- *
 * lpm (low power mode)
 * ------------------------------------------------------------------------- */

/* Set display low power mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static void xmce_set_low_power_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        gboolean val = xmce_parse_enabled(args);
        mcetool_gconf_set_bool(MCE_GCONF_USE_LOW_POWER_MODE_PATH, val);
}

/** Get current low power mode state from mce and print it out
 */
static void xmce_get_low_power_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( mcetool_gconf_get_bool(MCE_GCONF_USE_LOW_POWER_MODE_PATH, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use low power mode:", txt);
}

/* ------------------------------------------------------------------------- *
 * blanking inhibit
 * ------------------------------------------------------------------------- */

static void xmce_set_inhibit_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = parse_inhibitmode(args);
        mcetool_gconf_set_int(MCE_GCONF_BLANKING_INHIBIT_MODE_PATH, val);
}

/** Get current blanking inhibit mode from mce and print it out
 */
static void xmce_get_inhibit_mode(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_int(MCE_GCONF_BLANKING_INHIBIT_MODE_PATH, &val) )
                txt = repr_inhibitmode(val);
        printf("%-"PAD1"s %s \n", "Blank inhibit:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * cpu scaling governor override
 * ------------------------------------------------------------------------- */

static void xmce_set_cpu_scaling_governor(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(governor_values, args);
        if( val < 0 ) {
                errorf("%s: invalid cpu scaling governor value\n", args);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_CPU_SCALING_GOVERNOR_PATH, val);
}

/** Get current autosuspend policy from mce and print it out
 */
static void xmce_get_cpu_scaling_governor(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_int(MCE_GCONF_CPU_SCALING_GOVERNOR_PATH, &val) )
                txt = rlookup(governor_values, val);
        printf("%-"PAD1"s %s \n", "CPU Scaling Governor:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * never blank
 * ------------------------------------------------------------------------- */

static void xmce_set_never_blank(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(never_blank_values, args);
        if( val < 0 ) {
                errorf("%s: invalid never blank value\n", args);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_DISPLAY_NEVER_BLANK_PATH, val);
}

static void xmce_get_never_blank(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_int(MCE_GCONF_DISPLAY_NEVER_BLANK_PATH, &val) )
                txt = rlookup(never_blank_values, val);
        printf("%-"PAD1"s %s \n", "Display never blank:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * autosuspend on display blank policy
 * ------------------------------------------------------------------------- */

static void xmce_set_suspend_policy(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(suspendpol_values, args);
        if( val < 0 ) {
                errorf("%s: invalid suspend policy value\n", args);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_USE_AUTOSUSPEND_PATH, val);
}

/** Get current autosuspend policy from mce and print it out
 */
static void xmce_get_suspend_policy(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_int(MCE_GCONF_USE_AUTOSUSPEND_PATH, &val) )
                txt = rlookup(suspendpol_values, val);
        printf("%-"PAD1"s %s \n", "Autosuspend policy:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * use mouse clicks to emulate touchscreen doubletap policy
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_DOUBLETAP_EMULATION
static void xmce_set_fake_doubletap(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(fake_doubletap_values, args);
        if( val < 0 ) {
                errorf("%s: invalid fake doubletap value\n", args);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_bool(MCE_GCONF_USE_FAKE_DOUBLETAP_PATH, val != 0);
}

/** Get current fake double tap policy from mce and print it out
 */
static void xmce_get_fake_doubletap(void)
{
        gboolean    val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_bool(MCE_GCONF_USE_FAKE_DOUBLETAP_PATH, &val) )
                txt = rlookup(fake_doubletap_values, val);
        printf("%-"PAD1"s %s \n", "Use fake doubletap:", txt ?: "unknown");
}
#endif /* ENABLE_DOUBLETAP_EMULATION */

/* ------------------------------------------------------------------------- *
 * tklock
 * ------------------------------------------------------------------------- */

/** Lookup table for tklock open values
 */
static const symbol_t tklock_open_values[] = {
#if 0 // DEPRECATED
        { "none",     TKLOCK_NONE },
        { "enable",   TKLOCK_ENABLE },
        { "help",     TKLOCK_HELP },
        { "select",   TKLOCK_SELECT },
#endif
        { "oneinput", TKLOCK_ONEINPUT },
        { "visual",   TKLOCK_ENABLE_VISUAL },
        { "lpm",      TKLOCK_ENABLE_LPM_UI },
        { "pause",    TKLOCK_PAUSE_UI },
        { NULL, -1 }
};

/** Simulate tklock open from mce to lipstick
 */
static void xmce_tklock_open(const char *args)
{
	debugf("%s(%s)\n", __FUNCTION__, args);
	int val = lookup(tklock_open_values, args);
	if( val < 0 ) {
		errorf("%s: invalid tklock open value\n", args);
		exit(EXIT_FAILURE);
	}

        DBusConnection *bus = xdbus_init();
	DBusMessage    *rsp = 0;
	DBusMessage    *req = 0;
        DBusError       err = DBUS_ERROR_INIT;

	const char   *cb_service   = MCE_SERVICE;
	const char   *cb_path      = MCE_REQUEST_PATH;
	const char   *cb_interface = MCE_REQUEST_IF;
	const char   *cb_method    = MCE_TKLOCK_CB_REQ;
	dbus_uint32_t mode         = (dbus_uint32_t)val;
	dbus_bool_t   silent       = TRUE;
	dbus_bool_t   flicker_key  = FALSE;

	req = dbus_message_new_method_call(SYSTEMUI_SERVICE,
					   SYSTEMUI_REQUEST_PATH,
					   SYSTEMUI_REQUEST_IF,
					   SYSTEMUI_TKLOCK_OPEN_REQ);
	if( !req ) goto EXIT;

	dbus_message_append_args(req,
				 DBUS_TYPE_STRING, &cb_service,
				 DBUS_TYPE_STRING, &cb_path,
				 DBUS_TYPE_STRING, &cb_interface,
				 DBUS_TYPE_STRING, &cb_method,
				 DBUS_TYPE_UINT32, &mode,
				 DBUS_TYPE_BOOLEAN, &silent,
				 DBUS_TYPE_BOOLEAN, &flicker_key,
				 DBUS_TYPE_INVALID);

	rsp = dbus_connection_send_with_reply_and_block(bus, req, -1, &err);

	if( !req ) {
		errorf("no reply to %s; %s: %s\n", SYSTEMUI_TKLOCK_OPEN_REQ,
		       err.name, err.message);
		goto EXIT;
	}
	printf("got reply to %s\n", SYSTEMUI_TKLOCK_OPEN_REQ);

EXIT:
	if( rsp ) dbus_message_unref(rsp), rsp = 0;
	if( req ) dbus_message_unref(req), req = 0;
	dbus_error_free(&err);
}

/** Simulate tklock close from mce to lipstick
 */
static void xmce_tklock_close(void)
{
	debugf("%s(%s)\n", __FUNCTION__, args);

        DBusConnection *bus = xdbus_init();
	DBusMessage    *rsp = 0;
	DBusMessage    *req = 0;
        DBusError       err = DBUS_ERROR_INIT;

	dbus_bool_t silent = TRUE;

	req = dbus_message_new_method_call(SYSTEMUI_SERVICE,
					   SYSTEMUI_REQUEST_PATH,
					   SYSTEMUI_REQUEST_IF,
					   SYSTEMUI_TKLOCK_CLOSE_REQ);
	if( !req ) goto EXIT;

	dbus_message_append_args(req,
				 DBUS_TYPE_BOOLEAN, &silent,
				 DBUS_TYPE_INVALID);

	rsp = dbus_connection_send_with_reply_and_block(bus, req, -1, &err);

	if( !req ) {
		errorf("no reply to %s; %s: %s\n", SYSTEMUI_TKLOCK_CLOSE_REQ,
		       err.name, err.message);
		goto EXIT;
	}
	printf("got reply to %s\n", SYSTEMUI_TKLOCK_CLOSE_REQ);

EXIT:
	if( rsp ) dbus_message_unref(rsp), rsp = 0;
	if( req ) dbus_message_unref(req), req = 0;
	dbus_error_free(&err);
}

/** Lookup table for tklock callback values
 */
static const symbol_t tklock_callback_values[] = {
        { "unlock",  TKLOCK_UNLOCK  },
        { "retry",   TKLOCK_RETRY   },
        { "timeout", TKLOCK_TIMEOUT },
        { "closed",  TKLOCK_CLOSED  },
        { NULL, -1 }
};

/** Simulate tklock callback from lipstick to mce
 */
static void xmce_tklock_callback(const char *args)
{
  debugf("%s(%s)\n", __FUNCTION__, args);
  dbus_int32_t val = lookup(tklock_callback_values, args);
  if( val < 0 ) {
    errorf("%s: invalidt klock callback value\n", args);
    exit(EXIT_FAILURE);
  }

  xmce_ipc_no_reply(MCE_TKLOCK_CB_REQ,
		    DBUS_TYPE_INT32, &val,
		    DBUS_TYPE_INVALID);
}

/** Enable/disable the tklock
 *
 * @param mode The mode to change to; valid modes:
 *             "locked", "locked-dim", "locked-delay", "unlocked"
 */
static void xmce_set_tklock_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        xmce_ipc_no_reply(MCE_TKLOCK_MODE_CHANGE_REQ,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
}

/** Get current tklock mode from mce and print it out
 */
static void xmce_get_tklock_mode(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_TKLOCK_MODE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n", "Touchscreen/Keypad lock:", str ?: "unknown");
        free(str);
}

/* Set tklock blanking inhibit mode
 *
 * @param args string that can be parsed to inhibit mode
 */
static void xmce_set_tklock_blank(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(tklockblank_values, args);
        if( val < 0 ) {
                errorf("%s: invalid lockscreen blanking policy value\n", args);
                exit(EXIT_FAILURE);
        }
        mcetool_gconf_set_int(MCE_GCONF_TK_AUTO_BLANK_DISABLE_PATH, val);
}

/** Get current tklock autoblank inhibit mode from mce and print it out
 */
static void xmce_get_tklock_blank(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( mcetool_gconf_get_int(MCE_GCONF_TK_AUTO_BLANK_DISABLE_PATH, &val) )
                txt = rlookup(tklockblank_values, val);
        printf("%-"PAD1"s %s \n", "Tklock autoblank policy:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * misc
 * ------------------------------------------------------------------------- */

/** Get mce version from mce and print it out
 */
static void xmce_get_version(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_VERSION_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","MCE version:", str ?: "unknown");
        free(str);
}

/** Get inactivity state from mce and print it out
 */
static void xmce_get_inactivity_state(void)
{
        gboolean val = 0;
        char txt[32];
        strcpy(txt, "unknown");
        if( xmce_ipc_bool_reply(MCE_INACTIVITY_STATUS_GET, &val, DBUS_TYPE_INVALID) )
                snprintf(txt, sizeof txt, "%s", val ? "inactive" : "active");
        printf("%-"PAD1"s %s\n","Inactivity status:", txt);
}

/** Get keyboard backlight state from mce and print it out
 */
static void xmce_get_keyboard_backlight_state(void)
{
        gboolean val = 0;
        char txt[32];
        strcpy(txt, "unknown");
        if( xmce_ipc_bool_reply(MCE_KEY_BACKLIGHT_STATE_GET, &val, DBUS_TYPE_INVALID) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n","Keyboard backlight:", txt);
}

/** Obtain and print mce status information
 */
static void xmce_get_status(void)
{
        printf("\n"
                "MCE status:\n"
                "-----------\n");

        xmce_get_version();
        xmce_get_radio_states();
        xmce_get_call_state();
        xmce_get_display_state();
        xmce_get_color_profile();
        xmce_get_display_brightness();
        xmce_get_cabc_mode();
        xmce_get_dim_timeout();
        xmce_get_adaptive_dimming_mode();
        xmce_get_adaptive_dimming_time();
        xmce_get_never_blank();
        xmce_get_blank_timeout();
        xmce_get_inhibit_mode();
        xmce_get_keyboard_backlight_state();
        xmce_get_inactivity_state();
        xmce_get_power_saving_mode();
        xmce_get_forced_psm();
        xmce_get_psm_threshold();
        xmce_get_tklock_mode();
        xmce_get_autolock_mode();
        xmce_get_doubletap_mode();
        xmce_get_doubletap_wakeup();
        xmce_get_low_power_mode();
        xmce_get_als_mode();
        xmce_get_ps_mode();
        xmce_get_dim_timeouts();
        xmce_get_suspend_policy();
        xmce_get_cpu_scaling_governor();
#ifdef ENABLE_DOUBLETAP_EMULATION
	xmce_get_fake_doubletap();
#endif
        xmce_get_tklock_blank();

        printf("\n");
}

/* ------------------------------------------------------------------------- *
 * special
 * ------------------------------------------------------------------------- */

/** Handle --block command line option
 *
 * @param args optarg from command line, or NULL
 */
static void mcetool_block(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args ?: "inf");
        struct timespec ts;

        if( mcetool_parse_timspec(&ts, args) )
                TEMP_FAILURE_RETRY(nanosleep(&ts, &ts));
        else
                pause();
}

/** Handle --demo-mode command line option
 *
 * @param args optarg from command line
 */
static void xmce_set_demo_mode(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        if( !strcmp(args, "on") ) {
                // mcetool --unblank-screen
                //         --set-inhibit-mode=stay-on
                //         --set-tklock-mode=unlocked
                //         --set-tklock-blank=disabled
                xmce_set_display_state("on");
                xmce_set_inhibit_mode("stay-on");
                xmce_set_tklock_mode("unlocked");
                xmce_set_tklock_blank("disabled");
        }
        else if( !strcmp(args, "off")) {
                // mcetool --unblank-screen --dim-screen --blank-screen
                //         --set-inhibit-mode=disabled
                //         --set-tklock-mode=locked
                //         --set-tklock-blank=enabled
                xmce_set_display_state("on");
                xmce_set_display_state("dim");
                xmce_set_display_state("off");
                xmce_set_inhibit_mode("disabled");
                xmce_set_tklock_mode("locked");
                xmce_set_tklock_blank("enabled");
        }
        else {
                errorf("%s: invalid demo mode value\n", args);
                exit(EXIT_FAILURE);
        }
}
#define EXTRA "\t\t"
#define PARAM "  "
/** usage information */
static const char usage_text[] =
"Usage: "PROG_NAME" [OPTION]\n"
"Mode Control Entity tool\n"
"\n"
PARAM"-U, --unblank-screen\n"
EXTRA"send display on request\n"
PARAM"-d, --dim-screen\n"
EXTRA"send display dim request\n"
PARAM"-n, --blank-screen\n"
EXTRA"send display off request\n"
PARAM"-P, --blank-prevent\n"
EXTRA"send blank prevent request\n"
PARAM"-v, --cancel-blank-prevent\n"
EXTRA"send cancel blank prevent request\n"
PARAM"-G, --set-dim-timeout=<secs>\n"
EXTRA"set the automatic dimming timeout\n"
PARAM"-O, --set-dim-timeouts=<secs,secs,...>\n"
EXTRA"set the allowed dim timeouts; valid list must\n"
EXTRA"  must have 5 entries, in ascending order\n"
PARAM"-f, --set-adaptive-dimming-mode=<enabled|disabled>\n"
EXTRA"set the adaptive dimming mode; valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
PARAM"-J, --set-adaptive-dimming-time=<secs>\n"
EXTRA"set the adaptive dimming threshold\n"
PARAM"-o, --set-blank-timeout=<secs>\n"
EXTRA"set the automatic blanking timeout\n"
PARAM"-j, --set-never-blank=<enabled|disabled>\n"
EXTRA"set never blank mode; valid modes are:\n"
EXTRA"  'disabled', 'enabled'\n"
PARAM"-K, --set-autolock-mode=<enabled|disabled>\n"
EXTRA"set the autolock mode; valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
PARAM"-t, --set-tklock-blank=<enabled|disabled>\n"
EXTRA"set the touchscreen/keypad autoblank mode;\n"
EXTRA"  valid modes are: 'enabled' and 'disabled'\n"
PARAM"-I, --set-inhibit-mode=<disabled|stay-on-with-charger|stay-on|stay-dim-with-charger|stay-dim>\n"
EXTRA"set the blanking inhibit mode to MODE;\n"
EXTRA"  valid modes are:\n"
EXTRA"  'disabled',\n"
EXTRA"  'stay-on-with-charger', 'stay-on',\n"
EXTRA"  'stay-dim-with-charger', 'stay-dim'\n"
PARAM"-k, --set-tklock-mode=<locked|locked-dim|locked-delay|unlocked>\n"
EXTRA"set the touchscreen/keypad lock mode;\n"
EXTRA"  valid modes are:\n"
EXTRA"  'locked', 'locked-dim',\n"
EXTRA"  'locked-delay',\n"
EXTRA"  and 'unlocked'\n"
PARAM"-m, --tklock-callback=<unlock|retry|timeout|closed>\n"
EXTRA"simulate tklock callback from systemui\n"
PARAM"-q, --tklock-open=<oneinput|visual|lpm|pause>\n"
EXTRA"simulate tklock open from mce\n"
PARAM"-Q, --tklock-close\n"
EXTRA"simulate tklock close from mce\n"
PARAM"-M, --set-doubletap-mode=<disabled|show-unlock-screen|unlock>\n"
EXTRA"set the autolock mode; valid modes are:\n"
EXTRA"  'disabled', 'show-unlock-screen', 'unlock'\n"
PARAM"-z, --set-doubletap-wakeup=<never|always|proximity>\n"
EXTRA"set the doubletap wakeup mode; valid modes are:\n"
EXTRA"  'never', 'always', 'proximity'\n"
PARAM"-r, --enable-radio=<master|cellular|wlan|bluetooth>\n"
EXTRA"enable the specified radio; valid radios are:\n"
EXTRA"  'master', 'cellular',\n"
EXTRA"  'wlan' and 'bluetooth';\n"
EXTRA"  'master' affects all radios\n"
PARAM"-R, --disable-radio=<master|cellular|wlan|bluetooth>\n"
EXTRA"disable the specified radio; valid radios are:\n"
EXTRA"  'master', 'cellular',\n"
EXTRA"  'wlan' and 'bluetooth';\n"
EXTRA"  'master' affects all radios\n"
PARAM"-p, --set-power-saving-mode=<enabled|disabled>\n"
EXTRA"set the power saving mode; valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
PARAM"-T, --set-psm-threshold=<10|20|30|40|50>\n"
EXTRA"set the threshold for the power saving mode;\n"
EXTRA"  valid values are:\n"
EXTRA"  10, 20, 30, 40, 50\n"
PARAM"-F, --set-forced-psm=<enabled|disabled>\n"
EXTRA"the forced power saving mode to MODE;\n"
EXTRA"  valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
PARAM"-E, --set-low-power-mode=<enabled|disabled>\n"
EXTRA"set the low power mode; valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
PARAM"-s, --set-suspend-policy=<enabled|disabled|early>\n"
EXTRA"set the autosuspend mode; valid modes are:\n"
EXTRA"  'enabled', 'disabled' and 'early'\n"
PARAM"-S, --set-cpu-scaling-governor=<automatic|performance|interactive>\n"
EXTRA"set the cpu scaling governor override; valid\n"
EXTRA"  modes are: 'automatic', 'performance',\n"
EXTRA"  'interactive'\n"
#ifdef ENABLE_DOUBLETAP_EMULATION
PARAM"-i, --set-fake-doubletap=<enabled|disabled>\n"
EXTRA"set the doubletap emulation mode; valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
#endif
PARAM"-b, --set-display-brightness=<1|2|3|4|5>\n"
EXTRA"set the display brightness to BRIGHTNESS;\n"
EXTRA"  valid values are: 1-5\n"
PARAM"-g, --set-als-mode=<enabled|disabled>\n"
EXTRA"set the als mode; valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
PARAM"-u, --set-ps-mode=<enabled|disabled>\n"
EXTRA"set the ps mode; valid modes are:\n"
EXTRA"  'enabled' and 'disabled'\n"
PARAM"-a, --get-color-profile-ids\n"
EXTRA"get available color profile ids\n"
PARAM"-A, --set-color-profile=ID\n"
EXTRA"set the color profile to ID; valid ID names\n"
EXTRA"  can be obtained with --get-color-profile-ids\n"
PARAM"-C, --set-cabc-mode=<off|ui|still-image|moving-image>\n"
EXTRA"set the CABC mode to MODE;\n"
EXTRA"  valid modes are:\n"
EXTRA"  'off', 'ui',\n"
EXTRA"  'still-image' and 'moving-image'\n"
PARAM"-c, --set-call-state=<none|ringing|active|service>:<normal|emergency>\n"
EXTRA"set the call state to STATE and the call type\n"
EXTRA"  to TYPE; valid states are:\n"
EXTRA"  'none', 'ringing',\n"
EXTRA"  'active' and 'service'\n"
EXTRA"  valid types are:\n"
EXTRA"  'normal' and 'emergency'\n"
PARAM"-l, --enable-led\n"
EXTRA"enable LED framework\n"
PARAM"-L, --disable-led\n"
EXTRA"disable LED framework\n"
PARAM"-y, --activate-led-pattern=PATTERN\n"
EXTRA"activate a LED pattern\n"
PARAM"-Y, --deactivate-led-pattern=PATTERN\n"
EXTRA"deactivate a LED pattern\n"
PARAM"-e, --powerkey-event=<short|double|long>\n"
EXTRA"trigger a powerkey event; valid types are:\n"
EXTRA"  'short', 'double' and 'long'\n"
PARAM"-D, --set-demo-mode=<on|off>\n"
EXTRA"  set the display demo mode  to STATE;\n"
EXTRA"     valid states are: 'on' and 'off'\n"
PARAM"-N, --status\n"
EXTRA"output MCE status\n"
PARAM"-B, --block[=<secs>]\n"
EXTRA"block after executing commands\n"
EXTRA"  for D-Bus\n"
PARAM"-h, --help\n"
EXTRA"display list of options and exit\n"
PARAM"-H, --long-help\n"
EXTRA"display full usage information  and exit\n"
PARAM"-V, --version\n"
EXTRA"output version information and exit\n"
"\n"
"If no options are specified, the status is output.\n"
"\n"
"If non-option arguments are given, matching parts of long help\n"
"is printed out.\n"
"\n"
"Report bugs to <david.weinehall@nokia.com>\n"
;

/** Show full usage information
 */
static void usage_long(void)
{
	printf("%s\n", usage_text);
}

/** Show only option names
 */
static void usage_short(void)
{
	char *temp = strdup(usage_text);
	for( char *zen = temp; zen; )
	{
		char *now = zen;
		if( (zen = strchr(now, '\n')) )
			*zen++ = 0;
		if( *now != '\t' )
			printf("%s\n", now);
	}
	free(temp);
}

/** Show options matching partial strings given at command line
 */
static void usage_quick(char **pat)
{
	char *temp = strdup(usage_text);
	bool active = false;
	for( char *zen = temp; zen; )
	{
		char *now = zen;
		if( (zen = strchr(now, '\n')) )
			*zen++ = 0;
		if( *now == ' ' ) {
			active = false;
			for( int i = 0; pat[i]; ++i ) {
				if( !strcasestr(now, pat[i]) )
					continue;
				active = true;
				break;
			}
		}
		if( *now != ' ' && *now != '\t' )
			continue;
		if( active )
			printf("%s\n", now);
	}
	free(temp);
}

/** Version information */
static const char version_text[] =
PROG_NAME" v"G_STRINGIFY(PRG_VERSION)"\n"
"Written by David Weinehall.\n"
"\n"
"Copyright (C) 2005-2011 Nokia Corporation.  All rights reserved.\n"
;

// Unused short options left ....
// - - - - - - - - - - - - - - - - - - - - - - w x - -
// - - - - - - - - - - - - - - - - - - - - - - W X - Z

const char OPT_S[] =
"B::" // --block,
"P"   // --blank-prevent,
"v"   // --cancel-blank-prevent,
"U"   // --unblank-screen,
"d"   // --dim-screen,
"n"   // --blank-screen,
"b:"  // --set-display-brightness,
"I:"  // --set-inhibit-mode,
"D:"  // --set-demo-mode,
"C:"  // --set-cabc-mode,
"a"   // --get-color-profile-ids,
"A:"  // --set-color-profile,
"c:"  // --set-call-state,
"r:"  // --enable-radio,
"R:"  // --disable-radio,
"p:"  // --set-power-saving-mode,
"F:"  // --set-forced-psm,
"T:"  // --set-psm-threshold,
"k:"  // --set-tklock-mode,
"l"   // --enable-led,
"L"   // --disable-led,
"y:"  // --activate-led-pattern,
"Y:"  // --deactivate-led-pattern,
"e:"  // --powerkey-event,
"N"   // --status,
"h"   // --help,
"H"   // --long-help,
"V"   // --version,
"f:"  // --set-adaptive-dimming-mode
"E:"  // --set-low-power-mode
"g:"  // --set-als-mode
"G:"  // --set-dim-timeout
"o:"  // --set-blank-timeout
"j:"  // --set-never-blank
"J:"  // --set-adaptive-dimming-time
"K:"  // --set-autolock-mode
"M:"  // --set-doubletap-mode
"z:"  // --set-doubletap-wakeup
"O:"  // --set-dim-timeouts
"s:"  // --set-suspend-policy
"S:"  // --set-cpu-scaling-governor
"t:"  // --set-tklock-blank
"m:"  // --tklock-callback
"q:"  // --tklock-open
"Q"   // --tklock-close
"u:"  // --set-ps-mode
#ifdef ENABLE_DOUBLETAP_EMULATION
"i:"  // --set-fake-doubletap
#endif
;

struct option const OPT_L[] =
{
        { "block",                     2, 0, 'B' }, // N/A
        { "blank-prevent",             0, 0, 'P' }, // xmce_prevent_display_blanking()
        { "cancel-blank-prevent",      0, 0, 'v' }, // xmce_allow_display_blanking()
        { "unblank-screen",            0, 0, 'U' }, // xmce_set_display_state("on")
        { "dim-screen",                0, 0, 'd' }, // xmce_set_display_state("dim")
        { "blank-screen",              0, 0, 'n' }, // xmce_set_display_state("off")
        { "set-display-brightness",    1, 0, 'b' }, // xmce_set_display_brightness()
        { "set-inhibit-mode",          1, 0, 'I' }, // xmce_set_inhibit_mode()
        { "set-demo-mode",             1, 0, 'D' }, // xmce_set_demo_mode()
        { "set-cabc-mode",             1, 0, 'C' }, // xmce_set_cabc_mode()
        { "get-color-profile-ids",     0, 0, 'a' }, // xmce_get_color_profile_ids()
        { "set-color-profile",         1, 0, 'A' }, // xmce_set_color_profile()
        { "set-call-state",            1, 0, 'c' }, // xmce_set_call_state()
        { "enable-radio",              1, 0, 'r' }, // xmce_enable_radio()
        { "disable-radio",             1, 0, 'R' }, // xmce_disable_radio()
        { "set-power-saving-mode",     1, 0, 'p' }, // xmce_set_power_saving_mode()
        { "set-forced-psm",            1, 0, 'F' }, // xmce_set_forced_psm()
        { "set-psm-threshold",         1, 0, 'T' }, // xmce_set_psm_threshold()
        { "set-tklock-mode",           1, 0, 'k' }, // xmce_set_tklock_mode()
        { "tklock-callback",           1, 0, 'm' }, // xmce_tklock_callback()
        { "tklock-open",               1, 0, 'q' }, // xmce_tklock_open()
        { "tklock-close",              0, 0, 'Q' }, // xmce_tklock_close()
        { "set-tklock-blank",          1, 0, 't' }, // xmce_set_tklock_blank()
        { "enable-led",                0, 0, 'l' }, // set_led_state()
        { "disable-led",               0, 0, 'L' }, // set_led_state()
        { "activate-led-pattern",      1, 0, 'y' }, // set_led_pattern_state()
        { "deactivate-led-pattern",    1, 0, 'Y' }, // set_led_pattern_state()
        { "powerkey-event",            1, 0, 'e' }, // xmce_powerkey_event()
        { "status",                    0, 0, 'N' }, // xmce_get_status()
        { "help",                      0, 0, 'h' }, // N/A
        { "long-help",                 0, 0, 'H' }, // N/A
        { "version",                   0, 0, 'V' }, // N/A
        { "set-adaptive-dimming-mode", 1, 0, 'f' }, // xmce_set_adaptive_dimming_mode()
        { "set-adaptive-dimming-time", 1, 0, 'J' }, // xmce_set_adaptive_dimming_time()
        { "set-low-power-mode",        1, 0, 'E' }, // xmce_set_low_power_mode()
        { "set-als-mode",              1, 0, 'g' }, // xmce_set_als_mode()
        { "set-ps-mode",               1, 0, 'u' }, // xmce_set_ps_mode()
        { "set-dim-timeout",           1, 0, 'G' }, // xmce_set_dim_timeout()
        { "set-never-blank",           1, 0, 'j' }, // xmce_set_never_blank()
        { "set-blank-timeout",         1, 0, 'o' }, // xmce_set_blank_timeout()
        { "set-autolock-mode",         1, 0, 'K' }, // xmce_set_autolock_mode()
        { "set-doubletap-mode",        1, 0, 'M' }, // xmce_set_doubletap_mode()
        { "set-doubletap-wakeup",      1, 0, 'z' }, // xmce_set_doubletap_wakeup()
        { "set-dim-timeouts",          1, 0, 'O' }, // xmce_set_dim_timeouts()
        { "set-suspend-policy",        1, 0, 's' }, // xmce_set_suspend_policy()
        { "set-cpu-scaling-governor",  1, 0, 's' }, // xmce_set_cpu_scaling_governor()
#ifdef ENABLE_DOUBLETAP_EMULATION
        { "set-fake-doubletap",        1, 0, 'i' }, // xmce_set_fake_doubletap()
#endif
        { 0, 0, 0, 0 }
};

/** Main
 *
 * @param argc Number of command line arguments
 * @param argv Array with command line arguments
 * @return 0 on success, non-zero on failure
 */
int main(int argc, char **argv)
{
        int exitcode = EXIT_FAILURE;

        /* No args -> show mce status */
        if( argc < 2 )
                xmce_get_status();

        /* Parse the command-line options */
        for( ;; )
        {
                int opt = getopt_long(argc, argv, OPT_S, OPT_L, 0);

                if( opt < 0 )
                        break;

                switch( opt )
                {
                case 'U': xmce_set_display_state("on");           break;
                case 'd': xmce_set_display_state("dim");          break;
                case 'n': xmce_set_display_state("off");          break;

                case 'P': xmce_prevent_display_blanking();        break;
                case 'v': xmce_allow_display_blanking();          break;

                case 'G': xmce_set_dim_timeout(optarg);           break;
                case 'O': xmce_set_dim_timeouts(optarg);          break;
                case 'f': xmce_set_adaptive_dimming_mode(optarg); break;
                case 'J': xmce_set_adaptive_dimming_time(optarg); break;

                case 'j': xmce_set_never_blank(optarg);           break;
                case 'o': xmce_set_blank_timeout(optarg);         break;

                case 'K': xmce_set_autolock_mode(optarg);         break;
                case 't': xmce_set_tklock_blank(optarg);          break;
                case 'I': xmce_set_inhibit_mode(optarg);          break;
                case 'k': xmce_set_tklock_mode(optarg);           break;
		case 'm': xmce_tklock_callback(optarg);           break;
		case 'q': xmce_tklock_open(optarg);               break;
		case 'Q': xmce_tklock_close();                    break;
                case 'M': xmce_set_doubletap_mode(optarg);        break;
                case 'z': xmce_set_doubletap_wakeup(optarg);        break;

                case 'r': xmce_enable_radio(optarg);              break;
                case 'R': xmce_disable_radio(optarg);             break;

                case 'p': xmce_set_power_saving_mode(optarg);     break;
                case 'T': xmce_set_psm_threshold(optarg);         break;
                case 'F': xmce_set_forced_psm(optarg);            break;
                case 'E': xmce_set_low_power_mode(optarg);        break;

                case 's': xmce_set_suspend_policy(optarg);        break;
		case 'S': xmce_set_cpu_scaling_governor(optarg);  break;
#ifdef ENABLE_DOUBLETAP_EMULATION
                case 'i': xmce_set_fake_doubletap(optarg);        break;
#endif
                case 'b': xmce_set_display_brightness(optarg);    break;
                case 'g': xmce_set_als_mode(optarg);              break;
                case 'u': xmce_set_ps_mode(optarg);               break;

                case 'a': xmce_get_color_profile_ids();           break;
                case 'A': xmce_set_color_profile(optarg);         break;
                case 'C': xmce_set_cabc_mode(optarg);             break;

                case 'c': xmce_set_call_state(optarg);            break;

                case 'l': set_led_state(TRUE);                    break;
                case 'L': set_led_state(FALSE);                   break;
                case 'y': set_led_pattern_state(optarg, TRUE);    break;
                case 'Y': set_led_pattern_state(optarg, FALSE);   break;

                case 'e': xmce_powerkey_event(optarg);            break;

                case 'D': xmce_set_demo_mode(optarg);             break;

                case 'N': xmce_get_status();                      break;
                case 'B': mcetool_block(optarg);                  break;

                case 'h':
			usage_short();
                        exitcode = EXIT_SUCCESS;
                        goto EXIT;

                case 'H':
			usage_long();
                        exitcode = EXIT_SUCCESS;
                        goto EXIT;

                case 'V':
                        printf("%s\n", version_text);
                        exitcode = EXIT_SUCCESS;
                        goto EXIT;

                default:
                        goto EXIT;
                }
        }

        /* Non-flag arguments are quick help patterns */
        if( optind < argc ) {
		usage_quick(argv + optind);
        }

        exitcode = EXIT_SUCCESS;

EXIT:
        xdbus_exit();

        return exitcode;
}

