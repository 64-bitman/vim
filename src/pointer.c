/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * pointer.c: functions that deal with a pointer object
 */
#include "vim.h"

#ifdef FEAT_EVAL

/*
 * Create a new custom object, returns NULL on failure. "free_func" may be NULL.
 * Caller should take care of reference count.
 */
    pointer_T *
pointer_new(void *ptr, char_u *type, pointer_free_func_T free_func)
{
    pointer_T *pr = ALLOC_ONE(pointer_T);

    if (pr == NULL)
	return NULL;

    pr->pr_type = vim_strsave(type);
    if (pr->pr_type == NULL)
    {
	vim_free(pr);
	return NULL;
    }

    pr->pr_refcount = 0;
    pr->pr_ptr = ptr;
    pr->pr_free_func = free_func;

    return pr;
}

    void
pointer_free(pointer_T *pr)
{
    if (pr->pr_free_func != NULL)
	pr->pr_free_func(pr);
    vim_free(pr->pr_type);
    vim_free(pr);
}

    void
pointer_unref(pointer_T *pr)
{
    if (--pr->pr_refcount <= 0)
	pointer_free(pr);
}

#endif
