/*
 * plruby.c - ruby as a procedural language for PostgreSQL
 * copied from pltcl.c by Guy Decoux <ts@moulon.inra.fr>
 * 
 * You can redistribute it and/or modify it under the same term as
 * Ruby.
 *
 * Original copyright from pltcl.c
 */
/**********************************************************************
 * pltcl.c		- PostgreSQL support for Tcl as
 *			  procedural language (PL)
 *
 * IDENTIFICATION
 *	  $Header: /usr/local/cvsroot/pgsql/src/pl/tcl/pltcl.c,v 1.12 1999/05/26 12:57:23 momjian Exp $
 *
 *	  This software is copyrighted by Jan Wieck - Hamburg.
 *
 *	  The author hereby grants permission  to  use,  copy,	modify,
 *	  distribute,  and	license this software and its documentation
 *	  for any purpose, provided that existing copyright notices are
 *	  retained	in	all  copies  and  that	this notice is included
 *	  verbatim in any distributions. No written agreement, license,
 *	  or  royalty  fee	is required for any of the authorized uses.
 *	  Modifications to this software may be  copyrighted  by  their
 *	  author  and  need  not  follow  the licensing terms described
 *	  here, provided that the new terms are  clearly  indicated  on
 *	  the first page of each file where they apply.
 *
 *	  IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *	  PARTY  FOR  DIRECT,	INDIRECT,	SPECIAL,   INCIDENTAL,	 OR
 *	  CONSEQUENTIAL   DAMAGES  ARISING	OUT  OF  THE  USE  OF  THIS
 *	  SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *	  IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *	  DAMAGE.
 *
 *	  THE  AUTHOR  AND	DISTRIBUTORS  SPECIFICALLY	 DISCLAIM	ANY
 *	  WARRANTIES,  INCLUDING,  BUT	NOT  LIMITED  TO,  THE	IMPLIED
 *	  WARRANTIES  OF  MERCHANTABILITY,	FITNESS  FOR  A  PARTICULAR
 *	  PURPOSE,	AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *	  AN "AS IS" BASIS, AND THE AUTHOR	AND  DISTRIBUTORS  HAVE  NO
 *	  OBLIGATION   TO	PROVIDE   MAINTENANCE,	 SUPPORT,  UPDATES,
 *	  ENHANCEMENTS, OR MODIFICATIONS.
 *
 **********************************************************************/

#ifndef SAFE_LEVEL
#define SAFE_LEVEL 12
#endif

/* system stuff */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <setjmp.h>

/* postgreSQL stuff */
#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/elog.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "access/heapam.h"

#include "tcop/tcopprot.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_type.h"

#ifndef MAXFMGRARGS
#define RUBY_ARGS_MAXFMGR FUNC_MAX_ARGS
#define RUBY_TYPOID TYPEOID
#define RUBY_PROOID PROCOID
#define RUBY_TYPNAME TYPENAME
#else
#define RUBY_ARGS_MAXFMGR MAXFMGRARGS
#define RUBY_TYPOID TYPOID
#define RUBY_PROOID PROOID
#define RUBY_TYPNAME TYPNAME
#endif

#ifdef PG_FUNCTION_ARGS
#define NEW_STYLE_FUNCTION
#endif

#include <ruby.h>

enum { TG_OK, TG_SKIP };
enum { TG_BEFORE, TG_AFTER, TG_ROW, TG_STATEMENT, TG_INSERT,
       TG_DELETE, TG_UPDATE, TG_UNKNOWN }; 

static ID to_s_id;
   
/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct plruby_proc_desc
{
    char	   *proname;
    FmgrInfo	result_in_func;
    Oid			result_in_elem;
    int			result_in_len;
    int			nargs;
    FmgrInfo	arg_out_func[RUBY_ARGS_MAXFMGR];
    Oid			arg_out_elem[RUBY_ARGS_MAXFMGR];
    int			arg_out_len[RUBY_ARGS_MAXFMGR];
    int			arg_is_rel[RUBY_ARGS_MAXFMGR];
} plruby_proc_desc;

static void
plruby_proc_free(proc)
    plruby_proc_desc *proc;
{
    if (proc->proname)
	free(proc->proname);
    free(proc);
}
/**********************************************************************
 * The information we cache about prepared and saved plans
 **********************************************************************/
typedef struct plruby_query_desc
{
    char		qname[20];
    void	   *plan;
    int			nargs;
    Oid		   *argtypes;
    FmgrInfo   *arginfuncs;
    Oid		   *argtypelems;
    Datum	   *argvalues;
    int		   *arglen;
} plruby_query_desc;

/**********************************************************************
 * Global data
 **********************************************************************/
static int	plruby_firstcall = 1;
static int	plruby_call_level = 0;
static VALUE    pg_mPLruby, pg_mPLtemp, pg_cPLrubyPlan;
static VALUE    pg_ePLruby, pg_eCatch;
static VALUE    PLruby_hash;

static char *definition = "
def PLtemp.%s(%s)
    %s
end
";

/**********************************************************************
 * Forward declarations
 **********************************************************************/
static void plruby_init_all(void);

#ifdef NEW_STYLE_FUNCTION
Datum plruby_call_handler(PG_FUNCTION_ARGS);
static Datum plruby_func_handler(PG_FUNCTION_ARGS);
static HeapTuple plruby_trigger_handler(PG_FUNCTION_ARGS);
#else
Datum plruby_call_handler(FmgrInfo *proinfo,
			  FmgrValues *proargs, bool *isNull);

static Datum plruby_func_handler(FmgrInfo *proinfo,
				 FmgrValues *proargs, bool *isNull);

static HeapTuple plruby_trigger_handler(FmgrInfo *proinfo);
#endif

static VALUE plruby_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc,
					 int iterat);

static VALUE
plruby_protect(args)
    VALUE *args;
{
    Datum *retval;
    VALUE res;

    if (sigsetjmp(Warn_restart, 1) != 0)
	return pg_eCatch;
    res = Data_Make_Struct(rb_cObject, Datum, 0, free, retval);
    rb_obj_call_init(res, 0, 0);
#ifdef NEW_STYLE_FUNCTION
    if (CALLED_AS_TRIGGER((FunctionCallInfo)args))
	*retval = PointerGetDatum(plruby_trigger_handler((FunctionCallInfo)args));
    else
	*retval = plruby_func_handler((FunctionCallInfo)args);
#else
    if (CurrentTriggerData == NULL)
	*retval = plruby_func_handler((FmgrInfo *)args[0], 
				     (FmgrValues *)args[1],
				     (bool *)args[2]);
    else
	*retval = (Datum) plruby_trigger_handler((FmgrInfo *)args[0]);
#endif
    return res;
}

