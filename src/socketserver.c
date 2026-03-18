/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * socketserver.c: Clientserver functionality using channels
 */

#include "vim.h"

#ifdef FEAT_SOCKETSERVER

# include <netinet/in.h>

# define SS_MAX_MSG 10
# define SS_MAX_MESSAGE_PAYLOAD 16384
# define PROTOCOL_VERSION 1

typedef enum {
    SS_MESSAGE_TYPE_EMPTY	= 0,	// Message not set
    SS_MESSAGE_TYPE_ENCODING    = 'e',  // Encoding of message.
    SS_MESSAGE_TYPE_STRING	= 'c',  // Script to execute or reply string.
    SS_MESSAGE_TYPE_SERIAL	= 's',  // Serial of pending command
    SS_MESSAGE_TYPE_CODE	= 'r',  // Result code for an expression sent
    SS_MESSAGE_TYPE_SENDER	= 'd',  // Location of socket for the client
					// that sent the command
    SS_MESSAGE_TYPE_VERSION	= 'v'	// Version of protocol
} ss_message_type_T;

# define MSG_TYPE_VALID(type) \
    (type == SS_MESSAGE_TYPE_ENCODING || type == SS_MESSAGE_TYPE_STRING || \
     type == SS_MESSAGE_TYPE_SERIAL || type == SS_MESSAGE_TYPE_CODE || \
     type == SS_MESSAGE_TYPE_SENDER)

typedef enum {
    SS_COMMAND_TYPE_EXPR	= 'E',  // An expression
    SS_COMMAND_TYPE_KEYSTROKES  = 'K',  // Series of keystrokes
    SS_COMMAND_TYPE_REPLY	= 'R',  // Reply from an expression
    SS_COMMAND_TYPE_NOTIFY	= 'N',  // A notification
    SS_COMMAND_TYPE_ALIVE	= 'A',  // Check if server is still responsive
					// (DEPRECATED)
} ss_command_type_T;

# define CMD_TYPE_VALID(type) \
    (type == SS_COMMAND_TYPE_EXPR || type == SS_COMMAND_TYPE_KEYSTROKES || \
     type == SS_COMMAND_TYPE_REPLY || type == SS_COMMAND_TYPE_NOTIFY || \
     type == SS_COMMAND_TYPE_ALIVE)

typedef struct
{
    char_u	msg_type;	// Type of message
    uint32_t	msg_len;	// Total length of contents
    char_u	*msg_contents;	// Message contents
} ss_message_T;

typedef struct {
    char_u	    cmd_type;		    // Type of command
    uint32_t	    cmd_num;		    // Number of messages
    uint32_t	    cmd_len;		    // Combined size of all
					    // messages
    ss_message_T    cmd_msgs[SS_MAX_MSG];   // Array of messages
} ss_command_T;

/*
 * Represents a pending reply from a command sent to another Vim server. When a
 * command is sent out, we generate unique serial number with it. When we
 * receive any reply, we check which pending command has a matching serial
 * number, and is therefore the reply for that pending command.
 */
typedef struct ss_pending_command_S ss_pending_command_T;
struct ss_pending_command_S {
    uint32_t		    p_serial;	// Serial number expected in result
    char_u		    p_code;	// Result code, can be 0 or -1.
    char_u		    *p_result;	// Result of command
    ss_pending_command_T    *p_next;
};

// The enums are in the order they will be called
typedef enum
{
    SS_STATE_EMPTY = 0,	// Nothing set
    SS_STATE_TYPE,	// cmd_type set
    SS_STATE_NUM,	// cmd_num set
    SS_STATE_LEN,	// cmd_len set

    SS_STATE_MSGTYPE,	// msg_type set
    SS_STATE_MSGLEN,	// msg_len set
    SS_STATE_MSGDATA,	// msg_contents set

    SS_STATE_FINISHED,	// Received all messages
} ss_state_T;

// Represents a connection to a client.
typedef struct ss_connection_S ss_connection_T;
struct ss_connection_S
{
    channel_T	    *channel;

