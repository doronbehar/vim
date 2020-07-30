/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * dict.c: Dictionary support
 */

#include "vim.h"

#if defined(FEAT_EVAL) || defined(PROTO)

// List head for garbage collection. Although there can be a reference loop
// from partial to dict to partial, we don't need to keep track of the partial,
// since it will get freed when the dict is unused and gets freed.
static dict_T		*first_dict = NULL;

/*
 * Allocate an empty header for a dictionary.
 */
    dict_T *
dict_alloc(void)
{
    dict_T *d;

    d = ALLOC_CLEAR_ONE(dict_T);
    if (d != NULL)
    {
	// Add the dict to the list of dicts for garbage collection.
	if (first_dict != NULL)
	    first_dict->dv_used_prev = d;
	d->dv_used_next = first_dict;
	d->dv_used_prev = NULL;
	first_dict = d;

	hash_init(&d->dv_hashtab);
	d->dv_lock = 0;
	d->dv_scope = 0;
	d->dv_refcount = 0;
	d->dv_copyID = 0;
    }
    return d;
}

/*
 * dict_alloc() with an ID for alloc_fail().
 */
    dict_T *
dict_alloc_id(alloc_id_T id UNUSED)
{
#ifdef FEAT_EVAL
    if (alloc_fail_id == id && alloc_does_fail(sizeof(list_T)))
	return NULL;
#endif
    return (dict_alloc());
}

    dict_T *
dict_alloc_lock(int lock)
{
    dict_T *d = dict_alloc();

    if (d != NULL)
	d->dv_lock = lock;
    return d;
}

/*
 * Allocate an empty dict for a return value.
 * Returns OK or FAIL.
 */
    int
rettv_dict_alloc(typval_T *rettv)
{
    dict_T	*d = dict_alloc_lock(0);

    if (d == NULL)
	return FAIL;

    rettv_dict_set(rettv, d);
    return OK;
}

/*
 * Set a dictionary as the return value
 */
    void
rettv_dict_set(typval_T *rettv, dict_T *d)
{
    rettv->v_type = VAR_DICT;
    rettv->vval.v_dict = d;
    if (d != NULL)
	++d->dv_refcount;
}

/*
 * Free a Dictionary, including all non-container items it contains.
 * Ignores the reference count.
 */
    void
dict_free_contents(dict_T *d)
{
    hashtab_free_contents(&d->dv_hashtab);
}

/*
 * Clear hashtab "ht" and dict items it contains.
 */
    void
hashtab_free_contents(hashtab_T *ht)
{
    int		todo;
    hashitem_T	*hi;
    dictitem_T	*di;

    // Lock the hashtab, we don't want it to resize while freeing items.
    hash_lock(ht);
    todo = (int)ht->ht_used;
    for (hi = ht->ht_array; todo > 0; ++hi)
    {
	if (!HASHITEM_EMPTY(hi))
	{
	    // Remove the item before deleting it, just in case there is
	    // something recursive causing trouble.
	    di = HI2DI(hi);
	    hash_remove(ht, hi);
	    dictitem_free(di);
	    --todo;
	}
    }

    // The hashtab is still locked, it has to be re-initialized anyway.
    hash_clear(ht);
}

    static void
dict_free_dict(dict_T *d)
{
    // Remove the dict from the list of dicts for garbage collection.
    if (d->dv_used_prev == NULL)
	first_dict = d->dv_used_next;
    else
	d->dv_used_prev->dv_used_next = d->dv_used_next;
    if (d->dv_used_next != NULL)
	d->dv_used_next->dv_used_prev = d->dv_used_prev;
    vim_free(d);
}

    static void
dict_free(dict_T *d)
{
    if (!in_free_unref_items)
    {
	dict_free_contents(d);
	dict_free_dict(d);
    }
}

/*
 * Unreference a Dictionary: decrement the reference count and free it when it
 * becomes zero.
 */
    void
dict_unref(dict_T *d)
{
    if (d != NULL && --d->dv_refcount <= 0)
	dict_free(d);
}