/**********************************************************************
 * plruby_call_handler		- This is the only visible function
 *				  of the PL interpreter. The PostgreSQL
 *				  function manager and trigger manager
 *				  call this function for execution of
 *				  ruby procedures.
 **********************************************************************/
Datum
#ifdef NEW_STYLE_FUNCTION
plruby_call_handler(PG_FUNCTION_ARGS)
#else
plruby_call_handler(FmgrInfo *proinfo,
		    FmgrValues *proargs,
		    bool *isNull)
#endif
{
    VALUE *args, c;
    sigjmp_buf save_restart;
    Datum *result;
    int state;

    if (plruby_firstcall)
	plruby_init_all();


    if (SPI_connect() != SPI_OK_CONNECT) {
	if (plruby_call_level)
	    rb_raise(pg_ePLruby, "cannot connect to SPI manager");
	else
	    elog(ERROR, "cannot connect to SPI manager");
    }


#ifdef NEW_STYLE_FUNCTION
    args = (VALUE *)fcinfo;
#else
    args = ALLOCA_N(VALUE, 3);
    args[0] = (VALUE)proinfo;
    args[1] = (VALUE)proargs;
    args[2] = (VALUE)isNull;
#endif

    state = 0;
    plruby_call_level++;
    memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
    c = rb_protect(plruby_protect, (VALUE)args, &state);
    memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
    plruby_call_level--;

    if (state) {
	c = rb_gv_get("$!");
    }
    if (c == pg_eCatch || rb_obj_is_kind_of(c, pg_eCatch)) {
	if (plruby_call_level)
	    rb_raise(pg_eCatch, "SPI ERROR");
	else
	    siglongjmp(Warn_restart, 1);
    }
    else if (rb_obj_is_kind_of(c, rb_eException)) {
	VALUE d = rb_funcall(c, to_s_id, 0);
	if (!plruby_call_level)
	    elog(ERROR, "%.*s", RSTRING(d)->len, RSTRING(d)->ptr);
	else
	    rb_raise(pg_ePLruby, "%.*s", RSTRING(d)->len, RSTRING(d)->ptr);
    }
    Data_Get_Struct(c, Datum, result);
    return *result;
}

/**********************************************************************
 * plruby_func_handler()		- Handler for regular function calls
 **********************************************************************/
static Datum
#ifdef NEW_STYLE_FUNCTION
plruby_func_handler(PG_FUNCTION_ARGS)
#else
plruby_func_handler(proinfo, proargs, isNull)
    FmgrInfo *proinfo;
    FmgrValues *proargs;
    bool *isNull;
#endif
{
    int			i;
    char		internal_proname[512];
    int		proname_len;
#ifndef NEW_STYLE_FUNCTION
    char	   *stroid;
#endif
    plruby_proc_desc *prodesc;
    VALUE value_proc_desc;
    Datum    retval;
    VALUE value_proname;
    VALUE rubyret;
    VALUE ary, c;
    static char *argf = "args";
    
#ifdef NEW_STYLE_FUNCTION
    sprintf(internal_proname, "proc_%u", fcinfo->flinfo->fn_oid);
#else
    stroid = oidout(proinfo->fn_oid);
    strcpy(internal_proname, "proc_");
    strcat(internal_proname, stroid);
    pfree(stroid);
#endif
    proname_len = strlen(internal_proname);

    value_proname = rb_tainted_str_new(internal_proname, proname_len);
    if ((value_proc_desc = rb_hash_aref(PLruby_hash, value_proname)) == Qnil) {
	HeapTuple	procTup;
	HeapTuple	typeTup;
	Form_pg_proc procStruct;
	Form_pg_type typeStruct;
	char		proc_internal_args[4096];
	char	   *proc_source;
	char *proc_internal_def;
	int status;

	value_proc_desc = Data_Make_Struct(rb_cObject, plruby_proc_desc, 0, plruby_proc_free, prodesc);
	rb_obj_call_init(value_proc_desc, 0, 0);
#ifdef NEW_STYLE_FUNCTION
	procTup = SearchSysCacheTuple(RUBY_PROOID,
				      ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
				      0, 0, 0);
#else
	procTup = SearchSysCacheTuple(RUBY_PROOID,
				      ObjectIdGetDatum(proinfo->fn_oid),
				      0, 0, 0);
#endif
	if (!HeapTupleIsValid(procTup))	{
	    rb_raise(pg_ePLruby, "cache lookup from pg_proc failed");
	}
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	
	typeTup = SearchSysCacheTuple(RUBY_TYPOID,
				      ObjectIdGetDatum(procStruct->prorettype),
				      0, 0, 0);
	if (!HeapTupleIsValid(typeTup))	{
	    rb_raise(pg_ePLruby, "cache lookup for return type failed");
	}
	typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

	if (typeStruct->typrelid != InvalidOid)	{
	    rb_raise(pg_ePLruby, "return types of tuples not supported yet");
	}

	fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
	prodesc->result_in_elem = (Oid) (typeStruct->typelem);
	prodesc->result_in_len = typeStruct->typlen;
	prodesc->nargs = procStruct->pronargs;
	proc_internal_args[0] = '\0';
	for (i = 0; i < prodesc->nargs; i++)	{
	    typeTup = SearchSysCacheTuple(RUBY_TYPOID,
					  ObjectIdGetDatum(procStruct->proargtypes[i]),
					  0, 0, 0);
	    if (!HeapTupleIsValid(typeTup)) {
		rb_raise(pg_ePLruby, "cache lookup for argument type failed");
	    }
	    typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

	    if (typeStruct->typrelid != InvalidOid)
		prodesc->arg_is_rel[i] = 1;
	    else
		prodesc->arg_is_rel[i] = 0;

	    fmgr_info(typeStruct->typoutput, &(prodesc->arg_out_func[i]));
	    prodesc->arg_out_elem[i] = (Oid) (typeStruct->typelem);
	    prodesc->arg_out_len[i] = typeStruct->typlen;

	}

#ifdef NEW_STYLE_FUNCTION
	proc_source = DatumGetCString(DirectFunctionCall1(textout,
							  PointerGetDatum(&procStruct->prosrc)));
#else
	proc_source = textout(&(procStruct->prosrc));
#endif
	proc_internal_def = ALLOCA_N(char, strlen(definition) + proname_len +
				     strlen(argf) + strlen(proc_source) + 1);
	sprintf(proc_internal_def, definition, internal_proname, argf, proc_source);
	pfree(proc_source);

	rb_eval_string_protect(proc_internal_def, &status);
	if (status) {
	    VALUE s = rb_funcall(rb_gv_get("$!"), to_s_id, 0);
	    rb_raise(pg_ePLruby, "cannot create internal procedure\n%s\n<<===%s\n===>>",
		 RSTRING(s)->ptr, proc_internal_def);
	}
	
	prodesc->proname = malloc(strlen(internal_proname) + 1);
	strcpy(prodesc->proname, internal_proname);
	rb_hash_aset(PLruby_hash, value_proname, value_proc_desc); 
    }

    Data_Get_Struct(value_proc_desc, plruby_proc_desc, prodesc);

    ary = rb_ary_new2(prodesc->nargs);
    for (i = 0; i < prodesc->nargs; i++) {
	if (prodesc->arg_is_rel[i]) {
#ifdef NEW_STYLE_FUNCTION
	    TupleTableSlot *slot = (TupleTableSlot *) fcinfo->arg[i];
#else
	    TupleTableSlot *slot = (TupleTableSlot *) proargs->data[i];
#endif
	    rb_ary_push(ary, plruby_build_tuple_argument(slot->val,
							 slot->ttc_tupleDescriptor, 
							 0));
	} 
	else {
#ifdef NEW_STYLE_FUNCTION
	    if (fcinfo->argnull[i]) {
		rb_ary_push(ary, Qnil);
 	    }
	    else {
#endif
		char *tmp;
#ifdef NEW_STYLE_FUNCTION
		tmp = DatumGetCString(FunctionCall3(&prodesc->arg_out_func[i],
						    fcinfo->arg[i],
						    ObjectIdGetDatum(prodesc->arg_out_elem[i]),
						    Int32GetDatum(prodesc->arg_out_len[i])));
#else
		tmp = (*fmgr_faddr(&(prodesc->arg_out_func[i])))
		    (proargs->data[i],
		     prodesc->arg_out_elem[i],
		     prodesc->arg_out_len[i]);
#endif
		rb_ary_push(ary, rb_tainted_str_new2(tmp));
		pfree(tmp);
	    }
	}
#ifdef NEW_STYLE_FUNCTION
    }
#endif

    c = rb_funcall(pg_mPLtemp, rb_intern(RSTRING(value_proname)->ptr), 1, ary);

    if (SPI_finish() != SPI_OK_FINISH)
	rb_raise(pg_ePLruby, "SPI_finish() failed");
    
    if (c == Qnil) {
#ifdef NEW_STYLE_FUNCTION
	PG_RETURN_NULL();
#else
	*isNull = true;
	return (Datum)0;
#endif
    }

    rubyret = rb_funcall(c, to_s_id, 0);

#ifdef NEW_STYLE_FUNCTION
    retval = FunctionCall3(&prodesc->result_in_func,
			   PointerGetDatum(RSTRING(rubyret)->ptr),
			   ObjectIdGetDatum(prodesc->result_in_elem),
			   Int32GetDatum(prodesc->result_in_len));
#else
    retval = (Datum) (*fmgr_faddr(&prodesc->result_in_func))
	(RSTRING(rubyret)->ptr,
	 prodesc->result_in_elem,
	 prodesc->result_in_len);
#endif
    return retval;

}

