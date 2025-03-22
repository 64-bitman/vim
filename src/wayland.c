/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * wayland.c: Functions to handle wayland related tasks
 */


#include "vim.h"
#include "wlr-data-control-unstable-v1.h"
#include <wayland-client.h>
#include <poll.h>

// Core wayland global objects
static struct wl_display *vwl_display;
static struct wl_registry *vwl_registry;
static int vwl_registry_listened; // if we are listening to the registry

// Global objects
static struct wl_seat *vwl_seat;
static struct zwlr_data_control_manager_v1 *vzwlr_control_manager;

// We need numerical name in order to destory object when
// the wl_registry global_remove event is called
static uint32_t vwl_seat_name, vzwlr_control_manager_name;

// display file descriptor
static int vwl_display_fd;

// Data control protocol to use per state
enum da_protocol_T {
    PROTOCOL_ZWLR_DATA_CONTROL_V1,
    PROTOCOL_EXT_DATA_CONTROL_V1
};

// Struct that contains the state for a selection or source operation
typedef struct
{
    const char *mimetype; // current mime type
    enum vwl_sel_type_T sel_type; // current selection type (primary or clipboard)

    // mime types to listen for
    struct {
	const char **list;
	int len; // length of list
	int found; // # of mime types we have found
    } mimes;

    // per state objects
    union {
	struct zwlr_data_control_offer_v1 *zwlr;
    } offer;
    union {
	struct zwlr_data_control_device_v1 *zwlr;
    } device;
    union {
	struct zwlr_data_control_source_v1 *zwlr;
    } source;

    // should not be destroyed, is just a pointer to the global
    union {
	struct zwlr_data_control_manager_v1 **zwlr;
    } manager;

    // da = data protocol
    enum da_protocol_T protocol;

    // callback to use when we receive data from a selection
    vwl_recv_callback_T recv_callback;
} vwl_state_T;

static char *mime_plain_utf8 = "text/plain;charset=utf-8";

static vwl_state_T vwl_create_state(void);
static int vwl_prepare_selection(vwl_state_T *state);
static int vzwlr_prepare_selection(vwl_state_T *state);
static void vzwlr_free_selection(vwl_state_T *state);
static int vwl_free_selection(vwl_state_T *state);

static void vwl_registry_listener_global(void *data,
	struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version);
static void vwl_registry_listener_global_remove(void *data,
	struct wl_registry *registry, uint32_t name);

static void vzwlr_offer_control_offer(void *data,
	struct zwlr_data_control_offer_v1 *offer, const char *mime);
static void vzwlr_device_control_data_offer(void *data,
	struct zwlr_data_control_device_v1 *device,
	struct zwlr_data_control_offer_v1 *offer);
static void vzwlr_device_control_selection(void *data,
	struct zwlr_data_control_device_v1 *device,
	struct zwlr_data_control_offer_v1 *offer);
static void vzwlr_device_control_primary_selection(void *data,
	struct zwlr_data_control_device_v1 *device,
	struct zwlr_data_control_offer_v1 *offer);
static void vzwlr_selection_handler(struct zwlr_data_control_offer_v1 *offer,
	vwl_state_T *state);
static void vzwlr_device_control_finished(void *data,
	struct zwlr_data_control_device_v1 *device);
static void data_control_receive(vwl_state_T *state);

static struct wl_registry_listener vwl_registry_listener = {
    .global = vwl_registry_listener_global,
    .global_remove = vwl_registry_listener_global_remove
};
// Order of events for zwlr_data_control_device:
// 1. The data_offer event is sent, which provides the offer object
// 2. The offer object is sent immediately after, giving the mime type
// 3. Then either the selection or primary_selection event is sent,
//    which are sent when we have a new selection in the respective selection
static struct zwlr_data_control_device_v1_listener vzwlr_device_control_listener = {
    .data_offer = vzwlr_device_control_data_offer,
    .selection = vzwlr_device_control_selection,
    .primary_selection = vzwlr_device_control_primary_selection,
    .finished = vzwlr_device_control_finished,
};

static struct zwlr_data_control_offer_v1_listener vzwlr_offer_control_listener = {
    .offer = vzwlr_offer_control_offer
};

/*
 * Connect to the wayland display if we aren't and get the registry if we haven't
 * in order to receive available global objects. Returns FAIL on failure and
 * OK on success
 */
    static int
vwl_attempt_global_init()
{
    if (vwl_display == NULL)
    {
	    vwl_display = wl_display_connect(NULL);
	    ch_log(NULL, "connected to wayland display");

	    if (vwl_display == NULL)
		return FAIL;
	    vwl_display_fd = wl_display_get_fd(vwl_display);
    }

    if (vwl_registry == NULL)
    {
	vwl_registry = wl_display_get_registry(vwl_display);
	ch_log(NULL, "got registry from wayland display");

	if (vwl_registry == NULL)
	    return FAIL;
    }

    if (!vwl_registry_listened)
    {
	if (wl_registry_add_listener(vwl_registry,
		    &vwl_registry_listener, NULL) == -1)
	    return FAIL;
	else
	    vwl_registry_listened = TRUE;
    }

    // Send the above requests to the compositor and block until
    // it has processed them all
    if (wl_display_roundtrip(vwl_display) == -1)
	return FAIL;

    // Return error if we haven't received the required objects,
    // such as when the compositor doesn't support it.
    if (vwl_seat == NULL)
	return FAIL;

    if (vzwlr_control_manager == NULL)
    {
	wl_seat_destroy(vwl_seat);
	vwl_seat = NULL;
	return FAIL;
    }

    return OK;
}