/*
 * Go through the list of dicts and free items without the copyID.
 * Returns TRUE if something was freed.
 */
    int
dict_free_nonref(int copyID)
{
    dict_T	*dd;
    int		did_free = FALSE;

    for (dd = first_dict; dd != NULL; dd = dd->dv_used_next)
	if ((dd->dv_copyID & COPYID_MASK) != (copyID & COPYID_MASK))
	{
	    // Free the Dictionary and ordinary items it contains, but don't
	    // recurse into Lists and Dictionaries, they will be in the list
	    // of dicts or list of lists.
	    dict_free_contents(dd);
	    did_free = TRUE;
	}
    return did_free;
}

    void
dict_free_items(int copyID)
{
    dict_T	*dd, *dd_next;

    for (dd = first_dict; dd != NULL; dd = dd_next)
    {
	dd_next = dd->dv_used_next;
	if ((dd->dv_copyID & COPYID_MASK) != (copyID & COPYID_MASK))
	    dict_free_dict(dd);
    }
}

/*
 * Allocate a Dictionary item.
 * The "key" is copied to the new item.
 * Note that the type and value of the item "di_tv" still needs to be
 * initialized!
 * Returns NULL when out of memory.
 */
    dictitem_T *
dictitem_alloc(char_u *key)
{
    dictitem_T *di;

    di = alloc(offsetof(dictitem_T, di_key) + STRLEN(key) + 1);
    if (di != NULL)
    {
	STRCPY(di->di_key, key);
	di->di_flags = DI_FLAGS_ALLOC;
	di->di_tv.v_lock = 0;
    }
    return di;
}

/*
 * Make a copy of a Dictionary item.
 */
    static dictitem_T *
dictitem_copy(dictitem_T *org)
{
    dictitem_T *di;

    di = alloc(offsetof(dictitem_T, di_key) + STRLEN(org->di_key) + 1);
    if (di != NULL)
    {
	STRCPY(di->di_key, org->di_key);
	di->di_flags = DI_FLAGS_ALLOC;
	copy_tv(&org->di_tv, &di->di_tv);
    }
    return di;
}

/*
 * Remove item "item" from Dictionary "dict" and free it.
 */
    void
dictitem_remove(dict_T *dict, dictitem_T *item)
{
    hashitem_T	*hi;

    hi = hash_find(&dict->dv_hashtab, item->di_key);
    if (HASHITEM_EMPTY(hi))
	internal_error("dictitem_remove()");
    else
	hash_remove(&dict->dv_hashtab, hi);
    dictitem_free(item);
}

/*
 * Free a dict item.  Also clears the value.
 */
    void
dictitem_free(dictitem_T *item)
{
    clear_tv(&item->di_tv);
    if (item->di_flags & DI_FLAGS_ALLOC)
	vim_free(item);
}

/*
 * Make a copy of dict "d".  Shallow if "deep" is FALSE.
 * The refcount of the new dict is set to 1.
 * See item_copy() for "copyID".
 * Returns NULL when out of memory.
 */
    dict_T *
dict_copy(dict_T *orig, int deep, int copyID)
{
    dict_T	*copy;
    dictitem_T	*di;
    int		todo;
    hashitem_T	*hi;

    if (orig == NULL)
	return NULL;

    copy = dict_alloc();
    if (copy != NULL)
    {
	if (copyID != 0)
	{
	    orig->dv_copyID = copyID;
	    orig->dv_copydict = copy;
	}
	todo = (int)orig->dv_hashtab.ht_used;
	for (hi = orig->dv_hashtab.ht_array; todo > 0 && !got_int; ++hi)
	{
	    if (!HASHITEM_EMPTY(hi))
	    {
		--todo;

		di = dictitem_alloc(hi->hi_key);
		if (di == NULL)
		    break;
		if (deep)
		{
		    if (item_copy(&HI2DI(hi)->di_tv, &di->di_tv, deep,
							      copyID) == FAIL)
		    {
			vim_free(di);
			break;
		    }
		}
		else
		    copy_tv(&HI2DI(hi)->di_tv, &di->di_tv);
		if (dict_add(copy, di) == FAIL)
		{
		    dictitem_free(di);
		    break;
		}
	    }
	}

	++copy->dv_refcount;
	if (todo > 0)
	{
	    dict_unref(copy);
	    copy = NULL;
	}
    }

    return copy;
}