    ss_state_T	    state;
    ss_command_T    command; // Current command that we are building, completed
			     // when "stage" is SS_STAGE_FINISHED

    // An ss_message_T is the largest single entity we can received.
    char_u	    buf[512];
    int		    buflen;

    garray_T	    msgdata;
    int		    n_msgs_received;

    ss_connection_T *next;
    ss_connection_T *prev;
};

#define FOR_ALL_SS_CONNECTIONS(ct) for (ct = socketserver_connections; \
	ct != NULL; ct = ct->next)

// Channel used by Vim to act as a server. If we are not a server, then this is
// NULL.
static channel_T	*socketserver_channel = NULL;
static char_u		*socketserver_address = NULL;
static bool		socketserver_is_unix = false;
static ss_connection_T  *socketserver_connections = NULL;

// Always greater than zero
static int		    socketserver_serial = 0;
static ss_pending_command_T *socketserver_pending = NULL;

static void socketserver_callback(channel_T *channel);
    static void socketserver_close(channel_T *channel);

/*
 * Initialize the socketserver at the given address, following channel address
 * naming conventions if "channel_addr" is true. If the address starts with "./"
 * or "../" or "/", then it is taken as a path (Unix domain socket address),
 * only on Unix. Returns OK on success and FAIL on failure.
 */
    int
socketserver_start(char_u *address, bool channel_addr)
{
    channel_T	*channel;
    bool	got_addr = false;
    bool	is_unix = false;

    address = vim_strsave(address);
    if (address == NULL)
    {
	emsg(_(e_out_of_memory));
	return FAIL;
    }

    if (channel_addr)
    {
	int port;

	if (channel_parse_address(&address, &port, true) == FAIL)
	    goto fail;

	if (port == -1)
	    channel = channel_listen_unix((char *)address, NULL);
	else
	    channel = channel_listen((char *)address, port, NULL);
	is_unix = port == -1;
	got_addr = true;
    }

    if (!got_addr && (STRNCMP(address, "./", 2) == 0
		|| STRNCMP(address, "../", 3) == 0
		|| STRNCMP(address, "/", 1) == 0))
    {
	channel = channel_listen_unix((char *)address, NULL);
	got_addr = true;
	is_unix = true;
    }

    if (!got_addr)
    {
	semsg(_(e_invalid_argument_str), address);
	goto fail;
    }

    if (channel == NULL)
	goto fail;

    socketserver_is_unix = is_unix;

    channel->ch_listen_callback = socketserver_callback;
    channel->ch_close_callback = socketserver_close;
    socketserver_channel = channel;
    socketserver_address = address;

    return OK;
fail:
    vim_free(address);
    return FAIL;
}

/*
 * Stop running the socketserver if it is and cleanup.
 */
    void
socketserver_stop(void)
{
    channel_T *channel = socketserver_channel;

    if (channel == NULL)
	return;

    if (channel->ch_port == 0)
	// Unix domain socket, remove socket file
	mch_remove(socketserver_address);

    channel_close(channel, false);
    channel_clear(channel);

    while (socketserver_connections != NULL)
    {
	// Close callback will remove it from list
	channel_close(socketserver_connections->channel, true);
	channel_clear(socketserver_connections->channel);
    }

    vim_free(socketserver_address);
    socketserver_channel = NULL;
}

    static void
ss_command_init(ss_command_T *cmd, ss_command_type_T type)
{
    cmd->cmd_len = 0;
    cmd->cmd_num = 0;
    cmd->cmd_type = type;
}

    static void
ss_command_clear(ss_command_T *cmd)
{
    for (uint32_t i = 0; i < cmd->cmd_num; i++)
    {
	ss_message_T *msg = cmd->cmd_msgs + i;

	vim_free(msg->msg_contents);
    }
}

/*
 * Append a message to a command. Note that "len" is the length of contents.
 * Returns OK on success and FAIL on failure
 */
    static int