/*
 * Initialize a state struct
 */
    static vwl_state_T
vwl_create_state(void)
{
    return (vwl_state_T){0};
}

/*
 * Choose the data control protocol to use and then get the respective
 * device in order to listen events that it sends. wl_display_roundtrip
 * should be called after this in order to let the compositor process events.
 * Returns FAIL on failure * and OK on success.
 */
    static int
vwl_prepare_selection(vwl_state_T *state)
{
    if (vwl_attempt_global_init() == FAIL)
	return FAIL;

    if (vzwlr_control_manager != NULL)
	state->protocol = PROTOCOL_ZWLR_DATA_CONTROL_V1;

    if (state->protocol == PROTOCOL_ZWLR_DATA_CONTROL_V1)
    {
	ch_log(NULL, "using zwlr-data-control-v1 protocol");
	if (vzwlr_control_manager == NULL)
	    return FAIL;

	if (state->manager.zwlr == NULL)
	    state->manager.zwlr = &vzwlr_control_manager;
	return vzwlr_prepare_selection(state);
    }
    else
	return FAIL;
}

/*
 * Prepare the selection using the wlr_data_control_v1 protocol.
 * Returns FAIL on failure and OK on success.
 */
    static int
vzwlr_prepare_selection(vwl_state_T *state)
{
    if (state->device.zwlr != NULL)
	return OK;

    state->device.zwlr = zwlr_data_control_manager_v1_get_data_device(
	    vzwlr_control_manager, vwl_seat);

    if (state->device.zwlr == NULL)
	return FAIL;
    ch_log(NULL, "got data device");

    // Compositor will send a selection as soon as we bind
    if (zwlr_data_control_device_v1_add_listener(
		state->device.zwlr, &vzwlr_device_control_listener, state) == -1)
	return FAIL;

    ch_log(NULL, "listening data device");

    return OK;
}

/*
 * Clean up after we have done what we needed with the selection
 */
    static int
vwl_free_selection(vwl_state_T *state)
{
    if (state->protocol == PROTOCOL_ZWLR_DATA_CONTROL_V1)
	vzwlr_free_selection(state);

    // Both the client and compositor both need to agree in order
    // to perform a request, in this case destroying objects.
    if (wl_display_roundtrip(vwl_display) == -1)
	return FAIL;
    return OK;
}

    static void
vzwlr_free_selection(vwl_state_T *state)
{
    if (state->device.zwlr != NULL)
	zwlr_data_control_device_v1_destroy(state->device.zwlr);

    if (state->source.zwlr != NULL)
	zwlr_data_control_source_v1_destroy(state->source.zwlr);
}

/*
    Bind global objects to their respective interface, specifically wl_seat and
    zwlr_data_control_manager.
 */
static void
vwl_registry_listener_global(void *data,
                             struct wl_registry *registry,
                             uint32_t name,
                             const char *interface,
                             uint32_t version)
{
    if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0 &&
        version == (uint32_t)zwlr_data_control_manager_v1_interface.version &&
	vzwlr_control_manager == NULL)
    {
        vzwlr_control_manager =
            wl_registry_bind(vwl_registry, name,
                             &zwlr_data_control_manager_v1_interface, version);

        if (vzwlr_control_manager == NULL)
	    return;
	vzwlr_control_manager_name = name;
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0 &&
             version == (uint32_t)wl_seat_interface.version &&
	     vwl_seat == NULL)
    {
        vwl_seat =
            wl_registry_bind(vwl_registry, name, &wl_seat_interface, version);

        if (vwl_seat == NULL)
	    return;
	vwl_seat_name = name;
    }
    else
	return;
    ch_log(NULL, "received global object %s %u", interface, version);
}

/*
 * Destory global objects when they no longer become available
 */
    static void
vwl_registry_listener_global_remove(void *data,
                                    struct wl_registry *registry,
                                    uint32_t name)
{
    if (name == vzwlr_control_manager_name)
    {
	ch_log(NULL, "destroying data control manager");
	zwlr_data_control_manager_v1_destroy(vzwlr_control_manager);
	vzwlr_control_manager = NULL;
    }
    else if (name == vwl_seat_name)
    {
	ch_log(NULL, "destroying seat");
	wl_seat_destroy(vwl_seat);
	vwl_seat = NULL;
    }
}

/*
 * Called immediately after data_offer event, set the mime type.
 */
    static void
