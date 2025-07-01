/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 * X command server by Flemming Madsen
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 *
 * if_xcmdsrv.c: Functions for passing commands through an X11 display.
 *
 */

#include "vim.h"
#include "version.h"

#if defined(FEAT_CLIENTSERVER) && defined(HAVE_GIO) && defined(UNIX)

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

static GSocketService	*socket_service;
static GSocketAddress	*socket_address;
static char		*socket_path;

static char_u	    *get_socket_dir(void);
static gboolean	    handle_signal_incoming(GSocketService *service,
	GSocketConnection *ct, GObject *object, gpointer data);

/*
 * Iterate through 'socketdirs' and use the first directory that is accessible,
 * creating the parent directories before it.
 */
static char_u *
get_socket_dir(void)
{
    int		len	= (int)STRLEN(p_sd) + 1;
    char_u	*buf	= alloc(len);

    if (buf == NULL)
	return NULL;

    // Use runtimepath if 'socketdirs' is an empty string.
    char_u	*path	= NULL;
    char_u	*p	= p_sd;

    if (STRLEN(p) == 0)
	// No point in iterating through an empty string.
	return NULL;

    while (*p != NUL)
    {
	(void)copy_option_part(&p, buf, len, ",");

	// Expand the path incase of any shell/env symbols
	char_u *actual = expand_env_save(buf);

	// Attempt to create directory including any parents
	if (vim_mkdir_parents(actual, 0755) == OK)
	{
	    path = actual;
	    goto exit;
	}

	vim_free(actual);
    }

exit:
    vim_free(buf);

    return path;
}

/*
 * Create GSocketService object and make Vim ready to accept clients.
 */
    int
vgio_setup_socket_service(const char_u *name)
{
    // If "name" has a path separator in it, assume it is a path to the socket.
    // Otherwise automatically create a common directory to place sockets in.
    GError *error = NULL;
    stat_T st;

    if (vim_strchr((char_u *)name, '/') != NULL)
	socket_path = (char *)vim_strsave((char_u *)name);

    if (socket_path == NULL)
    {
	char *socket_dir = (char *)get_socket_dir();

	if (socket_dir == NULL)
	{
	    emsg(_(e_no_socket_dir_available));
	    return FAIL;
	}
	socket_path = g_strdup_printf("%s/%s", socket_dir, name);
	vim_free(socket_dir);
    }

    // Check if socket already exists
    if (mch_stat(socket_path, &st) >= 0)
    {
	emsg(_(e_socket_path_already_exists));
	VIM_CLEAR(socket_path);
	return FAIL;
    }

    socket_service = g_socket_service_new();

    // Create local socket
    socket_address = g_unix_socket_address_new(socket_path);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(socket_service),
		socket_address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
		NULL, NULL, &error))
    {
	semsg("Failed creating socket %s: %s", socket_path, error->message);

	g_error_free(error);
	g_object_unref(socket_address);

	VIM_CLEAR(socket_path);
	return FAIL;
    }



    g_signal_connect(socket_service, "incoming",
	    G_CALLBACK(handle_signal_incoming), NULL);

    return OK;
}

/*
 * Remove socket for Vim instance if it exists and unref objects.
 */
    void
vgio_takedown_socket_service(void)
{
    if (socket_address != NULL)
	g_object_unref(socket_address);
    if (socket_service != NULL)
	g_object_unref(socket_service);

    if (socket_path != NULL)
    {
	mch_remove(socket_path);
	VIM_CLEAR(socket_path);
    }
}

static gboolean
handle_signal_incoming(
	GSocketService *service UNUSED,
	GSocketConnection *ct UNUSED,
	GObject *object UNUSED,
	gpointer data UNUSED)
{
    return TRUE;
}

#endif	// FEAT_CLIENTSERVER