/**********************************************************************
 * plruby_build_tuple_argument() - Build a string for a ref to a hash
 *				  from all attributes of a given tuple
 **********************************************************************/
static VALUE
plruby_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc, int iterat)
{
    int			i;
    VALUE output;
    Datum		attr;
    bool		isnull;
    
    char	   *attname;
    char*     outputstr;
    HeapTuple	typeTup;
    Oid			typoutput;
    Oid			typelem;
    
    if (!iterat)
	output = rb_hash_new();
    else
	output = Qtrue;

    for (i = 0; i < tupdesc->natts; i++) {
#ifdef NEW_STYLE_FUNCTION
	attname = NameStr(tupdesc->attrs[i]->attname);
#else
	attname = tupdesc->attrs[i]->attname.data;
#endif
	attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);
	typeTup = SearchSysCacheTuple(RUBY_TYPOID,
				      ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
				      0, 0, 0);
	if (!HeapTupleIsValid(typeTup))	{
	    rb_raise(pg_ePLruby, "Cache lookup for attribute '%s' type %ld failed",
		 attname, ObjectIdGetDatum(tupdesc->attrs[i]->atttypid));
	}

	typoutput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typoutput);
	typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);
	if (!isnull && OidIsValid(typoutput)) {
	    VALUE s;
#ifdef NEW_STYLE_FUNCTION
	    outputstr = DatumGetCString(OidFunctionCall3(typoutput,
							 attr,
							 ObjectIdGetDatum(typelem),
							 Int32GetDatum(tupdesc->attrs[i]->attlen)));
#else
	    FmgrInfo	finfo;
	    
	    fmgr_info(typoutput, &finfo);
	    
	    outputstr = (*fmgr_faddr(&finfo))
		(attr, typelem,
		 tupdesc->attrs[i]->attlen);
#endif
	    s = rb_tainted_str_new2(outputstr);
	    pfree(outputstr);
	    if (iterat)
		rb_yield(rb_assoc_new(rb_tainted_str_new2(attname), s));
	    else
		rb_hash_aset(output, rb_tainted_str_new2(attname), s);
	} 
	else {
	    if (isnull) {
		if (iterat)
		    rb_yield(rb_assoc_new(rb_tainted_str_new2(attname), Qnil));
		else
		    rb_hash_aset(output, rb_tainted_str_new2(attname), Qnil);
	    }
	}
    }
    return output;
}

/**********************************************************************
 * plruby_trigger_handler()	- Handler for trigger calls
 **********************************************************************/
struct foreach_fmgr {
    TupleDesc	tupdesc;
    int		   *modattrs;
    Datum	   *modvalues;
    char	   *modnulls;
}; 

