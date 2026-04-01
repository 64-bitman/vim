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

typedef struct treesitter_predicate_S treesitter_predicate_T;

typedef enum
{
    PREDICATE_FLAG_NONE = 0,	    // ""
    PREDICATE_FLAG_ANY = 1 << 0,    // "any-"
    PREDICATE_FLAG_NOT = 1 << 1,    // "not-"
} treesitter_predicate_flags_T;

typedef enum
{
    PREDICATES,		// Predicates
    PREDICATE_EQ,	// "eq? {capture} {string|capture}"
    PREDICATE_MATCH,	// "match? {capture} {regex}"

    DIRECTIVES,		// Directives
    DIRECTIVE_SET,	// "set! {key} {value}"
    DIRECTIVE_OFFSET,	// "offset! {capture} srow scol erow ecol"

    PREDICATE_UNKNOWN	// Unknown predicate or directive. TODO user defined
} treesitter_predicate_type_T;

struct treesitter_predicate_S
{
    treesitter_predicate_type_T	tp_type;
    char_u			tp_flags;
    const TSQueryPredicateStep	*tp_steps;  // Owned by the TSQuery object
    uint32_t			tp_len;
};

typedef struct
{
    treesitter_predicate_T  *tpa_predicates; // List of predicates and
					     // directives.
    uint32_t		    tpa_len;
} treesitter_pattern_T;

typedef struct
{
    TSQuery	*tq_query;
    char_u	*tq_source;
    bool	tq_ispath;

    treesitter_pattern_T    *tq_patterns;
    uint32_t		    tq_len;
} treesitter_query_T;

/*
 * Represents Treesitter language library
 */
struct treesitter_lang_S
{
    HANDLE		tl_handle;
    TSLanguage		*tl_obj;    // Opaque object, may be NULL if not loaded

    char_u		*tl_path;   // Path to language parser
    char_u		*tl_symbol; // Symbol name suffix

    treesitter_query_T	*tl_injection_query;

    treesitter_lang_T	*tl_next;
    treesitter_lang_T	*tl_prev;

    char_u		tl_name[1]; // Actually longer (language name)
};

static TSRange tsrange_max;

static treesitter_lang_T *treesitter_langs = NULL;
#define FOR_ALL_LANGS(l) for (l = treesitter_langs; l != NULL; l = l->tl_next)

static int treesitter_query_setup(treesitter_lang_T *lang, treesitter_query_T *tq);
static void treesitter_free_query(treesitter_query_T *tq);
static void treesitter_free_lang(treesitter_lang_T *lang);

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

    tsrange_max.end_byte = UINT32_MAX;
    tsrange_max.end_point.row = UINT32_MAX;
    tsrange_max.end_point.column = UINT32_MAX;
}

    void
treesitter_uninit(void)
{
    treesitter_lang_T *lang = treesitter_langs;

    while (lang != NULL)
    {
	treesitter_lang_T *next = lang->tl_next;

	treesitter_free_lang(lang);
	lang = next;
    }
}

/*
 * Load the Treesitter library at the given path, and return the language object
 * + store the handle in "handle". Returns NULL on failure.
 */
    static TSLanguage *