vzwlr_offer_control_offer(void *data,
	struct zwlr_data_control_offer_v1 *offer, const char *mime)
{
    vwl_state_T *state = data;

    // We already are processing an offer or have found all the 
    // mime types we want, so skip this.
    if (state->offer.zwlr != NULL || state->mimes.found == state->mimes.len)
	return;

    for (int i = 0; i < state->mimes.len; i++)
    {
	if (STRCMP(mime, state->mimes.list[i]) == 0)
	{
	    state->mimetype = state->mimes.list[i];
	    state->offer.zwlr = offer;
	    state->mimes.found++;
	    ch_log(NULL, "received offer of type %s", mime);
	    return;
	}
    }
}

/*
 * When the data_offer event is sent. Handle it by then listening for the offer
 * event that the data_offer sends immediately after.
 */
    static void
vzwlr_device_control_data_offer(void *data,
	struct zwlr_data_control_device_v1 *device,
	struct zwlr_data_control_offer_v1 *offer)
{
    if (offer == NULL)
	return;

    zwlr_data_control_offer_v1_add_listener(offer,
		&vzwlr_offer_control_listener, data);
}

/*
 * Called when compositor receives a new selection.
 */
    static void
vzwlr_device_control_selection(void *data,
	struct zwlr_data_control_device_v1 *device,
	struct zwlr_data_control_offer_v1 *offer)
{
    ((vwl_state_T *)data)->sel_type = VWL_SEL_CLIPBOARD;
    vzwlr_selection_handler(offer, data);
}

/*
 * Called when compositor receives a new primary selection.
 */
    static void
vzwlr_device_control_primary_selection(void *data,
	struct zwlr_data_control_device_v1 *device,
	struct zwlr_data_control_offer_v1 *offer)
{
    ((vwl_state_T *)data)->sel_type = VWL_SEL_PRIMARY;
    vzwlr_selection_handler(offer, data);
}

/*
 * General purpose handler for selection events.
 */
    static void
vzwlr_selection_handler(struct zwlr_data_control_offer_v1 *offer,
	vwl_state_T *state)
{
    if (offer == NULL)
	return;

    // only receive from the offer we have selected
    if (state->offer.zwlr == offer)
    {
	data_control_receive(state);
	ch_log(NULL, "got selection");

	state->offer.zwlr = NULL;
    }
    zwlr_data_control_offer_v1_destroy(offer);
}

/*
 * Device object is no longer valid, destroy it.
 */
    static void
vzwlr_device_control_finished(void *data,
	struct zwlr_data_control_device_v1 *device)
{
    ch_log(NULL, "finished");
    vwl_state_T *state = data;

    zwlr_data_control_device_v1_destroy(state->device.zwlr);
    state->device.zwlr = NULL;
}

/*
 * Receive data given by an offer in the encoding that we chose when
 * we got the mime type in the offer event. This data is piped into
 * a fifo which is given (a read only non blocking fd) to the callback
 * function. No need to worry about pipe becoming full and then block,
 * IO is non blocking.
 */
    static void
data_control_receive(vwl_state_T *state)
{
    int pipefds[2], ret, flags;

    if (pipe(pipefds) == -1)
	return;

    zwlr_data_control_offer_v1_receive(state->offer.zwlr,
	    state->mimetype, pipefds[1]);

    struct pollfd fds = {
	.fd = vwl_display_fd,
	.events = POLLOUT
    };

    // Send requests to compositor, difference from wl_display_roundtrip
    // is that it doesn't block. Will try to write as much data, and if there
    // is data that has not been written, errno is set to EAGAIN and -1 is
    // returned. Therefore, we should poll the display fd until it is
    // writable again.
    while (errno = 0, wl_display_flush(vwl_display) == -1 && errno == EAGAIN)
	// abort if we have an error or we have timed out
	if ((ret = poll(&fds, 1, 2000)) == -1 || ret == 0)
	    goto close_fds;
    wl_display_flush(vwl_display);

    // exit if other error occured other than EAGAIN
    if (errno != 0)
	goto close_fds;

    // call callback to read from pipe
    state->recv_callback(pipefds[0], state->sel_type, state->mimetype);

close_fds:
    close(pipefds[0]);
    close(pipefds[1]);
}

/*
 * Get either clipboard or primary selection, and send data to callback in the
 * form of a pipe. Callback will be called for each mime type specified (if
 * it exists). Returns FAIL on failure and OK on success.
 */
    int
vwl_clip_get_selection(vwl_recv_callback_T callback, const char **mime_arr, int mlen)
{
    vwl_state_T state = vwl_create_state();

    state.recv_callback = callback;
    state.mimes.list = mime_arr;
    state.mimes.len = mlen;

    if (vwl_prepare_selection(&state) == FAIL)
	return FAIL;

    // prepare doesn't send requests to compositor, we need to do it outselves
    if (wl_display_roundtrip(vwl_display) == -1)
	return FAIL;

    // destroy device and other things
    return vwl_free_selection(&state);
}