static VALUE
for_numvals(obj, argobj)
    VALUE obj, argobj;
{
    int			attnum;
    HeapTuple	typeTup;
    Oid			typinput;
    Oid			typelem;
    FmgrInfo	finfo;
    VALUE key, value;
    struct foreach_fmgr *arg;

    Data_Get_Struct(argobj, struct foreach_fmgr, arg);
    key = rb_funcall(rb_ary_entry(obj, 0), to_s_id, 0);
    value = rb_funcall(rb_ary_entry(obj, 1), to_s_id, 0);

    /************************************************************
     * Ignore pseudo elements with a dot name
     ************************************************************/
    if ((RSTRING(key)->ptr)[0]  == '.')
	return Qnil;

    /************************************************************
     * Special case for NULL values
     ************************************************************/
    if (NIL_P(value))
	return Qnil;

    attnum = SPI_fnumber(arg->tupdesc, RSTRING(key)->ptr);
    if (attnum == SPI_ERROR_NOATTRIBUTE)
	rb_raise(pg_ePLruby, "invalid attribute '%s'", RSTRING(key)->ptr);
    typeTup = SearchSysCacheTuple(RUBY_TYPOID,
				  ObjectIdGetDatum(arg->tupdesc->attrs[attnum - 1]->atttypid),
				  0, 0, 0);
    if (!HeapTupleIsValid(typeTup)) {	
	rb_raise(pg_ePLruby, "Cache lookup for attribute '%s' type %ld failed",
	     RSTRING(key)->ptr,
	     ObjectIdGetDatum(arg->tupdesc->attrs[attnum - 1]->atttypid));
    }
    typinput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typinput);
    typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);

    /************************************************************
     * Set the attribute to NOT NULL and convert the contents
     ************************************************************/
    arg->modnulls[attnum - 1] = ' ';
    fmgr_info(typinput, &finfo);
#ifdef NEW_STYLE_FUNCTION
    arg->modvalues[attnum - 1] =
	FunctionCall3(&finfo,
		      CStringGetDatum(RSTRING(value)->ptr),
		      ObjectIdGetDatum(typelem),
		      Int32GetDatum((!VARLENA_FIXED_SIZE(arg->tupdesc->attrs[attnum - 1]))
		      ? arg->tupdesc->attrs[attnum - 1]->attlen
		      : arg->tupdesc->attrs[attnum - 1]->atttypmod));
#else
    arg->modvalues[attnum - 1] = (Datum) (*fmgr_faddr(&finfo))
	(RSTRING(value)->ptr,
	 typelem,
	 (!VARLENA_FIXED_SIZE(arg->tupdesc->attrs[attnum - 1]))
	 ? arg->tupdesc->attrs[attnum - 1]->attlen
	 : arg->tupdesc->attrs[attnum - 1]->atttypmod
	    );
#endif
    return Qnil;
}
 

static HeapTuple
#ifdef NEW_STYLE_FUNCTION
plruby_trigger_handler(PG_FUNCTION_ARGS)
#else
plruby_trigger_handler(FmgrInfo *proinfo)
#endif
{
    TriggerData *trigdata;
    char		internal_proname[512];
    char	   *stroid;
    plruby_proc_desc *prodesc;
    TupleDesc	tupdesc;
    volatile HeapTuple	rettup;
    int			i;
    int		   *modattrs;
    Datum	   *modvalues;
    char	   *modnulls;
    VALUE tg_new, tg_old, args, TG, c, tmp;
    int proname_len, status;
    VALUE value_proname, value_proc_desc;
    char *proc_internal_def;
    static char *argt = "new, old, args, tg";
    
#ifdef NEW_STYLE_FUNCTION
    trigdata = (TriggerData *) fcinfo->context;

    sprintf(internal_proname, "proc_%u", fcinfo->flinfo->fn_oid);
#else
    trigdata = CurrentTriggerData;
    CurrentTriggerData = NULL;

    stroid = oidout(proinfo->fn_oid);
    strcpy(internal_proname, "proc_");
    strcat(internal_proname, stroid);
    pfree(stroid);
#endif
    proname_len = strlen(internal_proname);

    value_proname = rb_tainted_str_new(internal_proname, proname_len);
    if ((value_proc_desc = rb_hash_aref(PLruby_hash, value_proname)) == Qnil) {
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char	   *proc_source;
	
	value_proc_desc = Data_Make_Struct(rb_cObject, plruby_proc_desc, 0, plruby_proc_free, prodesc);
	rb_obj_call_init(value_proc_desc, 0, 0);

#ifdef NEW_STYLE_FUNCTION
	procTup = SearchSysCacheTuple(RUBY_PROOID,
				      ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
				      0, 0, 0);
#else
	procTup = SearchSysCacheTuple(RUBY_PROOID,
				      ObjectIdGetDatum(proinfo->fn_oid),
				      0, 0, 0);
#endif
	if (!HeapTupleIsValid(procTup))
	    rb_raise(pg_ePLruby, "cache lookup from pg_proc failed");
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

#ifdef NEW_STYLE_FUNCTION
	proc_source = DatumGetCString(DirectFunctionCall1(textout,
							  PointerGetDatum(&procStruct->prosrc)));
#else
	proc_source = textout(&(procStruct->prosrc));
#endif
	proc_internal_def = ALLOCA_N(char, strlen(definition) + proname_len +
				     strlen(argt) + strlen(proc_source) + 1);
	sprintf(proc_internal_def, definition, internal_proname, argt, proc_source);
	pfree(proc_source);

	rb_eval_string_protect(proc_internal_def, &status);
	if (status) {
	    VALUE s = rb_funcall(rb_gv_get("$!"), to_s_id, 0);
	    rb_raise(pg_ePLruby, "cannot create bb internal procedure %s\n<<===%s\n===>>",
		 RSTRING(s)->ptr, proc_internal_def);
	}
	prodesc->proname = malloc(strlen(internal_proname) + 1);
	strcpy(prodesc->proname, internal_proname);
	rb_hash_aset(PLruby_hash, value_proname, value_proc_desc); 
    }
    Data_Get_Struct(value_proc_desc, plruby_proc_desc, prodesc);

    tupdesc = trigdata->tg_relation->rd_att;

    TG = rb_hash_new();

    rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("name")), 
		 rb_str_freeze(rb_tainted_str_new2(trigdata->tg_trigger->tgname)));

    rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("relname")), 
		 rb_str_freeze(rb_tainted_str_new2(nameout(&(trigdata->tg_relation->rd_rel->relname)))));

#ifdef NEW_STYLE_FUNCTION
    stroid = DatumGetCString(DirectFunctionCall1(oidout,
						 ObjectIdGetDatum(trigdata->tg_relation->rd_id)));
