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
	
	if (file_is_readable(buf))
	{
	    found = true;
	    break;
	}
    }
    if (found)
	return buf;
    semsg(_(e_treesitter_lang_not_found), lang);
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
    {
	semsg(_(e_treesitter_lang_not_found), name);
	return FAIL;
    }

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
 * "lang". By default the language tree will span the entire buffer. Returns OK
 * on success and FAIL on failure.
 */
    static int
languagetree_new(buf_T *buf, char_u *lang)
{
    TSLanguage	    *langobj;
    languagetree_T  *lt;

    if (buf->b_languagetree != NULL) // TODO: allow replace?
	return FAIL;

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
    buf->b_languagetree = lt;

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
    vim_free(lt->lt_name);

    if (lt->lt_parser != NULL)
	ts_parser_delete(lt->lt_parser);

    for (lt_region_T *lr = lt->lt_regions; lr != NULL; lr = lr->lr_next)
    {
	vim_free(lr->lr_region);
	ts_tree_delete(lr->lr_tree);
    }
    if (lt->lt_tree != NULL)
	ts_tree_delete(lt->lt_tree);

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

    vim_free(lt);
}

/*
 * Add a region in the source to the languagetree, taking ownership of
 * "region". Returns OK on success and FAIL on failure
 */
    static int
languagetree_add_region(languagetree_T *lt, TSRange *region, int len)
{
    lt_region_T *lr = ALLOC_CLEAR_ONE(lt_region_T);

    if (lr == NULL)
	return FAIL;

    lr->lr_region = region;

    lr->lr_next = lt->lt_regions;
    lt->lt_regions = lr;

    return OK;
}

/*
 * Parse callback for ts_parser_parse(), for a buffer.
 */
    static const char *
parser_buf_read_callback(
	void *payload,
	uint32_t byte_index UNUSED,
	TSPoint position,
	uint32_t *bytes_read)
{
#define PARSE_BUFSIZE 256
    buf_T        *bp = payload;
    static char buf[PARSE_BUFSIZE];

    // Finish if we are past the last line
    if ((linenr_T)position.row >= bp->b_ml.ml_line_count)
    {
	*bytes_read = 0;
	return NULL;
    }

    char *line = (char *)ml_get_buf(bp, position.row + 1, FALSE);
    uint32_t cols = ml_get_buf_len(bp, position.row + 1);
    uint32_t to_copy;

    // Should only be true if the last call didn't add a newline
    if (position.column > cols)
    {
	*bytes_read = 0;
	return NULL;
    }

    // Subtract one from buffer size so we can include newline
    to_copy = MIN(cols - position.column, PARSE_BUFSIZE - 1);
    memcpy(buf, line + position.column, to_copy);

    // If to_copy == cols, then the entire line fits in the buffer - 1, add a
    // newline. If to_copy == 0, then add a newline because we are at end of the
    // line.
    if (to_copy == cols || to_copy == 0)
	buf[to_copy++] = NL;

    *bytes_read = to_copy;

    return buf;
}

/*
 * Decode callback for ts_parser_parse_with_options()
 */
    static uint32_t
parser_decode_callback(
	const uint8_t *string,
	uint32_t length,
	int32_t *code_point)
{
    char_u *str = (char_u *)string;

    if (length == 0)
    {
	*code_point = 0;
	return 0;
    }
    else if (has_mbyte)
    {
	uint32_t char_len = (uint32_t)(*mb_ptr2len_len)(str, length);

	if (char_len > length)
	    // In middle of mutlibyte character
	    return 0;

	*code_point = (int32_t)(*mb_ptr2char)(str);
	return char_len;
    }

    // Characters are just single bytes, just set it directly
    *code_point = *string;
    return 1;
}

/*
 * Start parsing a region in the language tree. If "lr" is NULL, then parse the
 * entire document. Returns OK on success and FAIL on failure.
 */
    static int
