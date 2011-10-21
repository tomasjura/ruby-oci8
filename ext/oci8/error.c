/* -*- c-file-style: "ruby"; indent-tabs-mode: nil -*- */
/*
  error.c - part of ruby-oci8

  Copyright (C) 2002-2011 KUBO Takehiro <kubo@jiubao.org>

*/
#include "oci8.h"

#ifndef DLEXT
#define DLEXT ".so"
#endif

/* Exception */
VALUE eOCIException;
VALUE eOCIBreak;
static VALUE eOCINoData;
static VALUE eOCIError;
static VALUE eOCIInvalidHandle;
static VALUE eOCINeedData;
static VALUE eOCIStillExecuting;
static VALUE eOCIContinue;
static VALUE eOCISuccessWithInfo;

static ID oci8_id_at_code;
static ID oci8_id_at_sql;
static ID oci8_id_at_parse_error_offset;
static ID oci8_id_caller;
static ID oci8_id_set_backtrace;

#define ERRBUF_EXPAND_LEN 256
static char *errbuf;
static ub4 errbufsiz;

static OCIMsg *msghp;

static VALUE set_backtrace(VALUE exc, const char *file, int line);

static VALUE get_error_msg(dvoid *errhp, ub4 type, const char *default_msg, sb4 *errcode_p)
{
    sword rv;
    size_t len;

retry:
    errbuf[0] = '\0';
    rv = OCIErrorGet(errhp, 1, NULL, errcode_p, TO_ORATEXT(errbuf), errbufsiz, type);
    /* OCI manual says:
     *   If type is set to OCI_HTYPE_ERROR, then the return
     *   code during truncation for OCIErrorGet() is
     *   OCI_ERROR. The client can then specify a bigger
     *   buffer and call OCIErrorGet() again.
     *
     * But as far as I tested on Oracle XE 10.2.0.1, the return
     * code is OCI_SUCCESS when the message is truncated.
     */
    len = strlen(errbuf);
    if (errbufsiz - len <= 7) {
        /* The error message may be truncated.
         * The magic number 7 means the maximum length of one utf-8
         * character plus the length of a nul terminator.
         */
        errbufsiz += ERRBUF_EXPAND_LEN;
        errbuf = xrealloc(errbuf, errbufsiz);
        goto retry;
    }
    if (rv != OCI_SUCCESS) {
        /* No message is found. Use the default message. */
        return rb_usascii_str_new_cstr(default_msg);
    }

    /* truncate trailing CR and LF */
    while (len > 0 && (errbuf[len - 1] == '\n' || errbuf[len - 1] == '\r')) {
        len--;
    }
    return rb_external_str_new_with_enc(errbuf, len, oci8_encoding);
}

VALUE oci8_make_exc(dvoid *errhp, sword status, ub4 type, OCIStmt *stmthp, const char *file, int line)
{
    VALUE exc;
    char errmsg[128];
    sb4 errcode = -1;
    VALUE msg;
    VALUE parse_error_offset = Qnil;
    VALUE sql = Qnil;
    int rv;

    switch (status) {
    case OCI_ERROR:
    case OCI_SUCCESS_WITH_INFO:
        /* get error string */
        msg = get_error_msg(errhp, type, "Error", &errcode);
        if (status == OCI_ERROR) {
            exc = eOCIError;
        } else {
            exc = eOCISuccessWithInfo;
        }
        if (stmthp != NULL) {
            ub2 offset;
            text *text;
            ub4 size;

            rv = OCIAttrGet(stmthp, OCI_HTYPE_STMT, &offset, 0, OCI_ATTR_PARSE_ERROR_OFFSET, errhp);
            if (rv == OCI_SUCCESS) {
                parse_error_offset = INT2FIX(offset);
            }
            rv = OCIAttrGet(stmthp, OCI_HTYPE_STMT, &text, &size, OCI_ATTR_STATEMENT, errhp);
            if (rv == OCI_SUCCESS) {
                sql = rb_external_str_new_with_enc(TO_CHARPTR(text), size, oci8_encoding);
            }
        }
        break;
    case OCI_NO_DATA:
        exc = eOCINoData;
        msg = get_error_msg(errhp, type, "No Data", &errcode);
        break;
    case OCI_INVALID_HANDLE:
        exc = eOCIInvalidHandle;
        msg = rb_usascii_str_new_cstr("Invalid Handle");
        break;
    case OCI_NEED_DATA:
        exc = eOCINeedData;
        msg = rb_usascii_str_new_cstr("Need Data");
        break;
    case OCI_STILL_EXECUTING:
        exc = eOCIStillExecuting;
        msg = rb_usascii_str_new_cstr("Still Executing");
        break;
    case OCI_CONTINUE:
        exc = eOCIContinue;
        msg = rb_usascii_str_new_cstr("Continue");
        break;
    default:
        sprintf(errmsg, "Unknown error (%d)", status);
        exc = eOCIException;
        msg = rb_usascii_str_new_cstr(errmsg);
    }
    exc = rb_funcall(exc, oci8_id_new, 4, msg, INT2FIX(errcode), sql, parse_error_offset);
    return set_backtrace(exc, file, line);
}