#else
    stroid = oidout(trigdata->tg_relation->rd_id);
#endif
    rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("relid")), 
		 rb_str_freeze(rb_tainted_str_new2(stroid)));
    pfree(stroid);

    tmp = rb_ary_new2(tupdesc->natts);
    for (i = 0; i < tupdesc->natts; i++)
	rb_ary_push(tmp, rb_str_freeze(rb_tainted_str_new2(tupdesc->attrs[i]->attname.data)));
    rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("relatts")), 
		 rb_ary_freeze(tmp));

    if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("when")), INT2FIX(TG_BEFORE)); 
    else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("when")), INT2FIX(TG_AFTER)); 
    else
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("when")), INT2FIX(TG_UNKNOWN)); 
    
    if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("level")),INT2FIX(TG_ROW));
    else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("level")), INT2FIX(TG_STATEMENT)); 
    else
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("level")), INT2FIX(TG_UNKNOWN)); 

    if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event)) {
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("op")), INT2FIX(TG_INSERT));
	tg_new = plruby_build_tuple_argument(trigdata->tg_trigtuple, tupdesc, 0);
	tg_old = rb_ary_new2(0);
	rettup = trigdata->tg_trigtuple;
    }
    else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) {
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("op")), INT2FIX(TG_DELETE));
	tg_old = plruby_build_tuple_argument(trigdata->tg_trigtuple, tupdesc, 0);
	tg_new = rb_ary_new2(0);

	rettup = trigdata->tg_trigtuple;
    }
    else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event)) {
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("op")), INT2FIX(TG_UPDATE)); 
	tg_new = plruby_build_tuple_argument(trigdata->tg_newtuple, tupdesc, 0);
	tg_old = plruby_build_tuple_argument(trigdata->tg_trigtuple, tupdesc, 0);
	rettup = trigdata->tg_newtuple;
    }
    else {
	rb_hash_aset(TG, rb_str_freeze(rb_tainted_str_new2("op")), INT2FIX(TG_UNKNOWN));
	tg_new = plruby_build_tuple_argument(trigdata->tg_trigtuple, tupdesc, 0);
	tg_old = plruby_build_tuple_argument(trigdata->tg_trigtuple, tupdesc, 0);
	rettup = trigdata->tg_trigtuple;
    }
    rb_hash_freeze(TG);

    args = rb_ary_new2(trigdata->tg_trigger->tgnargs);
    for (i = 0; i < trigdata->tg_trigger->tgnargs; i++)
	rb_ary_push(args, rb_str_freeze(rb_tainted_str_new2(trigdata->tg_trigger->tgargs[i])));
    rb_ary_freeze(args);

    c = rb_funcall(pg_mPLtemp, rb_intern(RSTRING(value_proname)->ptr),
		   4, tg_new, tg_old, args, TG);

    if (SPI_finish() != SPI_OK_FINISH)
	rb_raise(pg_ePLruby, "SPI_finish() failed");

    switch (TYPE(c)) {
    case T_TRUE:
	return rettup;
	break;
    case T_FALSE:
	return (HeapTuple) NULL;
	break;
    case T_FIXNUM:
	if (NUM2INT(c) == TG_OK) {
	    return rettup;
	}
	if (NUM2INT(c) == TG_SKIP) {
	    return (HeapTuple) NULL;
	}
	rb_raise(pg_ePLruby, "Invalid return code");
	break;
    case T_STRING:
	if (strcmp(RSTRING(c)->ptr, "OK") == 0) {
	    return rettup;
	}
	if (strcmp(RSTRING(c)->ptr, "SKIP") == 0) {
	    return (HeapTuple) NULL;
	}
	rb_raise(pg_ePLruby, "unknown response %s", RSTRING(c)->ptr);
	break;
    case T_HASH:
	break;
    default:
	rb_raise(pg_ePLruby, "Invalid return value");
	break;
    }

    modattrs = (int *) palloc(tupdesc->natts * sizeof(int));
    modvalues = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
    for (i = 0; i < tupdesc->natts; i++) {
	modattrs[i] = i + 1;
	modvalues[i] = (Datum) NULL;
    }

    modnulls = palloc(tupdesc->natts + 1);
    memset(modnulls, 'n', tupdesc->natts);
    modnulls[tupdesc->natts] = '\0';
    {
	struct foreach_fmgr *mgr;
	VALUE res;

	res = Data_Make_Struct(rb_cObject, struct foreach_fmgr, 0, free, mgr);
	mgr->tupdesc = tupdesc;
	mgr->modattrs = modattrs;
	mgr->modvalues = modvalues;
	mgr->modnulls = modnulls;
	rb_iterate(rb_each, c, for_numvals, res);
    }

    rettup = SPI_modifytuple(trigdata->tg_relation, rettup, tupdesc->natts,
			     modattrs, modvalues, modnulls);

    pfree(modattrs);
    pfree(modvalues);
    pfree(modnulls);
    
    if (rettup == NULL)
	rb_raise(pg_ePLruby, "SPI_modifytuple() failed - RC = %d\n", SPI_result);

    return rettup;
}

static VALUE
plruby_warn(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    int level, indice;

    level = NOTICE;
    indice = 0;
    switch (argc) {
    case 2:
	indice  = 1;
	switch (level = NUM2INT(argv[0])) {
	case NOTICE:
	case DEBUG:
	case NOIND:
	    break;
	default:
	    rb_raise(pg_ePLruby, "invalid level %d", level);
	}
    case 1:
	if (NIL_P(argv[indice]))
	    return Qnil;
	if (TYPE(argv[indice]) != T_STRING)
	    rb_raise(pg_ePLruby, "warn: string expected");
	break;
    default:
	rb_raise(pg_ePLruby, "invalid syntax");
    }
    elog(level, RSTRING(argv[indice])->ptr);
    return Qnil;
}

static VALUE
plruby_quote(obj, mes)
    VALUE obj, mes;
{    
    char	   *tmp;
    char	   *cp1;
    char	   *cp2;

    if (TYPE(mes) != T_STRING)
	rb_raise(pg_ePLruby, "quote: string expected");
    tmp = ALLOCA_N(char, RSTRING(mes)->len * 2 + 1);
    cp1 = RSTRING(mes)->ptr;
    cp2 = tmp;
    /************************************************************
     * Walk through string and double every quote and backslash
     ************************************************************/
    while (*cp1) {
	if (*cp1 == '\'')
	    *cp2++ = '\'';
	else {
	    if (*cp1 == '\\')
		*cp2++ = '\\';
	}
	*cp2++ = *cp1++;
    }

    /************************************************************
     * Terminate the string and set it as result
     ************************************************************/
    *cp2 = '\0';
    return rb_tainted_str_new2(tmp);
}

