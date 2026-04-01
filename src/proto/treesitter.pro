/* treesitter.c */
void treesitter_init(void);
void treesitter_uninit(void);
void langtree_free(langtree_T *lt);
void langtree_update(langtree_T *lt, linenr_T start, colnr_T col, linenr_T end, long added);
void langtree_parse(langtree_T *lt, TSRange *ranges, int ranges_len);
bool set_ref_in_langtree(langtree_T *lt, int copyID);
void ex_treesitter(exarg_T *eap);
void set_context_in_treesitter_cmd(expand_T *xp, char_u *arg);
void f_treesitter_add(typval_T *argvars, typval_T *rettv);
/* vim: set ft=c : */