/*
 * Add item "item" to Dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add(dict_T *d, dictitem_T *item)
{
    return hash_add(&d->dv_hashtab, item->di_key);
}

/*
 * Add a number or special entry to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    static int
dict_add_number_special(dict_T *d, char *key, varnumber_T nr, vartype_T vartype)
{
    dictitem_T	*item;

    item = dictitem_alloc((char_u *)key);
    if (item == NULL)
	return FAIL;
    item->di_tv.v_type = vartype;
    item->di_tv.vval.v_number = nr;
    if (dict_add(d, item) == FAIL)
    {
	dictitem_free(item);
	return FAIL;
    }
    return OK;
}

/*
 * Add a number entry to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_number(dict_T *d, char *key, varnumber_T nr)
{
    return dict_add_number_special(d, key, nr, VAR_NUMBER);
}

/*
 * Add a special entry to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_bool(dict_T *d, char *key, varnumber_T nr)
{
    return dict_add_number_special(d, key, nr, VAR_BOOL);
}

/*
 * Add a string entry to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_string(dict_T *d, char *key, char_u *str)
{
    return dict_add_string_len(d, key, str, -1);
}

/*
 * Add a string entry to dictionary "d".
 * "str" will be copied to allocated memory.
 * When "len" is -1 use the whole string, otherwise only this many bytes.
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_string_len(dict_T *d, char *key, char_u *str, int len)
{
    dictitem_T	*item;
    char_u	*val = NULL;

    item = dictitem_alloc((char_u *)key);
    if (item == NULL)
	return FAIL;
    item->di_tv.v_type = VAR_STRING;
    if (str != NULL)
    {
	if (len == -1)
	    val = vim_strsave(str);
	else
	    val = vim_strnsave(str, len);
    }
    item->di_tv.vval.v_string = val;
    if (dict_add(d, item) == FAIL)
    {
	dictitem_free(item);
	return FAIL;
    }
    return OK;
}

/*
 * Add a list entry to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_list(dict_T *d, char *key, list_T *list)
{
    dictitem_T	*item;

    item = dictitem_alloc((char_u *)key);
    if (item == NULL)
	return FAIL;
    item->di_tv.v_type = VAR_LIST;
    item->di_tv.vval.v_list = list;
    ++list->lv_refcount;
    if (dict_add(d, item) == FAIL)
    {
	dictitem_free(item);
	return FAIL;
    }
    return OK;
}

/*
 * Add a typval_T entry to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_tv(dict_T *d, char *key, typval_T *tv)
{
    dictitem_T	*item;

    item = dictitem_alloc((char_u *)key);
    if (item == NULL)
	return FAIL;
    copy_tv(tv, &item->di_tv);
    if (dict_add(d, item) == FAIL)
    {
	dictitem_free(item);
	return FAIL;
    }
    return OK;
}

/*
 * Add a callback to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_callback(dict_T *d, char *key, callback_T *cb)
{
    dictitem_T	*item;

    item = dictitem_alloc((char_u *)key);
    if (item == NULL)
	return FAIL;
    put_callback(cb, &item->di_tv);
    if (dict_add(d, item) == FAIL)
    {
	dictitem_free(item);
	return FAIL;
    }
    return OK;
}

/*
 * Initializes "iter" for iterating over dictionary items with
 * dict_iterate_next().
 * If "var" is not a Dict or an empty Dict then there will be nothing to
 * iterate over, no error is given.
 * NOTE: The dictionary must not change until iterating is finished!
 */
    void