static VALUE set_backtrace(VALUE exc, const char *file, int line)
{
    char errmsg[64];
    VALUE backtrace;
#ifdef _WIN32
    char *p = strrchr(file, '\\');
    if (p != NULL)
        file = p + 1;
#endif
    backtrace = rb_funcall(rb_cObject, oci8_id_caller, 0);
    if (TYPE(backtrace) == T_ARRAY) {
        snprintf(errmsg, sizeof(errmsg), "%s:%d:in " STRINGIZE(oci8lib) DLEXT, file, line);
        errmsg[sizeof(errmsg) - 1] = '\0';
        rb_ary_unshift(backtrace, rb_usascii_str_new_cstr(errmsg));
        rb_funcall(exc, oci8_id_set_backtrace, 1, backtrace);
    }
    return exc;
}

/*
 * call-seq:
 *    OCI8.new(message, code = nil, sql = nil, parse_error_offset = nil)
 *
 * Creates a new OCIException object.
 */
static VALUE oci8_error_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE msg;
    VALUE code;
    VALUE sql;
    VALUE parse_error_offset;

    rb_scan_args(argc, argv, "04", &msg, &code, &sql, &parse_error_offset);
    rb_call_super(argc > 1 ? 1 : argc, argv);
    rb_ivar_set(self, oci8_id_at_code, code);
    rb_ivar_set(self, oci8_id_at_sql, sql);
    rb_ivar_set(self, oci8_id_at_parse_error_offset, parse_error_offset);
    return Qnil;
}

sb4 oci8_get_error_code(OCIError *errhp)
{
    sb4 errcode = -1;
    OCIErrorGet(oci8_errhp, 1, NULL, &errcode, NULL, 0, OCI_HTYPE_ERROR);
    return errcode;
}

void Init_oci8_error(void)
{
    errbufsiz = ERRBUF_EXPAND_LEN;
    errbuf = xmalloc(errbufsiz);

    oci8_id_at_code = rb_intern("@code");
    oci8_id_at_sql = rb_intern("@sql");
    oci8_id_at_parse_error_offset = rb_intern("@parse_error_offset");
    oci8_id_caller = rb_intern("caller");
    oci8_id_set_backtrace = rb_intern("set_backtrace");

    eOCIException = rb_define_class("OCIException", rb_eStandardError);
    eOCIBreak = rb_define_class("OCIBreak", eOCIException);

    eOCINoData = rb_define_class("OCINoData", eOCIException);
    eOCIError = rb_define_class("OCIError", eOCIException);
    eOCIInvalidHandle = rb_define_class("OCIInvalidHandle", eOCIException);
    eOCINeedData = rb_define_class("OCINeedData", eOCIException);
    eOCIStillExecuting = rb_define_class("OCIStillExecuting", eOCIException);
    eOCIContinue = rb_define_class("OCIContinue", eOCIException);
    eOCISuccessWithInfo = rb_define_class("OCISuccessWithInfo", eOCIError);

    rb_define_method(eOCIException, "initialize", oci8_error_initialize, -1);
    rb_define_attr(eOCIException, "code", 1, 0);
    rb_define_attr(eOCIException, "sql", 1, 0);
    rb_define_attr(eOCIException, "parse_error_offset", 1, 0);
    rb_define_alias(eOCIException, "parseErrorOffset", "parse_error_offset");
}