ss_command_append(
	ss_command_T	*cmd,
	char_u		type,
	char_u		*contents,
	int		len)
{
    ss_message_T *msg = cmd->cmd_msgs + cmd->cmd_num;

    if (cmd->cmd_num >= SS_MAX_MSG)
    {
	iemsg("Internal limit for socketserver message count");
	return FAIL;
    }

    // Check if command will be too big.
    if (cmd->cmd_len + len > SS_MAX_MESSAGE_PAYLOAD)
	return FAIL;

    msg->msg_contents = alloc(len);

    if (msg->msg_contents == NULL)
	return FAIL;

    msg->msg_type = type;
    msg->msg_len = len;
    memcpy(msg->msg_contents, contents, len);

    cmd->cmd_len += sizeof(char_u) + sizeof(uint32_t) + len;
    cmd->cmd_num++;

    return OK;
}

/*
 * Serialize command struct and return the final message to send. Returns NULL
 * on failure.
 */
    static char_u *
ss_command_serialize(ss_command_T *cmd, size_t *sz)
{
    size_t size;
    char_u *buf;
    char_u *start;

    size = sizeof(char_u) + (sizeof(uint32_t) * 2) + cmd->cmd_len;
    buf = alloc(size);

    if (buf == NULL)
	return NULL;

    start = buf;
    memcpy(start, &cmd->cmd_type, sizeof(cmd->cmd_type));
    start += sizeof(cmd->cmd_type);
    memcpy(start, &cmd->cmd_num, sizeof(cmd->cmd_num));
    start += sizeof(cmd->cmd_num);
    memcpy(start, &cmd->cmd_len, sizeof(cmd->cmd_len));
    start += sizeof(cmd->cmd_len);

    // Append messages to buffer
    for (uint32_t i = 0; i < cmd->cmd_num; i++)
    {
	ss_message_T *msg = cmd->cmd_msgs + i;

	memcpy(start, &msg->msg_type, sizeof(msg->msg_type));
	start += sizeof(msg->msg_type);
	memcpy(start, &msg->msg_len, sizeof(msg->msg_len));
	start += sizeof(msg->msg_len);

	memcpy(start, msg->msg_contents, msg->msg_len);
	start += msg->msg_len;
    }

    *sz = size;

    return buf;
}

static void
set_channel_opts(channel_T *channel)
{
    channel->ch_drop_never = true;
    channel->ch_nonblock = true;

    for (ch_part_T part = PART_SOCK; part < PART_COUNT; ++part)
	channel->ch_part[part].ch_mode = CH_MODE_RAW;
}

/*
 * Execute a command
 */
    static void