dict_iterate_start(typval_T *var, dict_iterator_T *iter)
{
    if (var->v_type != VAR_DICT || var->vval.v_dict == NULL)
	iter->dit_todo = 0;
    else
    {
	dict_T	*d = var->vval.v_dict;

	iter->dit_todo = d->dv_hashtab.ht_used;
	iter->dit_hi = d->dv_hashtab.ht_array;
    }
}

/*
 * Iterate over the items referred to by "iter".  It should be initialized with
 * dict_iterate_start().
 * Returns a pointer to the key.
 * "*tv_result" is set to point to the value for that key.
 * If there are no more items, NULL is returned.
 */
    char_u *
dict_iterate_next(dict_iterator_T *iter, typval_T **tv_result)
{
    dictitem_T	*di;
    char_u      *result;

    if (iter->dit_todo == 0)
	return NULL;

    while (HASHITEM_EMPTY(iter->dit_hi))
	++iter->dit_hi;

    di = HI2DI(iter->dit_hi);
    result = di->di_key;
    *tv_result = &di->di_tv;

    --iter->dit_todo;
    ++iter->dit_hi;
    return result;
}

/*
 * Add a dict entry to dictionary "d".
 * Returns FAIL when out of memory and when key already exists.
 */
    int
dict_add_dict(dict_T *d, char *key, dict_T *dict)
{
    dictitem_T	*item;

    item = dictitem_alloc((char_u *)key);
    if (item == NULL)
	return FAIL;
    item->di_tv.v_type = VAR_DICT;
    item->di_tv.vval.v_dict = dict;
    ++dict->dv_refcount;
    if (dict_add(d, item) == FAIL)
    {
	dictitem_free(item);
	return FAIL;
    }
    return OK;
}

/*
 * Get the number of items in a Dictionary.
 */
    long
dict_len(dict_T *d)
{
    if (d == NULL)
	return 0L;
    return (long)d->dv_hashtab.ht_used;
}

/*
 * Find item "key[len]" in Dictionary "d".
 * If "len" is negative use strlen(key).
 * Returns NULL when not found.
 */
    dictitem_T *
dict_find(dict_T *d, char_u *key, int len)
{
#define AKEYLEN 200
    char_u	buf[AKEYLEN];
    char_u	*akey;
    char_u	*tofree = NULL;
    hashitem_T	*hi;

    if (d == NULL)
	return NULL;
    if (len < 0)
	akey = key;
    else if (len >= AKEYLEN)
    {
	tofree = akey = vim_strnsave(key, len);
	if (akey == NULL)
	    return NULL;
    }
    else
    {
	// Avoid a malloc/free by using buf[].
	vim_strncpy(buf, key, len);
	akey = buf;
    }

    hi = hash_find(&d->dv_hashtab, akey);
    vim_free(tofree);
    if (HASHITEM_EMPTY(hi))
	return NULL;
    return HI2DI(hi);
}

/*
 * Get a typval_T item from a dictionary and copy it into "rettv".
 * Returns FAIL if the entry doesn't exist or out of memory.
 */
    int
dict_get_tv(dict_T *d, char_u *key, typval_T *rettv)
{
    dictitem_T	*di;

    di = dict_find(d, key, -1);
    if (di == NULL)
	return FAIL;
    copy_tv(&di->di_tv, rettv);
    return OK;
}

/*
 * Get a string item from a dictionary.
 * When "save" is TRUE allocate memory for it.
 * When FALSE a shared buffer is used, can only be used once!
 * Returns NULL if the entry doesn't exist or out of memory.
 */
    char_u *
dict_get_string(dict_T *d, char_u *key, int save)
{
    dictitem_T	*di;
    char_u	*s;

    di = dict_find(d, key, -1);
    if (di == NULL)
	return NULL;
    s = tv_get_string(&di->di_tv);
    if (save && s != NULL)
	s = vim_strsave(s);
    return s;
}

/*
 * Get a number item from a dictionary.
 * Returns 0 if the entry doesn't exist.
 */
    varnumber_T