void oci8_do_raise(OCIError *errhp, sword status, OCIStmt *stmthp, const char *file, int line)
{
    rb_exc_raise(oci8_make_exc(errhp, status, OCI_HTYPE_ERROR, stmthp, file, line));
}

void oci8_do_env_raise(OCIEnv *envhp, sword status, const char *file, int line)
{
    rb_exc_raise(oci8_make_exc(envhp, status, OCI_HTYPE_ENV, NULL, file, line));
}

void oci8_do_raise_init_error(const char *file, int line)
{
    VALUE msg = rb_usascii_str_new_cstr("OCI Library Initialization Error");
    VALUE exc = rb_funcall(eOCIError, oci8_id_new, 2, msg, INT2FIX(-1));

    rb_exc_raise(set_backtrace(exc, file, line));
}

VALUE oci8_get_error_message(ub4 msgno, const char *default_msg)
{
    char head[32];
    size_t headsz;
    const char *errmsg = NULL;
    char msgbuf[64];

    if (have_OCIMessageGet) {
        if (msghp == NULL) {
            oci_lc(OCIMessageOpen(oci8_envhp, oci8_errhp, &msghp, TO_ORATEXT("rdbms"), TO_ORATEXT("ora"), OCI_DURATION_PROCESS));
        }
        errmsg = TO_CHARPTR(OCIMessageGet(msghp, msgno, NULL, 0));
    }
    if (errmsg == NULL) {
        if (default_msg != NULL) {
            errmsg = default_msg;
        } else {
            /* last resort */
            snprintf(msgbuf, sizeof(msgbuf), "Message %u not found;  product=rdbms; facility=ora", msgno);
            errmsg = msgbuf;
        }
    }
    headsz = snprintf(head, sizeof(head), "ORA-%05u: ", msgno);
    return rb_str_append(rb_usascii_str_new(head, headsz),
                         rb_external_str_new_with_enc(errmsg, strlen(errmsg), oci8_encoding));
}

void oci8_do_raise_by_msgno(ub4 msgno, const char *default_msg, const char *file, int line)
{
    VALUE msg = oci8_get_error_message(msgno, default_msg);
    VALUE exc = rb_funcall(eOCIError, oci8_id_new, 2, msg, INT2FIX(-1));

    rb_exc_raise(set_backtrace(exc, file, line));
}

/*
 * Document-class: OCIException
 *
 * The superclass for all exceptions raised by ruby-oci8.
 *
 * The following exceptions are defined as subclasses of OCIError.
 * These exceptions are raised when Oracle Call Interface functions
 * return with an error status.
 *
 * - OCIBreak
 * - OCINoData
 * - OCIError
 * - OCIInvalidHandle
 * - OCINeedData
 * - OCIStillExecuting
 * - OCIContinue
 * - OCISuccessWithInfo
 */

/*
 * Document-class: OCIBreak
 *
 * Subclass of OCIException
 *
 *
 */

/*
 * Document-class: OCINoData
 *
 * Subclass of OCIException
 *
 */

/*
 * Document-class: OCIError
 *
 * Subclass of OCIException
 *
 */

/*
 * Document-class: OCIInvalidHandle
 *
 * Subclass of OCIException
 *
 */

/*
 * Document-class: OCINeedData
 *
 * Subclass of OCIException
 *
 */

/*
 * Document-class: OCIStillExecuting
 *
 * Subclass of OCIException
 *
 */

/*
 * Document-class: OCIContinue
 *
 * Subclass of OCIException
 *
 */

/*
 * Document-class: OCISuccessWithInfo
 *
 * Subclass of OCIException
 *
 */
