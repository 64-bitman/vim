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
    TSLanguage		*tl_obj;    // Opaque object, may be NULL if not loaded

    char_u		*tl_path;   // Path to language parser
    char_u		*tl_symbol; // Symbol name suffix

    treesitter_lang_T	*tl_next;   // Next in list
    treesitter_lang_T	*tl_prev;   // Previous in list

    char_u		tl_name[1]; // Actually longer (language name)
};

static treesitter_lang_T *treesitter_langs = NULL;
#define FOR_ALL_LANGS(l) for (l = treesitter_langs; l != NULL; l = l->tl_next)

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
	semsg(_(e_could_not_load_library_function_str), IObuff);
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
 * that is done when neeeded. Returns OK on success and FAIL on failure.
 */
    static int
treesitter_add_language(char_u *name, char_u *path, char_u *symbol_name)
{
    treesitter_lang_T *lang;

    lang = alloc_clear(sizeof(treesitter_lang_T) + STRLEN(name));
    if (lang == NULL)
    {
	emsg(_(e_out_of_memory));
	return FAIL;
    }

    sprintf((char *)lang->tl_name, "%s", name);
    lang->tl_path = vim_strsave(path);
    lang->tl_symbol = vim_strsave(symbol_name == NULL ? name : symbol_name);

    if (lang->tl_path == NULL || lang->tl_symbol == NULL)
    {
	emsg(_(e_out_of_memory));
	treesitter_free_lang(lang);
	return FAIL;
    }

    lang->tl_next = treesitter_langs;
    treesitter_langs = lang;
    return OK;
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
 * Find the struct for the language named "name", and return its TSLanguage. If
 * it is not loaded, then load it. Returns NULL on failure.
 */
    static TSLanguage *
treesitter_get_language(char_u *name)
{
    treesitter_lang_T *lang = treesitter_find_lang(name);

    if (lang != NULL)
    {
	if (lang->tl_obj == NULL)
	    lang->tl_obj = treesitter_load_language(
		    lang->tl_path, lang->tl_symbol, &lang->tl_handle);
	return lang->tl_obj;
    }

    semsg(_(e_treesitter_lang_not_found), name);
    return NULL;
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

    // Handle ":treesitter info {lang}" subcommand.
    if (STRNCMP(line, "info", name_end - line) == 0)
    {
	lang = treesitter_find_lang(linep);
	if (lang == NULL)
	    semsg(_(e_treesitter_lang_not_found), linep);
	else
	    treesitter_list_lang(lang);
	return;
    }

    // Handle ":treesitter load {lang}" subcommand
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

    // Handle ":treesitter define {name} {path}" subcommand
    if (STRNCMP(line, "define", name_end - line) == 0)
    {
	char_u *name = NULL;

	// Isolate language name and parser path
	line = linep;
	name_end = skiptowhite(line);
	linep = skipwhite(name_end);

	if (*linep == NUL)
	{
	    emsg(_(e_invalid_argument));
	    return;
	}

	name = vim_strnsave(line, name_end - line);
	if (name == NULL)
	    return;

	treesitter_add_language(name, linep, NULL);
	vim_free(name);
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

#endif // FEAT_TREESITTER