dict_get_number(dict_T *d, char_u *key)
{
    return dict_get_number_def(d, key, 0);
}

/*
 * Get a number item from a dictionary.
 * Returns "def" if the entry doesn't exist.
 */
    varnumber_T
dict_get_number_def(dict_T *d, char_u *key, int def)
{
    dictitem_T	*di;

    di = dict_find(d, key, -1);
    if (di == NULL)
	return def;
    return tv_get_number(&di->di_tv);
}

/*
 * Get a number item from a dictionary.
 * Returns 0 if the entry doesn't exist.
 * Give an error if the entry is not a number.
 */
    varnumber_T
dict_get_number_check(dict_T *d, char_u *key)
{
    dictitem_T	*di;

    di = dict_find(d, key, -1);
    if (di == NULL)
	return 0;
    if (di->di_tv.v_type != VAR_NUMBER)
    {
	semsg(_(e_invarg2), tv_get_string(&di->di_tv));
	return 0;
    }
    return tv_get_number(&di->di_tv);
}

/*
 * Return an allocated string with the string representation of a Dictionary.
 * May return NULL.
 */
    char_u *
dict2string(typval_T *tv, int copyID, int restore_copyID)
{
    garray_T	ga;
    int		first = TRUE;
    char_u	*tofree;
    char_u	numbuf[NUMBUFLEN];
    hashitem_T	*hi;
    char_u	*s;
    dict_T	*d;
    int		todo;

    if ((d = tv->vval.v_dict) == NULL)
	return NULL;
    ga_init2(&ga, (int)sizeof(char), 80);
    ga_append(&ga, '{');

    todo = (int)d->dv_hashtab.ht_used;
    for (hi = d->dv_hashtab.ht_array; todo > 0 && !got_int; ++hi)
    {
	if (!HASHITEM_EMPTY(hi))
	{
	    --todo;

	    if (first)
		first = FALSE;
	    else
		ga_concat(&ga, (char_u *)", ");

	    tofree = string_quote(hi->hi_key, FALSE);
	    if (tofree != NULL)
	    {
		ga_concat(&ga, tofree);
		vim_free(tofree);
	    }
	    ga_concat(&ga, (char_u *)": ");
	    s = echo_string_core(&HI2DI(hi)->di_tv, &tofree, numbuf, copyID,
						 FALSE, restore_copyID, TRUE);
	    if (s != NULL)
		ga_concat(&ga, s);
	    vim_free(tofree);
	    if (s == NULL || did_echo_string_emsg)
		break;
	    line_breakcheck();

	}
    }
    if (todo > 0)
    {
	vim_free(ga.ga_data);
	return NULL;
    }

    ga_append(&ga, '}');
    ga_append(&ga, NUL);
    return (char_u *)ga.ga_data;
}

/*
 * Get the key for #{key: val} into "tv" and advance "arg".
 * Return FAIL when there is no valid key.
 */
    static int
get_literal_key(char_u **arg, typval_T *tv)
{
    char_u *p;

    if (!ASCII_ISALNUM(**arg) && **arg != '_' && **arg != '-')
	return FAIL;

    for (p = *arg; ASCII_ISALNUM(*p) || *p == '_' || *p == '-'; ++p)
	;
    tv->v_type = VAR_STRING;
    tv->vval.v_string = vim_strnsave(*arg, p - *arg);

    *arg = skipwhite(p);
    return OK;
}

/*
 * Allocate a variable for a Dictionary and fill it from "*arg".
 * "*arg" points to the "{".
 * "literal" is TRUE for #{key: val}
 * Return OK or FAIL.  Returns NOTDONE for {expr}.
 */
    int
