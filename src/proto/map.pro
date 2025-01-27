/* map.c */
mapblock_T *get_maphash_list(int state, int c);
mapblock_T *get_buf_maphash_list(int state, int c);
int is_maphash_valid(void);
int do_map(int maptype, char_u *arg, int mode, int abbrev);
void map_clear_int(buf_T *buf, int mode, int local, int abbr);
int mode_str2flags(char_u *modechars);
int map_to_exists(char_u *str, char_u *modechars, int abbr);
int map_to_exists_mode(char_u *rhs, int mode, int abbr);
char_u *set_context_in_map_cmd(expand_T *xp, char_u *cmd, char_u *arg, int forceit, int isabbrev, int isunmap, cmdidx_T cmdidx);
int ExpandMappings(char_u *pat, regmatch_T *regmatch, int *num_file, char_u ***file);
int check_abbr(int c, char_u *ptr, int col, int mincol);
char_u *eval_map_expr(mapblock_T *mp, int c);
char_u *vim_strsave_escape_csi(char_u *p);
void vim_unescape_csi(char_u *p);
int makemap(FILE *fd, buf_T *buf);
int put_escstr(FILE *fd, char_u *strstart, int what);
void check_map_keycodes(void);
char_u *check_map(char_u *keys, int mode, int exact, int ign_mod, int abbr, mapblock_T **mp_ptr, int *local_ptr);
void f_maparg(typval_T *argvars, typval_T *rettv);
void f_mapcheck(typval_T *argvars, typval_T *rettv);
void f_mapset(typval_T *argvars, typval_T *rettv);
void init_mappings(void);
void add_map(char_u *map, int mode);
int langmap_adjust_mb(int c);
void langmap_init(void);
void langmap_set(void);
void ex_abbreviate(exarg_T *eap);
void ex_map(exarg_T *eap);
void ex_unmap(exarg_T *eap);
void ex_mapclear(exarg_T *eap);
void ex_abclear(exarg_T *eap);
/* vim: set ft=c : */