treesitter_load_language(char_u *path, char_u *symbol, HANDLE *handle)
{
    HANDLE	h = load_dll((char *)path);
    TSLanguage	*lang;
    TSLanguage	*(*func)(void);

    if (h == NULL)
    {
	semsg(_(e_could_not_load_library_str_str), path, load_dll_error());
	return NULL;
    }

    vim_snprintf((char *)IObuff, IOSIZE, "tree_sitter_%s", symbol);
    func = symbol_from_dll(h, (char *)IObuff);
    if (func == NULL)
    {
	semsg(_(e_could_not_load_library_str_str), path, load_dll_error());
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
 * Free struct for language "name" and close the library/dll.
 */
    static void
treesitter_free_lang(treesitter_lang_T *lang)
{
    if (lang->tl_injection_query != NULL)
	treesitter_free_query(lang->tl_injection_query);

    if (lang->tl_obj != NULL)
	ts_language_delete(lang->tl_obj);
    if (lang->tl_handle != NULL)
	close_dll(lang->tl_handle);

    vim_free(lang->tl_path);
    vim_free(lang->tl_symbol);

    if (lang == treesitter_langs)
	treesitter_langs = lang->tl_next;
    if (lang->tl_prev != NULL)
	lang->tl_prev->tl_next = lang->tl_next;
    if (lang->tl_next != NULL)
	lang->tl_next->tl_prev = lang->tl_prev;

    vim_free(lang);
}

/*
 * Add the language to the global list. This does not load the langauge parser,
 * that is done when neeeded. Returns the lang object on success and NULL on
 * failure.
 */
    static treesitter_lang_T *
treesitter_add_language(char_u *name, char_u *path, char_u *symbol_name)
{
    treesitter_lang_T *lang;

    lang = alloc_clear(sizeof(treesitter_lang_T) + STRLEN(name));
    if (lang == NULL)
    {
	emsg(_(e_out_of_memory));
	return NULL;
    }

    sprintf((char *)lang->tl_name, "%s", name);
    lang->tl_path = vim_strsave(path);
    lang->tl_symbol = vim_strsave(symbol_name == NULL ? name : symbol_name);

    if (lang->tl_path == NULL || lang->tl_symbol == NULL)
    {
	emsg(_(e_out_of_memory));
	treesitter_free_lang(lang);
	return NULL;
    }

    lang->tl_next = treesitter_langs;
    treesitter_langs = lang;
    return lang;
}

/*
 * Return struct for language "name", otherwise NULL.
 */
    static treesitter_lang_T *
treesitter_find_lang(char_u *name)
{
    treesitter_lang_T *lang;

    FOR_ALL_LANGS(lang)
	if (STRCMP(lang->tl_name, name) == 0)
	    return lang;
    return NULL;
}

/*
 * Find the struct for the language "name", and return it. If it is not loaded,
 * then load it and initialize any objects. Returns NULL on failure.
 */
    static treesitter_lang_T *
treesitter_get_language(char_u *name)
{
    treesitter_lang_T *lang = treesitter_find_lang(name);

    if (lang != NULL)
    {
	int abi;

	if (lang->tl_obj == NULL)
	{
	    lang->tl_obj = treesitter_load_language(
		    lang->tl_path, lang->tl_symbol, &lang->tl_handle);

	    if (lang->tl_injection_query != NULL)
	    {
		treesitter_query_setup(lang, lang->tl_injection_query);

		if (lang->tl_injection_query == NULL)
		{
		    treesitter_free_query(lang->tl_injection_query);
		    lang->tl_injection_query = NULL;
		}
	    }
	}

	if (lang->tl_obj != NULL)
	{
	    abi = ts_language_abi_version(lang->tl_obj);

	    // Check if ABI is compatible with us
	    if (abi >= TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION
		    && abi <= TREE_SITTER_LANGUAGE_VERSION)
		return lang;
	    else
	    {
		semsg(_(e_treesitter_lang_incompatible_abi), abi);

		ts_language_delete(lang->tl_obj);
		close_dll(lang->tl_handle);
	    }
	}
	return NULL;
    }

    semsg(_(e_treesitter_lang_not_found), name);
    return NULL;
}

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

   static const char *
parser_read_callback(
	void *payload,
	uint32_t byte_index UNUSED,
	TSPoint position,
	uint32_t *bytes_read)
{
#define PARSE_BUFSIZE 512
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
 * Parse the the buffer and return the resulting TSTree. Returns NULL on
 * failure.
 */
    static TSTree *
treesitter_parse(
	buf_T *buf,
	TSParser *parser,
	TSTree *old_tree)
{
    TSInput	input;
    TSTree	*res;

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

    input.payload = buf;
    input.read = parser_read_callback;

    res = ts_parser_parse(parser, old_tree, input);

    if (res == NULL)
	// Shouldn't return NULL, this is blocking
	iemsg("ts_parser_parse() returned NULL?");

    return res; 
}

/*
 *
 */
    static bool
treesitter_range_intercepts(TSRange a, TSRange b)
{
    if (a.start_byte == 0 && a.end_byte == 0
	    && b.start_byte == 0 && a.end_byte == 0)
    {
	// Must use rows and columns
	if (a.start_point.row < b.end_point.row
		|| (a.start_point.row == b.end_point.row
		    && a.start_point.column < b.end_point.column))
	    if (b.start_point.row < a.end_point.row
		    || (b.start_point.row == a.end_point.row
			&& b.start_point.column < a.end_point.column))
		return true;
	return false;
    }

    return (a.end_byte >= b.start_byte && b.end_byte <= a.end_byte)
	|| (b.end_byte >= a.start_byte && a.end_byte <= b.end_byte);
}

/*
 * Note that "a" and "b" must be sorted in the correct order (first range starts
 * first in document, later ones start later).
 */
    static bool
treesitter_intercepts(TSRange *a, int alen, TSRange *b, int blen)
{
    int ap = 0;
    int bp = 0;

    while (ap < alen && bp < blen)
    {
	if (treesitter_range_intercepts(a[ap], b[bp]))
	    return true;

	// Advance index of range tha ends earlier before the other.
	if (a[ap].end_point.row < b[bp].start_point.row)
	    ap++;
	else
	    bp++;
    }
    return false;
}

    static const char *
treesitter_query_error_str(TSQueryError error_type)
{
    // Taken from Neovim
    switch (error_type) {
	case TSQueryErrorSyntax:
	    return "Invalid syntax";
	case TSQueryErrorNodeType:
	    return "Invalid node type";
	case TSQueryErrorField:
	    return "Invalid field name";
	case TSQueryErrorCapture:
	    return "Invalid capture name";
	case TSQueryErrorStructure:
	    return "Impossible pattern";
	default:
	    return "Unknown error";
    }
}

    static TSQuery *
treesitter_load_query(treesitter_lang_T *lang, char_u *str, uint32_t len)
{
    TSQueryError    error;
    uint32_t	    error_off = 0;
    TSQuery	    *query;

    query = ts_query_new(lang->tl_obj, (char *)str, len, &error_off, &error);
    if (query == NULL)
	// Something wrong with query
	semsg(_(e_treesitter_query_error_str),
		treesitter_query_error_str(error), error_off);
    return query;
}

/*
 * Load the query file and return it. Returns NULL on failure.
 */
    static TSQuery *
treesitter_load_query_file(treesitter_lang_T *lang, char_u *path)
{
    FILE    *fp;
    char    *buf;
    int	    len;
    int	    r;
    TSQuery *query = NULL;

    expand_env(path, NameBuff, MAXPATHL);
    fp = mch_fopen((char *)NameBuff, "r");

    if (fp == NULL
	    || fseek(fp, 0L, SEEK_END) == -1
	    || (len = ftell(fp)) == -1
	    || fseek(fp, 0L, SEEK_SET) == -1)
    {
	semsg(_(e_treesitter_cannot_open_query_file), path);
	if (fp != NULL)
	    fclose(fp);
	return NULL;
    }

    buf = alloc(len); // No need to include NUL since ts_query_new() has len arg
    if (buf == NULL)
    {
	semsg(_(e_out_of_memory_allocating_nr_bytes), len);
	fclose(fp);
	return NULL;
    }

    r = (int)fread(buf, (size_t)1, (size_t)len, fp);
    fclose(fp);

    if (r == len)
	query = treesitter_load_query(lang, (char_u *)buf, len);
    else
	semsg(_(e_treesitter_cannot_open_query_file), path);

    vim_free(buf);
    return query;
}

    static void
treesitter_free_query(treesitter_query_T *tq)
{

    if (tq->tq_query != NULL)
    {
	for (uint32_t i = 0; i < tq->tq_len; i++)
	{
	    treesitter_pattern_T *pat = tq->tq_patterns + i;

	    vim_free(pat->tpa_predicates);
	}
	vim_free(tq->tq_patterns);

	ts_query_delete(tq->tq_query);
    }
    vim_free(tq->tq_source);
    vim_free(tq);
}

/*
 * If "tq" is not loaded ("tq_query" is NULL), then create it and load the
 * predicates/directives. Returns OK on success and FAIL on failure.
 */
    static int
treesitter_query_setup(treesitter_lang_T *lang, treesitter_query_T *tq)
{
    char_u	*source = tq->tq_source;
    uint32_t    n_pats;

    if (tq->tq_query != NULL)
	return OK;

    if (tq->tq_ispath)
	tq->tq_query = treesitter_load_query_file(lang, source);
    else
	tq->tq_query = treesitter_load_query(lang, source, STRLEN(source));
    if (tq->tq_query == NULL)
	return FAIL;

    n_pats = ts_query_pattern_count(tq->tq_query);
    tq->tq_patterns = ALLOC_CLEAR_MULT(treesitter_pattern_T, n_pats);
    if (tq->tq_patterns == NULL)
	// No need to free stuff, caller will do that on failure
	return FAIL;
    tq->tq_len = n_pats;

    // Each pattern may have mutliple predicates or directives.
    for (uint32_t i = 0; i < n_pats; i++)
    {
	treesitter_pattern_T	    *pat = tq->tq_patterns + i;
	uint32_t		    n_steps;
	const TSQueryPredicateStep  *steps;

	steps = ts_query_predicates_for_pattern(tq->tq_query, i, &n_steps);
	if (n_steps == 0)
	    // The pattern will just have zero predicates, must still increment
	    // index as it is the ID.
	    continue;

	// Get number of predicates in pattern
	for (uint32_t k = 0; k < n_steps; k++)
	    if (steps[k].type == TSQueryPredicateStepTypeDone)
		pat->tpa_len++;

	pat->tpa_predicates = ALLOC_CLEAR_MULT(treesitter_predicate_T,
		pat->tpa_len);
	if (pat->tpa_predicates == NULL)
	    return FAIL;

	for (uint32_t k = 0, n = 0; k < n_steps; n++)
	{
	    treesitter_predicate_T  *pred = pat->tpa_predicates + n;
	    const char		    *name;
	    uint32_t		    namelen;

	    // Get number of steps in predicate (excluding the sentinel)
	    for (uint32_t j = k; j < n_steps; j++)
		if (steps[j].type == TSQueryPredicateStepTypeDone)
		{
		    pred->tp_len = j - k;
		    break;
		}
	    pred->tp_steps = steps + k;
	    k += pred->tp_len + 1;

	    // First step is the name of the predicate/directive
	    pred->tp_flags = PREDICATE_FLAG_NONE;
	    pred->tp_type = PREDICATE_UNKNOWN;

	    if (pred->tp_steps->type != TSQueryPredicateStepTypeString)
	    {
		emsg(_(e_treesitter_invalid_predicate_name));
		return FAIL;
	    }

            name = ts_query_string_value_for_id(tq->tq_query,
		    pred->tp_steps->value_id, &namelen);
            while (true)
	    {
		if (STRNCMP(name, "any-", 4) == 0)
		    pred->tp_flags |= PREDICATE_FLAG_ANY;
		else if (STRNCMP(name, "not-", 4) == 0)
		    pred->tp_flags |= PREDICATE_FLAG_NOT;
		else
		    break;
	    }
	    if (*name == NUL)
	    {
		emsg(_(e_treesitter_invalid_predicate_name));
		return FAIL;
	    }

	    if (STRCMP(name, "eq?") == 0)
	    {
		if (pred->tp_len == 3
			&& pred->tp_steps[1].type
			== TSQueryPredicateStepTypeCapture)
		    pred->tp_type = PREDICATE_EQ;
		else
		{
		    emsg(_(e_treesitter_invalid_predicate_args));
		    return FAIL;
		}
	    }
	    else if (STRCMP(name, "set!") == 0)
	    {
		if ((pred->tp_len == 2 || pred->tp_len == 3)
			&& pred->tp_steps[1].type
			== TSQueryPredicateStepTypeString
			&& (pred->tp_len == 2 || pred->tp_steps[2].type
			== TSQueryPredicateStepTypeString))
		    pred->tp_type = DIRECTIVE_SET;
		else
		{
		    emsg(_(e_treesitter_invalid_predicate_args));
		    return FAIL;
		}
	    }
	}
    }

    return OK;
}

/*
 * Return a query struct from the given source. If "is_path" is true, then
 * "source" is a path to a file, otherwise it is a string containing the query.
 * If "lang->tl_obj" is not NULL, then the query object is created. Returns NULL
 * on failure.
 */
    static treesitter_query_T *
treesitter_get_query(treesitter_lang_T *lang, char_u *source, bool is_path)
{
    treesitter_query_T *tq = ALLOC_CLEAR_ONE(treesitter_query_T);

    if (tq == NULL)
	return NULL;

    tq->tq_source = vim_strsave(source);
    if (tq->tq_source == NULL)
    {
	vim_free(tq);
	return NULL;
    }

    tq->tq_ispath = is_path;

    if (lang->tl_obj != NULL && treesitter_query_setup(lang, tq) == FAIL)
    {
	treesitter_free_query(tq);
	return NULL;
    }

    return tq;
}

/*
 * Implementation of the "eq? {capture} {string|capture}" predicate.
 */
    static bool
treesitter_predicate_eq(
	langtree_T		*lt,
	treesitter_predicate_T	*pred,
	const TSQueryCapture	*captures,
	uint32_t		captures_len)
{
    return true;
}

/*
 *
 */
    static bool
treesitter_match_predicates(
	treesitter_query_T  *tq,
	TSQueryMatch	    *match)
{
    return true;
}

/*
 *
 */
    static void
treesitter_apply_directives(
	treesitter_query_T  *tq,
	dict_T		    *metadata,
	TSQueryMatch	    *match)
{
}

/*
 * Allocate a new language tree for buffer "buf" with the given language.
 * Returns NULL on failure.
 */
    static langtree_T *
langtree_new(treesitter_lang_T *lang, buf_T *buf)
{
    langtree_T *lt = ALLOC_CLEAR_ONE(langtree_T);

    if (lt == NULL)
	return NULL;

    lt->lt_parser = ts_parser_new();
    if (lt->lt_parser == NULL)
    {
	vim_free(lt);
	return NULL;
    }

    lt->lt_lang = lang;
    lt->lt_buf = buf;

    // Should always succeed, because we check ABI version beforehand
    ts_parser_set_language(lt->lt_parser, lang->tl_obj);

    return lt;
}

    void
langtree_free(langtree_T *lt)
{
    langtree_region_T	*lr = lt->lt_regions;
    langtree_T		*lt_child = lt->lt_children;

    while (lr != NULL)
    {
	langtree_region_T *next = lr->lr_next;

	if (lr->lr_tree != NULL)
	    ts_tree_delete(lr->lr_tree);
	vim_free(lr->lr_ranges);
	vim_free(lr);

	lr = next;
    }

    while (lt_child != NULL)
    {
	langtree_T *next = lt_child->lt_next;

	langtree_free(lt);
	lt_child = next;
    }

    ts_parser_delete(lt->lt_parser);
    vim_free(lt);
}

/*
 * Parse all invalid regions in the language tree. If "ranges" is not NULL, then
 * only parse regions that intercept "ranges".
 */
    static int
langtree_parse_regions(langtree_T *lt, TSRange *ranges, int ranges_len)
{
    int ranges_parsed = 0;

    if (lt->lt_regions == NULL)
    {
	// Set included ranges to entire document
	lt->lt_regions = ALLOC_CLEAR_ONE(langtree_region_T);

	if (lt->lt_regions == NULL)
	    return 0;
	lt->lt_regions_len = 1;
    }

    if (lt->lt_regions_len == lt->lt_valid_regions_len)
	return 0;

    for (langtree_region_T *lr = lt->lt_regions; lr != NULL; lr = lr->lr_next)
    {
	bool	res;
	TSTree	*tree;

	if (lr->lr_valid)
	    continue;
	if (lr->lr_ranges != NULL && ranges != NULL
		&& !treesitter_intercepts(lr->lr_ranges, lr->lr_len,
		    ranges, ranges_len))
	    continue;

	if (lr->lr_ranges == 0)
	    res = ts_parser_set_included_ranges(lt->lt_parser, &tsrange_max, 1);
	else
	    res = ts_parser_set_included_ranges(lt->lt_parser, lr->lr_ranges,
		    lr->lr_len);

	if (!res)
	{
	    iemsg("ts_parser_set_included_ranges() fail");
	    continue;
	}

	tree = treesitter_parse(lt->lt_buf, lt->lt_parser, lr->lr_tree);

	if (tree != NULL)
	{
	    if (lr->lr_tree != NULL)
		ts_tree_delete(lr->lr_tree);
	    lr->lr_tree = tree;
	    lr->lr_valid = true;
	    ranges_parsed++;
	}
    }
    return ranges_parsed;
}

    static void
langtree_edit(langtree_T *lt, TSInputEdit *edit, TSRange *changed)
{
    for (langtree_region_T *lr = lt->lt_regions; lr != NULL; lr = lr->lr_next)
    {
	if (lr->lr_tree != NULL)
	{
	    ts_tree_edit(lr->lr_tree, edit);

	    // Update region range
	    vim_free(lr->lr_ranges);
	    lr->lr_ranges = ts_tree_included_ranges(lr->lr_tree, &lr->lr_len);

	    // Check if region is valid or not
	    if (lr->lr_ranges != NULL
		    && treesitter_intercepts(changed, 1, lr->lr_ranges,
			lr->lr_len))
		lr->lr_valid = false;
	}
    }

    ts_parser_reset(lt->lt_parser);

    for (langtree_T *c = lt->lt_children; c != NULL; c = c->lt_next)
	langtree_edit(lt, edit, changed);
}
    
/*
 * Update the language tree incrementally with the given changes.
 */
    void
langtree_update(
	langtree_T  *lt,
	linenr_T    start,
	colnr_T	    col,
	linenr_T    end,
	long	    added)
{
    buf_T	*buf = lt->lt_buf;
    TSInputEdit	edit;
    TSRange	changed;

    edit.start_point.row = start;
    edit.start_point.column = col;
    edit.start_byte = ml_find_line_or_offset(buf, edit.start_point.row, NULL);
    edit.old_end_point.row = end + added - 1;
    edit.old_end_point.column = ml_get_buf_len(buf, edit.old_end_point.row);
    edit.old_end_byte = ml_find_line_or_offset(buf, edit.old_end_point.row, NULL);
    edit.new_end_point.row = end - 1 + added;
    edit.new_end_point.column = ml_get_buf_len(buf, edit.new_end_point.row);
    edit.new_end_byte = ml_find_line_or_offset(buf, edit.new_end_point.row, NULL);

    if (edit.old_end_point.column > 0)
	edit.old_end_point.column--;
    if (edit.new_end_point.column > 0)
	edit.new_end_point.column--;

    changed.start_point.row = edit.start_point.row;
    changed.start_point.column = edit.start_point.column;
    changed.start_byte = edit.start_byte;
    changed.end_point.row = edit.old_end_point.row;
    changed.end_point.column = edit.old_end_point.column;
    changed.end_byte = edit.old_end_byte;

    langtree_edit(lt, &edit, &changed);
}

/*
 *
 */
    static void
langtree_handle_injections(langtree_T *lt)
{


}

/*
 * Parse the language tree and handle injected languages.
 */
    void
langtree_parse(langtree_T *lt, TSRange *ranges, int ranges_len)
{
    int parsed = langtree_parse_regions(lt, ranges, ranges_len);

    if (parsed == 0)
	// Everything is valid
	return;

    if (lt->lt_lang->tl_injection_query != NULL)
	langtree_handle_injections(lt);
}

/*
 * Output ABI version "abi". If -1, then use "unknown"
 */
    static void
treesitter_list_abi(int abi)
{
    static char buf[10];

    msg_puts("ABI version: ");
    if (abi == -1)
	msg_puts("unknown");
    else
    {
	vim_snprintf(buf, 10, "%d", abi);
	msg_puts(buf);
    }
}

    static void
treesitter_list_lang(treesitter_lang_T *lang)
{
    msg_puts("Name: ");
    msg_outtrans(lang->tl_name);
    msg_puts("\n");

    msg_puts("Loaded: ");
    msg_puts(lang->tl_obj == NULL ? "false" : "true");
    msg_puts("\n");

    msg_puts("Parser path: ");
    msg_outtrans(lang->tl_path);
    msg_puts("\n");

    msg_puts("Symbol name: ");
    msg_outtrans(lang->tl_symbol);
    msg_puts("\n");

    treesitter_list_abi(lang->tl_obj == NULL ? -1
	    : ts_language_abi_version(lang->tl_obj));
}

/*
 * Implementation of the ":treesitter" Ex command.
 */
    void
ex_treesitter(exarg_T *eap)
{
    char_u		*line = eap->arg;
    treesitter_lang_T	*lang;
    char_u		*name_end;
    char_u		*linep;

    // If no argument then list information about treesitter and loaded
    // languages.
    if (ends_excmd2(line - 1, line))
    {
	msg_putchar('\n');
	msg_puts("Latest ");
	treesitter_list_abi(TREE_SITTER_LANGUAGE_VERSION);
	msg_putchar('\n');

	msg_puts("Minimum ");
	treesitter_list_abi(TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION);
	msg_puts("\n\n");

	msg_puts("Treesitter languages:");
	msg_putchar('\n');

	FOR_ALL_LANGS(lang)
	    treesitter_list_lang(lang);
	return;
    }

    // Isolate the name.
    name_end = skiptowhite(line);
    linep = skipwhite(name_end);

    // Handle ":treesitter info {name}" subcommand.
    if (STRNCMP(line, "info", name_end - line) == 0)
    {
	lang = treesitter_find_lang(linep);
	if (lang == NULL)
	    semsg(_(e_treesitter_lang_not_found), linep);
	else
	    treesitter_list_lang(lang);
	return;
    }

    // Handle ":treesitter load {name}" subcommand
    if (STRNCMP(line, "load", name_end - line) == 0)
    {
	lang = treesitter_find_lang(linep);

	if (lang == NULL)
	    semsg(_(e_treesitter_lang_not_found), linep);
	else
	    lang->tl_obj = treesitter_load_language(
		    lang->tl_path, lang->tl_symbol, &lang->tl_handle);
	return;
    }

    // Handle ":treesitter start {name}" subcommand
    if (STRNCMP(line, "start", name_end - line) == 0)
    {
	if (curbuf->b_langtree != NULL)
	    return;

	lang = treesitter_get_language(linep);

	if (lang != NULL)
	{
	    curbuf->b_langtree = langtree_new(lang, curbuf);
	    langtree_parse(curbuf->b_langtree, NULL, 0);
	}
	return;
    }
}

/*
 * Handle command line completion for ":treesitter" command. TODO
 */
    void
set_context_in_treesitter_cmd(expand_T *xp, char_u *arg)
{
}

/*
 * "treesitter_add(name, opts)" function
 */
    void
f_treesitter_add(typval_T *argvars, typval_T *rettv)
{
    char_u		*name = NULL;
    char_u  		*path = NULL;
    char_u  		*symbol = NULL;
    char_u		*iquery = NULL;
    dict_T  		*opts;
    treesitter_lang_T	*lang;

    if (in_vim9script()
	    && (check_for_string_arg(argvars, 0) == FAIL
		|| check_for_dict_arg(argvars, 1) == FAIL))
	return;

    if (check_for_dict_arg(argvars, 1) == FAIL)
	return;

    name = vim_strsave(tv_get_string_strict(&argvars[0]));
    opts = argvars[1].vval.v_dict;

    if (dict_has_key(opts, "path"))
	path = dict_get_string(opts, "path", true);
    else
    {
	semsg(_(e_missing_argument_str), "path");
	goto exit;
    }

    if (dict_has_key(opts, "symbol"))
	symbol = dict_get_string(opts, "symbol", true);

    lang = treesitter_add_language(name, path, symbol);

    if (lang == NULL)
	goto exit;

    if (dict_has_key(opts, "injection_query"))
    {
	iquery = dict_get_string(opts, "injection_query", false);
	lang->tl_injection_query = treesitter_get_query(lang, iquery, false);
    }
    else if (dict_has_key(opts, "injection_query_path"))
    {
	iquery = dict_get_string(opts, "injection_query_path", false);
	lang->tl_injection_query = treesitter_get_query(lang, iquery, true);
    }

exit:
    vim_free(name);
    vim_free(path);
    vim_free(symbol);
}

#endif // FEAT_TREESITTER