eval_dict(char_u **arg, typval_T *rettv, evalarg_T *evalarg, int literal)
{
    int		evaluate = evalarg == NULL ? FALSE
					 : evalarg->eval_flags & EVAL_EVALUATE;
    dict_T	*d = NULL;
    typval_T	tvkey;
    typval_T	tv;
    char_u	*key = NULL;
    dictitem_T	*item;
    char_u	*start = skipwhite(*arg + 1);
    char_u	buf[NUMBUFLEN];
    int		vim9script = in_vim9script();
    int		had_comma;

    /*
     * First check if it's not a curly-braces thing: {expr}.
     * Must do this without evaluating, otherwise a function may be called
     * twice.  Unfortunately this means we need to call eval1() twice for the
     * first item.
     * But {} is an empty Dictionary.
     */
    if (!vim9script && *start != '}')
    {
	if (eval1(&start, &tv, NULL) == FAIL)	// recursive!
	    return FAIL;
	if (*start == '}')
	    return NOTDONE;
    }

    if (evaluate)
    {
	d = dict_alloc();
	if (d == NULL)
	    return FAIL;
    }
    tvkey.v_type = VAR_UNKNOWN;
    tv.v_type = VAR_UNKNOWN;

    *arg = skipwhite_and_linebreak(*arg + 1, evalarg);
    while (**arg != '}' && **arg != NUL)
    {
	if ((literal
		? get_literal_key(arg, &tvkey)
		: eval1(arg, &tvkey, evalarg)) == FAIL)	// recursive!
	    goto failret;

	// The colon should come right after the key, but this wasn't checked
	// previously, so only require it in Vim9 script.
	if (!vim9script)
	    *arg = skipwhite(*arg);
	if (**arg != ':')
	{
	    if (evaluate)
		semsg(_(e_missing_dict_colon), *arg);
	    clear_tv(&tvkey);
	    goto failret;
	}
	if (evaluate)
	{
	    key = tv_get_string_buf_chk(&tvkey, buf);
	    if (key == NULL)
	    {
		// "key" is NULL when tv_get_string_buf_chk() gave an errmsg
		clear_tv(&tvkey);
		goto failret;
	    }
	}
	if (vim9script && (*arg)[1] != NUL && !VIM_ISWHITE((*arg)[1]))
	{
	    semsg(_(e_white_after), ":");
	    clear_tv(&tvkey);
	    goto failret;
	}

	*arg = skipwhite_and_linebreak(*arg + 1, evalarg);
	if (eval1(arg, &tv, evalarg) == FAIL)	// recursive!
	{
	    if (evaluate)
		clear_tv(&tvkey);
	    goto failret;
	}
	if (evaluate)
	{
	    item = dict_find(d, key, -1);
	    if (item != NULL)
	    {
		if (evaluate)
		    semsg(_(e_duplicate_key), key);
		clear_tv(&tvkey);
		clear_tv(&tv);
		goto failret;
	    }
	    item = dictitem_alloc(key);
	    if (item != NULL)
	    {
		item->di_tv = tv;
		item->di_tv.v_lock = 0;
		if (dict_add(d, item) == FAIL)
		    dictitem_free(item);
	    }
	}
	clear_tv(&tvkey);

	// the comma must come after the value
	had_comma = **arg == ',';
	if (had_comma)
	{
	    if (vim9script && (*arg)[1] != NUL && !VIM_ISWHITE((*arg)[1]))
	    {
		semsg(_(e_white_after), ",");
		goto failret;
	    }
	    *arg = skipwhite(*arg + 1);
	}

	// the "}" can be on the next line
	*arg = skipwhite_and_linebreak(*arg, evalarg);
	if (**arg == '}')
	    break;
	if (!had_comma)
	{
	    if (evaluate)
		semsg(_(e_missing_dict_comma), *arg);
	    goto failret;
	}
    }

    if (**arg != '}')
    {
	if (evaluate)
	    semsg(_(e_missing_dict_end), *arg);
failret:
	if (d != NULL)
	    dict_free(d);
	return FAIL;
    }

    *arg = skipwhite(*arg + 1);
    if (evaluate)
	rettv_dict_set(rettv, d);

    return OK;
}

/*
 * Go over all entries in "d2" and add them to "d1".
 * When "action" is "error" then a duplicate key is an error.
 * When "action" is "force" then a duplicate key is overwritten.
 * Otherwise duplicate keys are ignored ("action" is "keep").
 */
    void
