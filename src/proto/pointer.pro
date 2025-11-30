/* pointer.c */
pointer_T *pointer_new(void *ptr, char_u *type, pointer_free_func_T free_func);
void pointer_free(pointer_T *pr);
void pointer_unref(pointer_T *pr);
/* vim: set ft=c : */