socketserver_exec(ss_connection_T *ct, ss_command_T *cmd)
{
    char_u	    *str = NULL;
    char_u	    *enc = NULL;
    char_u	    *sender = NULL;
    uint32_t	    serial = 0;
    char_u	    rcode = 0;
    char_u	    *to_free;
    char_u	    *to_free2;
    char_u	    version = 0; // If version is not provided in command, then
				 // we know its the original socketserver
				 // implementation.

    for (uint32_t i = 0; i < cmd->cmd_num; i++)
    {
	ss_message_T *msg = cmd->cmd_msgs + i;

	switch (msg->msg_type)
	{
	    case SS_MESSAGE_TYPE_STRING:
		str = msg->msg_contents;
		break;
	    case SS_MESSAGE_TYPE_ENCODING:
		enc = msg->msg_contents;
		break;
	    case SS_MESSAGE_TYPE_SERIAL:
		memcpy(&serial, msg->msg_contents, sizeof(serial));
		break;
	    case SS_MESSAGE_TYPE_CODE:
		memcpy(&rcode, msg->msg_contents, sizeof(rcode));
		break;
	    case SS_MESSAGE_TYPE_SENDER:
		sender = msg->msg_contents;

		// Save in global
		vim_free(client_socket);
		client_socket = vim_strsave(sender);
		break;
	    case SS_MESSAGE_TYPE_VERSION:
		memcpy(&version, msg->msg_contents, sizeof(version));
		break;
	}
    }

    ch_log(NULL, "socket_server_exec_cmd(): encoding: %s, result: %s",
	    enc == NULL ? (char_u *)"(null)" : enc,
	    str == NULL ? (char_u *)"(null)" : str);

    if (cmd->cmd_type == SS_COMMAND_TYPE_EXPR
	    || cmd->cmd_type == SS_COMMAND_TYPE_KEYSTROKES)
    {
	// Either an expression or keystrokes.
	str = serverConvert(enc, str, &to_free);

	if (cmd->cmd_type == SS_COMMAND_TYPE_KEYSTROKES)
	    server_to_input_buf(str);
	else if (sender != NULL)
	{
	    // Evaluate expression and send reply containing result
	    char_u	    *result;
	    char_u	    code;
	    ss_command_T    rcmd;
	    size_t	    sz;
	    char_u	    *buf;
	    char_u	    send_ver = PROTOCOL_VERSION;

	    result = eval_client_expr_to_string(str);
	    code = result == NULL ? -1 : 0;

	    ss_command_init(&rcmd, SS_COMMAND_TYPE_REPLY);

	    if (result != NULL)
		ss_command_append(&rcmd, SS_MESSAGE_TYPE_STRING, result,
			STRLEN(result) + 1); // We add +1 in case "result"
					     // is an empty string.
	    else
		// An error occurred, return an error msg instead
		ss_command_append(&rcmd, SS_MESSAGE_TYPE_STRING,
			(char_u *)_(e_invalid_expression_received),
			STRLEN(e_invalid_expression_received));

	    ss_command_append(&rcmd, SS_MESSAGE_TYPE_CODE, &code, sizeof(code));
	    ss_command_append(&rcmd, SS_MESSAGE_TYPE_ENCODING, p_enc,
		    STRLEN(p_enc));
	    ss_command_append(&rcmd, SS_MESSAGE_TYPE_SERIAL,
		    (char_u *)&serial, sizeof(serial));

	    ss_command_append(&rcmd, SS_MESSAGE_TYPE_VERSION,
		    (char_u *)&send_ver, sizeof(send_ver));

	    buf = ss_command_serialize(&rcmd, &sz);

	    if (buf != NULL && version == 0)
	    {
		// Original socketserver implementation expects a new connection
		// to be made.
		channel_T *rchannel = channel_open_unix((char *)sender, NULL);

		if (rchannel != NULL)
		{
		    set_channel_opts(rchannel);

		    channel_send(rchannel, PART_SOCK, buf, sz,
			    "socketserver_exec");

		    channel_close(rchannel, false);
		    channel_clear(rchannel);
		}
	    }
	    else if (buf != NULL)
		channel_send(ct->channel, PART_SOCK, buf, (int)sz,
			"socketserver_exec");

	    ss_command_clear(&rcmd);
	}
    }
}

/*
 * Called when something has been read from a client connection.
 */
    static void