/**********************************************************************
 * plruby_SPI_exec()		- The builtin SPI_exec command
 *				  for the safe interpreter
 **********************************************************************/
static VALUE
plruby_SPI_exec(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    int			spi_rc;
    volatile int			count = 0;
    int			i;
    int			ntuples;
    VALUE a, b, result;
    HeapTuple  *tuples;
    TupleDesc	tupdesc = NULL;

    if (rb_scan_args(argc, argv, "11", &a, &b) == 2)
	count = NUM2INT(b);
    if (TYPE(a) != T_STRING)
	rb_raise(pg_ePLruby, "exec: first argument must be a string");

    spi_rc = SPI_exec(RSTRING(a)->ptr, count);

    switch (spi_rc) {
    case SPI_OK_UTILITY:
	return Qtrue;
    case SPI_OK_SELINTO:
    case SPI_OK_INSERT:
    case SPI_OK_DELETE:
    case SPI_OK_UPDATE:
	return INT2NUM(SPI_processed);
    case SPI_OK_SELECT:
	break;
    case SPI_ERROR_ARGUMENT:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_ARGUMENT");
    case SPI_ERROR_UNCONNECTED:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_UNCONNECTED");
    case SPI_ERROR_COPY:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_COPY");
    case SPI_ERROR_CURSOR:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_CURSOR");
    case SPI_ERROR_TRANSACTION:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_TRANSACTION");
    case SPI_ERROR_OPUNKNOWN:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_OPUNKNOWN");
    default:
	rb_raise(pg_ePLruby, "SPI_exec() failed - unknown RC %d", spi_rc);
    }

    ntuples = SPI_processed;
    if (ntuples <= 0) {
	if (rb_iterator_p() || count == 1)
	    return Qfalse;
	else
	    return rb_ary_new2(0);
    }
    tuples = SPI_tuptable->vals;
    tupdesc = SPI_tuptable->tupdesc;
    if (rb_iterator_p()) {
	if (count == 1)
	    plruby_build_tuple_argument(tuples[0], tupdesc, 1);
	else {
	    for (i = 0; i < ntuples; i++)
		rb_yield(plruby_build_tuple_argument(tuples[i], tupdesc, 0));
	}
	result = Qtrue;
    }
    else {
	if (count == 1)
	    result = plruby_build_tuple_argument(tuples[0], tupdesc, 0);
	else {
	    result = rb_ary_new2(ntuples);
	    for (i = 0; i < ntuples; i++)
		rb_ary_push(result, plruby_build_tuple_argument(tuples[i], tupdesc, 0));
	}
    }
    return result;
}

/**********************************************************************
 * plruby_SPI_prepare()		- Builtin support for prepared plans
 *				  The Ruby command SPI_prepare
 *				  allways saves the plan using
 *				  SPI_saveplan and returns a key for
 *				  access. There is no chance to prepare
 *				  and not save the plan currently.
 **********************************************************************/
static void
plruby_query_free(qdesc)
    plruby_query_desc *qdesc;
{
    int j;
    if (qdesc->argtypes)
	free(qdesc->argtypes);
    if (qdesc->arginfuncs)
	free(qdesc->arginfuncs);
    if (qdesc->argtypelems)
	free(qdesc->argtypelems);
    if (qdesc->argvalues) {
	for (j = 0; j < qdesc->nargs; j++) {
	    if (qdesc->arglen[j] < 0 &&
		qdesc->argvalues[j] != (Datum) NULL) {
		free((char *)qdesc->argvalues[j]);
	    }
	}
	free(qdesc->argvalues);
    }
    if (qdesc->arglen)
	free(qdesc->arglen);
    free(qdesc);
}

static VALUE
plruby_SPI_prepare(obj, a, b)
    VALUE obj, a, b;
{
    int			nargs;
    plruby_query_desc *qdesc;
    void	   *plan;
    int			i;
    HeapTuple	typeTup;
    VALUE result;

    if (TYPE(a) != T_STRING)
	rb_raise(pg_ePLruby, "first argument must be a STRING");
    if (TYPE(b) != T_ARRAY)
	rb_raise(pg_ePLruby, "second argument must be an ARRAY");

    nargs = RARRAY(b)->len;

    result = Data_Make_Struct(pg_cPLrubyPlan, plruby_query_desc, 0, plruby_query_free, qdesc);
    rb_obj_call_init(result, 0, 0);
    sprintf(qdesc->qname, "%lx", (long) qdesc);
    qdesc->nargs = nargs;
    qdesc->argtypes = ALLOC_N(Oid, nargs);
    qdesc->arginfuncs = ALLOC_N(FmgrInfo ,nargs);
    qdesc->argtypelems = ALLOC_N(Oid ,nargs);
    qdesc->argvalues = ALLOC_N(Datum, nargs);
    qdesc->arglen = ALLOC_N(int, nargs);

    for (i = 0; i < nargs; i++)	{
	VALUE args = rb_funcall(RARRAY(b)->ptr[i], to_s_id, 0);
	typeTup = SearchSysCacheTuple(RUBY_TYPNAME,
				      PointerGetDatum(RSTRING(args)->ptr),
				      0, 0, 0);
	if (!HeapTupleIsValid(typeTup)) {
	    rb_raise(pg_ePLruby, "Cache lookup of type '%s' failed", RSTRING(args)->ptr);
	}
	qdesc->argtypes[i] = typeTup->t_data->t_oid;
	fmgr_info(((Form_pg_type) GETSTRUCT(typeTup))->typinput,
		  &(qdesc->arginfuncs[i]));
	qdesc->argtypelems[i] = ((Form_pg_type) GETSTRUCT(typeTup))->typelem;
	qdesc->argvalues[i] = (Datum) NULL;
	qdesc->arglen[i] = (int) (((Form_pg_type) GETSTRUCT(typeTup))->typlen);
    }

    plan = SPI_prepare(RSTRING(a)->ptr, nargs, qdesc->argtypes);

    if (plan == NULL) {
	char		buf[128];
	char	   *reason;

	switch (SPI_result) {
	case SPI_ERROR_ARGUMENT:
	    reason = "SPI_ERROR_ARGUMENT";
	    break;
	case SPI_ERROR_UNCONNECTED:
	    reason = "SPI_ERROR_UNCONNECTED";
	    break;
	case SPI_ERROR_COPY:
	    reason = "SPI_ERROR_COPY";
	    break;
	case SPI_ERROR_CURSOR:
	    reason = "SPI_ERROR_CURSOR";
	    break;
	case SPI_ERROR_TRANSACTION:
	    reason = "SPI_ERROR_TRANSACTION";
	    break;
	case SPI_ERROR_OPUNKNOWN:
	    reason = "SPI_ERROR_OPUNKNOWN";
	    break;
	default:
	    sprintf(buf, "unknown RC %d", SPI_result);
	    reason = buf;
	    break;
	}
	rb_raise(pg_ePLruby, "SPI_prepare() failed - %s", reason);
    }

    qdesc->plan = SPI_saveplan(plan);
    if (qdesc->plan == NULL) {
	char		buf[128];
	char	   *reason;

	switch (SPI_result) {
	case SPI_ERROR_ARGUMENT:
	    reason = "SPI_ERROR_ARGUMENT";
	    break;
	case SPI_ERROR_UNCONNECTED:
	    reason = "SPI_ERROR_UNCONNECTED";
	    break;
	default:
	    sprintf(buf, "unknown RC %d", SPI_result);
	    reason = buf;
	    break;
	}
	rb_raise(pg_ePLruby, "SPI_saveplan() failed - %s", reason);
    }
    return result;
}