languagetree_parse_region(languagetree_T *lt, lt_region_T *lr)
{
    TSInput input;
    TSTree  *res;

    if ((lr != NULL && lr->lr_valid) || (lr == NULL && lt->lt_valid))
	// Already valid, no need to parse
	return OK;

    if ((lr != NULL
		&& !ts_parser_set_included_ranges(lt->lt_parser, lr->lr_region,
	    lr->lr_len))
	    || (lr == NULL && !ts_parser_set_included_ranges(lt->lt_parser,
		    NULL, 0)))
    {
	iemsg("failed setting included ranges for TSParser");
	return FAIL;
    }

    if (enc_utf8)
	input.encoding = TSInputEncodingUTF8;
    else if (enc_unicode == 2)
    {
	int prop = enc_canon_props(p_enc);

	// Can either be UTF-16 or UCS-2, make sure it is UTF-16.
	if (prop & (ENC_ENDIAN_B | ENC_2WORD))
	    input.encoding = TSInputEncodingUTF16BE;
	else if (prop & (ENC_ENDIAN_L | ENC_2WORD))
	    input.encoding = TSInputEncodingUTF16LE;
	else
	    input.decode = parser_decode_callback;
    }
    else
	input.decode = parser_decode_callback;

    input.payload = lt->lt_buf;
    input.read = parser_buf_read_callback;

    // Use previous tree for region is possible
    res = lr == NULL ? lt->lt_tree : lr->lr_tree;
    res = ts_parser_parse(lt->lt_parser, res, input);

    if (res == NULL)
	return FAIL;

    if (lr != NULL)
    {
	if (lr->lr_tree != NULL)
	    ts_tree_delete(lr->lr_tree);
	lr->lr_tree = res;
	lr->lr_valid = true;

	lt->lt_num_valid_regions++;
	if (lt->lt_num_valid_regions == lt->lt_num_regions)
	    lt->lt_valid = true;
    }
    else
    {
	if (lt->lt_tree != NULL)
	    ts_tree_delete(lt->lt_tree);
	lt->lt_tree = res;

	lt->lt_valid = true;
    }

    return OK;
}

/*
 * Start parsing all invalid regions in the language tree, and handle all side
 * effects such as detecting new/removed injected languages. Returns OK on
 * success and FAIL on failure.
 */
    static int
languagetree_start(languagetree_T *lt, bool background UNUSED) // TODO: async
{
    int res = FAIL;

    if (lt->lt_regions == NULL)
	// Parse entire document
	res = languagetree_parse_region(lt, NULL);
    else
	for (lt_region_T *lr = lt->lt_regions; lr != NULL; lr = lr->lr_next)
	{
	    int pres = languagetree_parse_region(lt, lr);

	    if (pres == FAIL)
	    {
		res = FAIL;
		break;
	    }
	}

    if (res == FAIL)
	return FAIL;

    if (lt->lt_injection_query != NULL)
    {
	// Detect injected languages
    }

    return OK;
}

/*
 * "treesitter_start({lang}, {buf})" function
 */
    void
f_treesitter_start(typval_T *argvars, typval_T *rettv)
{
    buf_T	    *buf;
    char_u	    *lang;
    int		    res;

    if (in_vim9script()
	    && (check_for_string_arg(argvars, 0) == FAIL
		|| check_for_opt_buffer_arg(argvars, 1) == FAIL))
	return;

    if (argvars[1].v_type == VAR_UNKNOWN)
	buf = curbuf;
    else
	buf = tv_get_buf_from_arg(&argvars[1]);

    if (buf == NULL)
	return;

    lang = tv_get_string(&argvars[0]);
    if (lang == NULL)
	return;

    if (languagetree_new(buf, lang) == FAIL)
	return;

    res = languagetree_start(buf->b_languagetree, false);

    rettv->v_type = VAR_BOOL;
    rettv->vval.v_number = res == OK;
}

#endif // FEAT_TREESITTER
