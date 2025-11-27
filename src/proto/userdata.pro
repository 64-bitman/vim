/* userdata.c */
userdata_T *userdata_new(void *ptr, char_u *type);
void rettv_userdata_set(typval_T *rettv, userdata_T *udata);
void userdata_free(userdata_T *udata);
void userdata_unref(userdata_T *udata);
/* vim: set ft=c : */