socketserver_read_callback(channel_T *channel, char_u *data, int size)
{
    ss_connection_T *ct = channel->ch_udata;
    ss_command_T    *cmd = &ct->command;
    bool	    abort = false;
    char_u	    *buf = ct->buf;
    bool	    dont_quit = false;

    // Consume "data" until we are done or if an error occured.
    while (size > 0 || ct->buflen > 0 || dont_quit)
    {
	int	take = MIN(sizeof(ct->buf) - ct->buflen, size);
	int	remove = 0;

	dont_quit = false;
	if (take > 0)
	{
	    memcpy(buf + ct->buflen, data, take);
	    ct->buflen += take;

	    data += take;
	    size -= take;
	}

	// State machine that interprets the data in the buffer depending on
	// which state it is in.
	switch (ct->state)
	{
	    case SS_STATE_EMPTY:
		// Read command type
		memcpy(&cmd->cmd_type, buf, sizeof(char_u));
		remove += sizeof(char_u);

		ct->state = SS_STATE_TYPE;
		if (!CMD_TYPE_VALID(cmd->cmd_type))
		    abort = true;
		break;
	    case SS_STATE_TYPE:
		if (ct->buflen < sizeof(uint32_t))
		    break;
		// Read number of messages
		memcpy(&cmd->cmd_num, buf, sizeof(uint32_t));
		remove += sizeof(uint32_t);

		// If server is using Unix domain socket, use native byte order.
		// Otherwise we must convert from network byte order.
		//
		// This is because the previous socketserver implementation
		// assumed native byte order (since it only supported domain
		// sockets). Must maintain backwards compatibility.
		if (!socketserver_is_unix)
		    cmd->cmd_num = ntohl(cmd->cmd_num);

		ct->state = SS_STATE_NUM;
		if (cmd->cmd_num > SS_MAX_MSG)
		    abort = true;
		break;
	    case SS_STATE_NUM:
		if (ct->buflen < sizeof(uint32_t))
		    break;
		// Read total length of all messages
		memcpy(&cmd->cmd_len, buf, sizeof(uint32_t));
		remove += sizeof(uint32_t);

		if (!socketserver_is_unix)
		    cmd->cmd_len = ntohl(cmd->cmd_len);

		ct->state = SS_STATE_LEN;
		if (cmd->cmd_len > SS_MAX_MESSAGE_PAYLOAD)
		    abort = true;
		break;
	    case SS_STATE_LEN:
	    {
		ss_message_T *msg = cmd->cmd_msgs + ct->n_msgs_received;

		// Read message type
		memcpy(&msg->msg_type, buf, sizeof(char_u));
		remove += sizeof(char_u);

		ct->state = SS_STATE_MSGTYPE;
		if (!MSG_TYPE_VALID(msg->msg_type))
		    abort = true;
		break;
	    }
	    case SS_STATE_MSGTYPE:
	    {
		if (ct->buflen < sizeof(uint32_t))
		    break;
		ss_message_T *msg = cmd->cmd_msgs + ct->n_msgs_received;

		// Read message length
		memcpy(&msg->msg_len, buf, sizeof(uint32_t));
		remove += sizeof(uint32_t);

		if (!socketserver_is_unix)
		    msg->msg_len = ntohl(msg->msg_len);

		ct->state = SS_STATE_MSGLEN;
		break;
	    }
	    case SS_STATE_MSGLEN:
	    {
		ss_message_T *msg = cmd->cmd_msgs + ct->n_msgs_received;
		int	     toread = msg->msg_len - ct->msgdata.ga_len;

		if (toread == 0)
		{
		    // Go onto next message
		    ct->state = SS_STATE_MSGDATA;
		    dont_quit = true;
		    break;
		}

		// Only read the data that we currently have.
		toread = MIN(toread, ct->buflen);

		if (ga_grow(&ct->msgdata, toread) == FAIL)
		    abort = true;
		else
		{
		    // Consume message contents
		    memcpy((char_u *)ct->msgdata.ga_data + ct->msgdata.ga_len,
			    buf, toread);
		    remove += toread;
		    ct->msgdata.ga_len += toread;
		    dont_quit = true;
		}

		break;
	    }
	    case SS_STATE_MSGDATA:
	    {
		ss_message_T *msg = cmd->cmd_msgs + ct->n_msgs_received++;

		msg->msg_contents = ct->msgdata.ga_data;
		ga_init(&ct->msgdata);

		// Finished, go onto next message. If there is no next message
		// we are finished.
		if (ct->n_msgs_received >= cmd->cmd_num)
		    ct->state = SS_STATE_FINISHED;
		else
		    ct->state = SS_STATE_LEN;
		dont_quit = true;
		break;
	    }
	    case SS_STATE_FINISHED:
		// Execute command
		socketserver_exec(ct, &ct->command);
		ss_command_clear(&ct->command);
		ct->state = SS_STATE_EMPTY;
		break;
	}

	if (abort)
	    break;
	if (size == 0 && remove == 0 && !dont_quit)
	    // Stop and wait for more data
	    break;
	if (remove == 0)
	    continue;

	// Consume the read data
	mch_memmove(buf, buf + remove, ct->buflen - remove);
	ct->buflen -= remove;
    }

    if (abort)
    {
	ch_log(NULL, "socketserver: corrupt command received");
	ga_clear(&ct->msgdata);
	ct->buflen = 0;
    }
}