dict_extend(dict_T *d1, dict_T *d2, char_u *action)
{
    dictitem_T	*di1;
    hashitem_T	*hi2;
    int		todo;
    char_u	*arg_errmsg = (char_u *)N_("extend() argument");

    todo = (int)d2->dv_hashtab.ht_used;
    for (hi2 = d2->dv_hashtab.ht_array; todo > 0; ++hi2)
    {
	if (!HASHITEM_EMPTY(hi2))
	{
	    --todo;
	    di1 = dict_find(d1, hi2->hi_key, -1);
	    if (d1->dv_scope != 0)
	    {
		// Disallow replacing a builtin function in l: and g:.
		// Check the key to be valid when adding to any scope.
		if (d1->dv_scope == VAR_DEF_SCOPE
			&& HI2DI(hi2)->di_tv.v_type == VAR_FUNC
			&& var_check_func_name(hi2->hi_key, di1 == NULL))
		    break;
		if (!valid_varname(hi2->hi_key))
		    break;
	    }
	    if (di1 == NULL)
	    {
		di1 = dictitem_copy(HI2DI(hi2));
		if (di1 != NULL && dict_add(d1, di1) == FAIL)
		    dictitem_free(di1);
	    }
	    else if (*action == 'e')
	    {
		semsg(_("E737: Key already exists: %s"), hi2->hi_key);
		break;
	    }
	    else if (*action == 'f' && HI2DI(hi2) != di1)
	    {
		if (var_check_lock(di1->di_tv.v_lock, arg_errmsg, TRUE)
			|| var_check_ro(di1->di_flags, arg_errmsg, TRUE))
		    break;
		clear_tv(&di1->di_tv);
		copy_tv(&HI2DI(hi2)->di_tv, &di1->di_tv);
	    }
	}
    }
}

/*
 * Return the dictitem that an entry in a hashtable points to.
 */
    dictitem_T *
dict_lookup(hashitem_T *hi)
{
    return HI2DI(hi);
}

/*
 * Return TRUE when two dictionaries have exactly the same key/values.
 */
    int
dict_equal(
    dict_T	*d1,
    dict_T	*d2,
    int		ic,	    // ignore case for strings
    int		recursive)  // TRUE when used recursively
{
    hashitem_T	*hi;
    dictitem_T	*item2;
    int		todo;

    if (d1 == d2)
	return TRUE;
    if (dict_len(d1) != dict_len(d2))
	return FALSE;
    if (dict_len(d1) == 0)
	// empty and NULL dicts are considered equal
	return TRUE;
    if (d1 == NULL || d2 == NULL)
	return FALSE;

    todo = (int)d1->dv_hashtab.ht_used;
    for (hi = d1->dv_hashtab.ht_array; todo > 0; ++hi)
    {
	if (!HASHITEM_EMPTY(hi))
	{
	    item2 = dict_find(d2, hi->hi_key, -1);
	    if (item2 == NULL)
		return FALSE;
	    if (!tv_equal(&HI2DI(hi)->di_tv, &item2->di_tv, ic, recursive))
		return FALSE;
	    --todo;
	}
    }
    return TRUE;
}

/*
 * Turn a dict into a list:
 * "what" == 0: list of keys
 * "what" == 1: list of values
 * "what" == 2: list of items
 */
    static void
