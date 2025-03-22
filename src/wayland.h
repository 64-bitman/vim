/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

enum vwl_sel_type_T {
    VWL_SEL_CLIPBOARD,
    VWL_SEL_PRIMARY
};

// callback function used when reading data from selection
typedef void (*vwl_recv_callback_T)(int fd, enum vwl_sel_type_T sel_type,
	const char *mime_type);
