/* socketserver.c */
int socketserver_start(char_u *given_address, bool channel_addr);
void socketserver_stop(void);
int set_ref_in_socketserver_channel(int copyID);
int socketserver_send(char_u *name, char_u *str, char_u **result, char_u **receiver, int is_expr, int timeout, int silent);
/* vim: set ft=c : */