dict_list(typval_T *argvars, typval_T *rettv, int what)
{
    list_T	*l2;
    dictitem_T	*di;
    hashitem_T	*hi;
    listitem_T	*li;
    listitem_T	*li2;
    dict_T	*d;
    int		todo;

    if (argvars[0].v_type != VAR_DICT)
    {
	emsg(_(e_dictreq));
	return;
    }
    if ((d = argvars[0].vval.v_dict) == NULL)
	return;

    if (rettv_list_alloc(rettv) == FAIL)
	return;

    todo = (int)d->dv_hashtab.ht_used;
    for (hi = d->dv_hashtab.ht_array; todo > 0; ++hi)
    {
	if (!HASHITEM_EMPTY(hi))
	{
	    --todo;
	    di = HI2DI(hi);

	    li = listitem_alloc();
	    if (li == NULL)
		break;
	    list_append(rettv->vval.v_list, li);

	    if (what == 0)
	    {
		// keys()
		li->li_tv.v_type = VAR_STRING;
		li->li_tv.v_lock = 0;
		li->li_tv.vval.v_string = vim_strsave(di->di_key);
	    }
	    else if (what == 1)
	    {
		// values()
		copy_tv(&di->di_tv, &li->li_tv);
	    }
	    else
	    {
		// items()
		l2 = list_alloc();
		li->li_tv.v_type = VAR_LIST;
		li->li_tv.v_lock = 0;
		li->li_tv.vval.v_list = l2;
		if (l2 == NULL)
		    break;
		++l2->lv_refcount;

		li2 = listitem_alloc();
		if (li2 == NULL)
		    break;
		list_append(l2, li2);
		li2->li_tv.v_type = VAR_STRING;
		li2->li_tv.v_lock = 0;
		li2->li_tv.vval.v_string = vim_strsave(di->di_key);

		li2 = listitem_alloc();
		if (li2 == NULL)
		    break;
		list_append(l2, li2);
		copy_tv(&di->di_tv, &li2->li_tv);
	    }
	}
    }
}

/*
 * "items(dict)" function
 */
    void
f_items(typval_T *argvars, typval_T *rettv)
{
    dict_list(argvars, rettv, 2);
}

/*
 * "keys()" function
 */
    void
f_keys(typval_T *argvars, typval_T *rettv)
{
    dict_list(argvars, rettv, 0);
}

/*
 * "values(dict)" function
 */
    void
f_values(typval_T *argvars, typval_T *rettv)
{
    dict_list(argvars, rettv, 1);
}

/*
 * Make each item in the dict readonly (not the value of the item).
 */
    void
dict_set_items_ro(dict_T *di)
{
    int		todo = (int)di->dv_hashtab.ht_used;
    hashitem_T	*hi;

    // Set readonly
    for (hi = di->dv_hashtab.ht_array; todo > 0 ; ++hi)
    {
	if (HASHITEM_EMPTY(hi))
	    continue;
	--todo;
	HI2DI(hi)->di_flags |= DI_FLAGS_RO | DI_FLAGS_FIX;
    }
}

/*
 * "has_key()" function
 */
    void
f_has_key(typval_T *argvars, typval_T *rettv)
{
    if (argvars[0].v_type != VAR_DICT)
    {
	emsg(_(e_dictreq));
	return;
    }
    if (argvars[0].vval.v_dict == NULL)
	return;

    rettv->vval.v_number = dict_find(argvars[0].vval.v_dict,
				      tv_get_string(&argvars[1]), -1) != NULL;
}

/*
 * "remove({dict})" function
 */
    void
dict_remove(typval_T *argvars, typval_T *rettv, char_u *arg_errmsg)
{
    dict_T	*d;
    char_u	*key;
    dictitem_T	*di;

    if (argvars[2].v_type != VAR_UNKNOWN)
	semsg(_(e_toomanyarg), "remove()");
    else if ((d = argvars[0].vval.v_dict) != NULL
	    && !var_check_lock(d->dv_lock, arg_errmsg, TRUE))
    {
	key = tv_get_string_chk(&argvars[1]);
	if (key != NULL)
	{
	    di = dict_find(d, key, -1);
	    if (di == NULL)
		semsg(_(e_dictkey), key);
	    else if (!var_check_fixed(di->di_flags, arg_errmsg, TRUE)
			&& !var_check_ro(di->di_flags, arg_errmsg, TRUE))
	    {
		*rettv = di->di_tv;
		init_tv(&di->di_tv);
		dictitem_remove(d, di);
	    }
	}
    }
}

#endif // defined(FEAT_EVAL)