/**********************************************************************
 * plruby_SPI_execp()		- Execute a prepared plan
 **********************************************************************/
static VALUE
plruby_SPI_execp(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    int			spi_rc;
    int			i, j;
    VALUE result;
    plruby_query_desc *qdesc;
    char	   *nulls = NULL;
    volatile int	count;
    volatile int	callnargs;
    int			ntuples;
    HeapTuple  *tuples = NULL;
    TupleDesc	tupdesc = NULL;
    VALUE argsv, countv;

    count = 0;
    Data_Get_Struct(obj, plruby_query_desc, qdesc);

    if (rb_scan_args(argc, argv, "11", &argsv, &countv) == 2) {
	count = NUM2INT(countv);
    }
    
    if (qdesc->nargs > 0) {
	if (TYPE(argsv) != T_ARRAY)
	    rb_raise(pg_ePLruby, "array expected for arguments");
	if (RARRAY(argsv)->len != qdesc->nargs)
	    rb_raise(pg_ePLruby, "length of arguments doesn't match # of arguments");
	callnargs = RARRAY(argsv)->len;
	nulls = ALLOCA_N(char, callnargs + 1);
	for (j = 0; j < callnargs; j++) {
	    if (qdesc->arglen[j] < 0 &&	qdesc->argvalues[j] != (Datum) NULL) {
		pfree((char *) (qdesc->argvalues[j]));
		qdesc->argvalues[j] = (Datum) NULL;
	    }
	}

	for (j = 0; j < callnargs; j++)	{
	    if (NIL_P(RARRAY(argsv)->ptr[j])) {
		nulls[j] = 'n';
		qdesc->argvalues[j] = (Datum)NULL;
	    }
	    else {
		VALUE args = rb_funcall(RARRAY(argsv)->ptr[j], to_s_id, 0);
		nulls[j] = ' ';
#ifdef NEW_STYLE_FUNCTION
		qdesc->argvalues[j] =
		    FunctionCall3(&qdesc->arginfuncs[j],
				  CStringGetDatum(RSTRING(args)->ptr),
				  ObjectIdGetDatum(qdesc->argtypelems[j]),
				  Int32GetDatum(qdesc->arglen[j]));
#else
		qdesc->argvalues[j] = (Datum) (*fmgr_faddr(&qdesc->arginfuncs[j]))
		    (RSTRING(args)->ptr, qdesc->argtypelems[j], qdesc->arglen[j]);
#endif
	    }
	}
	nulls[callnargs] = '\0';
    }
    else
	callnargs = 0;

    spi_rc = SPI_execp(qdesc->plan, qdesc->argvalues, nulls, count);

    for (j = 0; j < callnargs; j++) {
	if (qdesc->arglen[j] < 0 && qdesc->argvalues[j] != (Datum) NULL) {
	    pfree((char *) (qdesc->argvalues[j]));
	    qdesc->argvalues[j] = (Datum) NULL;
	}
    }

    switch (spi_rc) {
    case SPI_OK_UTILITY:
	return Qtrue;
    case SPI_OK_SELINTO:
    case SPI_OK_INSERT:
    case SPI_OK_DELETE:
    case SPI_OK_UPDATE:
	return INT2NUM(SPI_processed);
    case SPI_OK_SELECT:
	break;

    case SPI_ERROR_ARGUMENT:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_ARGUMENT");
    case SPI_ERROR_UNCONNECTED:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_UNCONNECTED");
    case SPI_ERROR_COPY:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_COPY");
    case SPI_ERROR_CURSOR:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_CURSOR");
    case SPI_ERROR_TRANSACTION:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_TRANSACTION");
    case SPI_ERROR_OPUNKNOWN:
	rb_raise(pg_ePLruby, "SPI_exec() failed - SPI_ERROR_OPUNKNOWN");
    default:
	rb_raise(pg_ePLruby, "SPI_exec() failed - unknown RC %d", spi_rc);
    }
    
    ntuples = SPI_processed;
    if (ntuples <= 0) {
	if (rb_iterator_p() || count == 1)
	    return Qfalse;
	else
	    return rb_ary_new2(0);
    }
    tuples = SPI_tuptable->vals;
    tupdesc = SPI_tuptable->tupdesc;
    if (rb_iterator_p()) {
	if (count == 1)
	    plruby_build_tuple_argument(tuples[0], tupdesc, 1);
	else {
	    for (i = 0; i < ntuples; i++)
		rb_yield(plruby_build_tuple_argument(tuples[i], tupdesc, 0));
	}
	result = Qtrue;
    }
    else {
	if (count == 1)
	    result = plruby_build_tuple_argument(tuples[0], tupdesc, 0);
	else {
	    result = rb_ary_new2(ntuples);
	    for (i = 0; i < ntuples; i++)
		rb_ary_push(result, plruby_build_tuple_argument(tuples[i], tupdesc, 0));
	}
    }
    return result;
}

