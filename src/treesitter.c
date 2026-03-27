/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * treesitter.c: stuff for treesitter integration
 */

#include "vim.h"

#ifdef FEAT_TREESITTER

#ifdef MSWIN
# define load_dll vimLoadLib
# define symbol_from_dll GetProcAddress
# define close_dll FreeLibrary
# define load_dll_error GetWin32Error
#else
# include <dlfcn.h>
# define HANDLE void*
# define load_dll(n) dlopen((n), RTLD_LAZY)
# define symbol_from_dll dlsym
# define close_dll dlclose
# define load_dll_error dlerror
#endif

/*
 * Represents Treesitter language library
 */
typedef struct treesitter_lang_S treesitter_lang_T;
struct treesitter_lang_S
{
    HANDLE		tl_handle;  // Library handle
    TSLanguage		*tl_obj;    // Opaque object
    treesitter_lang_T	*tl_next;   // Next in list
    char_u		tl_name[1]; // Actually longer (language name)
};

static treesitter_lang_T *treesitter_langs = NULL;
#define FOR_ALL_LANGS(l) for (l = treesitter_langs; l != NULL; l = l->tl_next)

// List of language trees that are waiting for their moment to have any invalid
// regions parsed.
static languagetree_T	*trees_to_parse = NULL;

/*
 * Vim allocator functions for treesitter
 */
    static void *
treesitter_calloc(size_t n, size_t size)
{
    return alloc_clear(n * size);
}

    static void *
treesitter_realloc(void *ptr, size_t size)
{
    return vim_realloc(ptr, size);
}

/*
 * Initialize some Treesitter stuff
 */
    void
treesitter_init(void)
{
    ts_set_allocator(alloc, treesitter_calloc, treesitter_realloc, vim_free);
}

/*
 * Load the Treesitter library at the given path, and return the language object
 * + store the handle in "handle". Returns NULL on failure.
 */
    static TSLanguage *
treesitter_load_language(char *path, char *symbol, HANDLE *handle)
{
    HANDLE	h = load_dll(path);
    TSLanguage	*lang;
    TSLanguage	*(*func)(void);
    static char	buf[255];

    if (h == NULL)
    {
	semsg(_(e_could_not_load_library_str_str), path, load_dll_error());
	return NULL;
    }

    vim_snprintf(buf, sizeof(buf), "tree_sitter_%s", symbol);
    func = symbol_from_dll(h, buf);
    if (func == NULL)
    {
	semsg(_(e_could_not_load_library_function_str), buf);
	close_dll(h);
	return NULL;
    }

    lang = func();
    if (lang == NULL)
    {
	semsg(_(e_treesitter_get_lang_error), path);
	close_dll(h);
	return NULL;
    }

    *handle = h;
    return lang;
}

/*
 * Return a string containing the path to the first found Treesitter library for
 * "lang". Note that string is only valid until the next function call. Return
 * NULL if none found.
 */
    static char_u *
treesitter_find_library(char_u *lang)
{
    static char	    *paths[] = {
	"/usr/lib/tree_sitter"
    };
    static char_u   buf[MAXPATHL];
    bool	    found = false;

    for (int i = 0; i < ARRAY_LENGTH(paths); i++)
    {
	char_u *path = (char_u *)paths[i];

	vim_snprintf((char *)buf, MAXPATHL, "%s/%s.so", path, lang);
	
	if (mch_isrealdir(buf))
	{
	    found = true;
	    break;
	}
    }
    if (found)
    {
	semsg(_(e_treesitter_lang_not_found), lang);
	return buf;
    }
    return NULL;
}

/*
 * If the Treesitter language is not load it, then load it. If it is already
 * loaded, then do nothing. If "symbol_name" is NULL, then "name" is used as the
 * symbol name to lookup. If "path" is NULL, then treesitter_find_library() is
 * used. Returnss the TSLanguage object on success and NULL on failure.
 */
    static TSLanguage *
treesitter_check_language(char_u *name, char_u *path, char_u *symbol_name)
{
    TSLanguage		*obj;
    treesitter_lang_T	*lang;
    HANDLE		handle;

    if (path == NULL)
	path = treesitter_find_library(name);
    if (path == NULL)
	return FAIL;

    // Check if language is loaded
    FOR_ALL_LANGS(lang)
	if (STRCMP(lang->tl_name, name) == 0)
	    return lang->tl_obj;

    lang = alloc(sizeof(treesitter_lang_T) + STRLEN(name));
    if (lang == NULL)
	return FAIL;

    obj = treesitter_load_language((char *)path,
	    (char *)(symbol_name == NULL ? name : symbol_name), &handle);

    if (obj == NULL)
    {
	vim_free(lang);
	return FAIL;
    }

    lang->tl_handle = handle;
    lang->tl_obj = obj;
    sprintf((char *)lang->tl_name, "%s", name);

    lang->tl_next = treesitter_langs;
    treesitter_langs = lang;

    return obj;
}

/*
 * Allocate a new empty language tree for the buffer with the language being
 * "lang". Returns OK on success and FAIL on failure.
 */
    int
languagetree_new(buf_T *buf, char_u *lang)
{
    TSLanguage	    *langobj;
    languagetree_T  *lt;

    langobj = treesitter_check_language(lang, NULL, NULL);

    if (langobj == NULL)
	return FAIL;

    lt = ALLOC_CLEAR_ONE(languagetree_T);

    if (lt == NULL)
	return FAIL;

    lt->lt_name = vim_strsave(lang);
    if (lt->lt_name == NULL)
	goto fail;

    lt->lt_parser = ts_parser_new();
    if (lt->lt_parser == NULL)
	goto fail;

    ts_parser_set_language(lt->lt_parser, langobj);
    lt->lt_buf = buf;

    return OK;
fail:
    languagetree_free(lt);
    return FAIL;
}

/*
 * Free language tree and all child language trees as well.
 */
    void
languagetree_free(languagetree_T *lt)
{
    tree_T	    *tr;

    vim_free(lt->lt_name);
    if (lt->lt_parser != NULL)
	ts_parser_delete(lt->lt_parser);

    for (tr = lt->lt_trees; tr != ((void *)0); tr = tr->tr_next)
    {
	ga_clear(&tr->tr_region);
	ts_tree_delete(tr->tr_tree);
    }

    // Free all child trees
    for (languagetree_T *child = lt->lt_children; child != NULL;)
    {
	languagetree_T *next = child->lt_next;

	languagetree_free(child);
	child = next;
    }

    // Unlink from both lists
    if (lt->lt_parent != NULL && lt->lt_parent->lt_children == lt)
	lt->lt_parent->lt_children = lt->lt_next;
    if (lt->lt_prev != NULL)
	lt->lt_prev->lt_next = lt->lt_next;
    if (lt->lt_next != NULL)
	lt->lt_next->lt_prev = lt->lt_prev;

    if (lt == trees_to_parse && trees_to_parse != NULL)
	trees_to_parse = lt->lt_nextparse;
    if (lt->lt_prevparse != NULL)
	lt->lt_prevparse->lt_nextparse = lt->lt_nextparse;
    if (lt->lt_nextparse != NULL)
	lt->lt_nextparse->lt_prevparse = lt->lt_prevparse;

    vim_free(lt);
}

#endif // FEAT_TREESITTER
