/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * userdata.c: Internal immutable data type that stores a pointer with a given
 * type. Equivalent to Lua userdata values.
 */

#include "vim.h"

#ifdef FEAT_EVAL

/*
 * Allocate new userdata value. "type" should be statically allocated and "ptr"
 * must not be NULL.
 */
userdata_T *
userdata_new(void *ptr, char_u *type)
{
    userdata_T *udata = ALLOC_ONE(userdata_T);

    udata->ud_ptr = ptr;
    udata->ud_type = type;
    udata->ud_refcount = 0;
    udata->ud_free_func = NULL;

    return udata;
}

/*
 * Set a userdata as the return value
 */
    void
rettv_userdata_set(typval_T *rettv, userdata_T *udata)
{
    rettv->v_type = VAR_USERDATA;
    rettv->vval.v_userdata = udata;
    if (udata != NULL)
	udata->ud_refcount++;
}

    void
userdata_free(userdata_T *udata)
{
    if (udata->ud_free_func != NULL)
	udata->ud_free_func(udata->ud_ptr);
    vim_free(udata);
}

/*
 * Unreference userdata and free it when refcount is zero
 */
    void
userdata_unref(userdata_T *udata)
{
    if (udata != NULL && --udata->ud_refcount <= 0)
	userdata_free(udata);
}
    
#endif