static int
plruby_exist_singleton()
{
    int spi_rc;

    spi_rc = SPI_exec("select 1 from pg_class where relname = 'plruby_singleton_methods'", 1);
    if (spi_rc != SPI_OK_SELECT || SPI_processed == 0)
	return 0;
    spi_rc = SPI_exec("select name from plruby_singleton_methods", 0);
    if (spi_rc != SPI_OK_SELECT || SPI_processed == 0)
	return 0;
    return SPI_processed;
}

static char *recherche = 
    "select name, args, body from plruby_singleton_methods where name = '%s'";

static VALUE
plruby_each(tmp)
    VALUE *tmp;
{
    rb_funcall2(pg_mPLtemp, (ID)tmp[0], (int)tmp[1], (VALUE *)tmp[2]);
}

static VALUE
plruby_yield(i, a)
    VALUE i, a;
{
    rb_ary_push(a, rb_yield(i));
}

static VALUE
plruby_load_singleton(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    int spi_rc, status;
    ID id;
    char *nom, *buff;
    int fname, fargs, fbody;
    char *name, *args, *body;
    char *sinm;

    if (argc == 0) 
	rb_raise(rb_eArgError, "no id given");
 
    id = SYM2ID(argv[0]);
    argc--; argv++;
    nom = rb_id2name(id);
    buff = ALLOCA_N(char, 1 + strlen(recherche) + strlen(nom));
    sprintf(buff, recherche, nom);
    spi_rc = SPI_exec(buff, 0);
    if (spi_rc != SPI_OK_SELECT || SPI_processed == 0)
	rb_raise(rb_eNameError, "undefined method `%s' for PLtemp:Module", nom);
    fname = SPI_fnumber(SPI_tuptable->tupdesc, "name");
    fargs = SPI_fnumber(SPI_tuptable->tupdesc, "args");
    fbody = SPI_fnumber(SPI_tuptable->tupdesc, "body");
    name = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, fname);
    args = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, fargs);
    body = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, fbody);
    sinm = ALLOCA_N(char, 1 + strlen(definition) + strlen(name) + 
		    strlen(args) + strlen(body));
    sprintf(sinm, definition, name, args, body);
    rb_eval_string_protect(sinm, &status);
    if (status) {
	VALUE s = rb_funcall(rb_gv_get("$!"), to_s_id, 0);
	rb_raise(pg_ePLruby, "cannot create internal procedure\n%s\n<<===%s\n===>>",
		 RSTRING(s)->ptr, sinm);
    }
    if (rb_iterator_p()) {
	VALUE tmp[3], res;
	tmp[0] = (VALUE)id;
	tmp[1] = (VALUE)argc;
	tmp[2] = (VALUE)argv;
	res = rb_ary_new();
	rb_iterate(plruby_each, (VALUE)tmp, plruby_yield, res);
	return res;
    }
    else {
	return rb_funcall2(pg_mPLtemp, id, argc, argv);
    }
}

/**********************************************************************
 * plruby_init_all()		- Initialize all
 **********************************************************************/
static VALUE plans;


static void
plruby_init_all(void)
{
    if (!plruby_firstcall)
	return;
#if SAFE_LEVEL >= 1
    rb_set_safe_level(1);
#endif
    ruby_init();
    rb_define_global_const("NOTICE", INT2FIX(NOTICE));
    rb_define_global_const("DEBUG", INT2FIX(DEBUG));
    rb_define_global_const("NOIND", INT2FIX(NOIND));
    pg_mPLruby = rb_define_module("PLruby");
    pg_ePLruby = rb_define_class("PLrubyError", rb_eStandardError);
    pg_eCatch = rb_define_class("PLrubyCatch", rb_eStandardError);
    rb_define_global_function("warn", plruby_warn, -1);
    rb_define_module_function(pg_mPLruby, "quote", plruby_quote, 1);
    rb_define_module_function(pg_mPLruby, "spi_exec", plruby_SPI_exec, -1);
    rb_define_module_function(pg_mPLruby, "exec", plruby_SPI_exec, -1);
    rb_define_const(pg_mPLruby, "OK", INT2FIX(TG_OK));
    rb_define_const(pg_mPLruby, "SKIP", INT2FIX(TG_SKIP));
    rb_define_const(pg_mPLruby, "BEFORE", INT2FIX(TG_BEFORE)); 
    rb_define_const(pg_mPLruby, "AFTER", INT2FIX(TG_AFTER)); 
    rb_define_const(pg_mPLruby, "ROW", INT2FIX(TG_ROW)); 
    rb_define_const(pg_mPLruby, "STATEMENT", INT2FIX(TG_STATEMENT)); 
    rb_define_const(pg_mPLruby, "INSERT", INT2FIX(TG_INSERT));
    rb_define_const(pg_mPLruby, "DELETE", INT2FIX(TG_DELETE)); 
    rb_define_const(pg_mPLruby, "UPDATE", INT2FIX(TG_UPDATE));
    rb_define_const(pg_mPLruby, "UNKNOWN", INT2FIX(TG_UNKNOWN));
    pg_cPLrubyPlan = rb_define_class("PLrubyPlan", rb_cObject);
    rb_undef_method(CLASS_OF(pg_cPLrubyPlan), "new");
    rb_define_module_function(pg_mPLruby, "spi_prepare", plruby_SPI_prepare, 2);
    rb_define_module_function(pg_mPLruby, "prepare", plruby_SPI_prepare, 2);
    rb_define_method(pg_cPLrubyPlan, "spi_execp", plruby_SPI_execp, -1);
    rb_define_method(pg_cPLrubyPlan, "execp", plruby_SPI_execp, -1);
    rb_define_method(pg_cPLrubyPlan, "exec", plruby_SPI_execp, -1);
    to_s_id = rb_intern("to_s");
    rb_set_safe_level(SAFE_LEVEL);
    plans = rb_hash_new();
    rb_define_variable("$Plans", &plans);
    pg_mPLtemp = rb_define_module("PLtemp");
    PLruby_hash = rb_hash_new();
    rb_global_variable(&PLruby_hash);
    if (SPI_connect() != SPI_OK_CONNECT)
	elog(ERROR, "plruby_singleton_methods : SPI_connect failed");
    if (plruby_exist_singleton())
	rb_define_module_function(pg_mPLtemp, "method_missing", plruby_load_singleton, -1);
    if (SPI_finish() != SPI_OK_FINISH)
	elog(ERROR, "plruby_singleton_methods : SPI_finish failed");
    plruby_firstcall = 0;
    return;
}