/*
 * Called when connection to client is closed.
 */
    static void
socketserver_connection_close(channel_T *channel)
{
    ss_connection_T *ct = channel->ch_udata;

    if (ct == NULL)
    {
	// Not sure if this can happen, but better to be safe
        ch_log(NULL, "socketserver: socketserver_connection_close() called "
		"more than once?");
        return;
    }

    ch_log(NULL, "socketserver: client connection closed");

    if (ct->state != SS_STATE_EMPTY)
	ss_command_clear(&ct->command);

    // Unlink from global list
    if (ct->prev != NULL)
	ct->prev->next = ct->next;
    if (ct->next != NULL)
	ct->next->prev = ct->prev;

    ga_clear(&ct->msgdata);
    vim_free(ct);
    channel->ch_udata = NULL;
}

/*
 * Called when new client accepted.
 */
    static void
socketserver_callback(channel_T *channel)
{
    ss_connection_T *ct = ALLOC_CLEAR_ONE(ss_connection_T);

    if (ct == NULL)
    {
	channel_close(channel, true);
	channel_clear(channel);
	return;
    }

    ch_log(NULL, "socketserver: accepted new client at address %s",
	    socketserver_address);

    set_channel_opts(channel);
    channel->ch_udata = ct;

    channel->ch_read_callback = socketserver_read_callback;
    channel->ch_close_callback = socketserver_connection_close;

    ct->channel = channel;
    ga_init2(&ct->msgdata, 1, 64);

    if (socketserver_connections != NULL)
	socketserver_connections->prev = ct;
    ct->next = socketserver_connections;
    ct->prev = NULL;
    socketserver_connections = ct->next;
}

/*
 * Called when server goes down
 */
    static void
socketserver_close(channel_T *channel)
{
    ch_log(NULL, "socketserver: server down");
    socketserver_stop();
}

/*
 * Mark all channels used by socketserver as referenced so they don't get
 * garbage collected.
 */
    int
set_ref_in_socketserver_channel(int copyID)
{
    ss_connection_T *ct;
    bool	    abort = false;
    typval_T	    tv;

    if (socketserver_channel != NULL)
    {
	tv.v_type = VAR_CHANNEL;
	tv.vval.v_channel = socketserver_channel;

	abort = set_ref_in_item(&tv, copyID, NULL, NULL, NULL);
    }

    FOR_ALL_SS_CONNECTIONS(ct)
    {
	tv.v_type = VAR_CHANNEL;
	tv.vval.v_channel = ct->channel;

	abort = abort || set_ref_in_item(&tv, copyID, NULL, NULL, NULL);
    }
    return abort;
}

/*
 * Poll all socketserver channels and handle them until "target" finally has
 * read something, or if "timeout" milliseconds is reached. Returns true on
 * success.
 */
    static bool
socketserver_wait(channel_T *target, int timeout)
{
    if (target->CH_SOCK_FD == INVALID_FD)
	return false;

    return false;
}

/*
 * Send command to socket named "name". Returns 0 for OK, -1 on error.
 */
    int
socketserver_send(
	char_u *name,	    // Socket path or a general name
	char_u *str,	    // What to send
	char_u **result,    // Set to result of expr
	char_u **receiver,  // Full path of "name"
	int is_expr,	    // Is it an expression or keystrokes?
	int timeout,	    // In milliseconds
	int silent)	    // Don't complain if socket doesn't exist
{


    return 0;
}

#endif // FEAT_SOCKETSERVER
