#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <grp.h>

#include "util.h"
#include "gss.h"
#include "net.h"
#include "strbuf.h"

static gss_cred_id_t my_creds = GSS_C_NO_CREDENTIAL;
static gss_ctx_id_t context_handle = GSS_C_NO_CONTEXT;
static gss_name_t peer_name = GSS_C_NO_NAME;
static gss_name_t imported_service = GSS_C_NO_NAME;
static char *peer_principal;
static char *peer_username;
static OM_uint32 ret_flags;
static int complete;
char service_name[128];

void free_gss(void) {
    OM_uint32 maj_stat, min_stat;

    if (peer_name) {
        maj_stat = gss_release_name(&min_stat, &peer_name);
        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_release_name", maj_stat, min_stat);
    }

    if (imported_service) {
        maj_stat = gss_release_name(&min_stat, &imported_service);
        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_release_name", maj_stat, min_stat);
    }

    if (context_handle) {
        maj_stat = gss_delete_sec_context(&min_stat, &context_handle, GSS_C_NO_BUFFER);
        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_delete_sec_context", maj_stat, min_stat);
    }

    if (my_creds) {
        maj_stat = gss_release_cred(&min_stat, &my_creds);
        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_release_creds", maj_stat, min_stat);
    }

    free(peer_principal);
    free(peer_username);
}

static char *gssbuf2str(gss_buffer_t buf) {
    char *msgstr = xmalloc(buf->length + 1);
    memcpy(msgstr, buf->value, buf->length);
    msgstr[buf->length] = '\0';
    return msgstr;
}

static void display_status(char *prefix, OM_uint32 code, int type) {
    OM_uint32 maj_stat, min_stat;
    gss_buffer_desc msg;
    OM_uint32 msg_ctx = 0;
    char *msgstr;

    maj_stat = gss_display_status(&min_stat, code, type, GSS_C_NULL_OID,
                                  &msg_ctx, &msg);
    (void)maj_stat;
    msgstr = gssbuf2str(&msg);
    logmsg(LOG_ERR, "%s: %s", prefix, msgstr);
    gss_release_buffer(&min_stat, &msg);
    free(msgstr);

    while (msg_ctx) {
        maj_stat = gss_display_status(&min_stat, code, type, GSS_C_NULL_OID,
                                      &msg_ctx, &msg);
        msgstr = gssbuf2str(&msg);
        logmsg(LOG_ERR, "additional: %s", msgstr);
        gss_release_buffer(&min_stat, &msg);
        free(msgstr);
    }
}

void gss_fatal(char *msg, OM_uint32 maj_stat, OM_uint32 min_stat) {
    logmsg(LOG_ERR, "fatal: %s", msg);
    display_status("major", maj_stat, GSS_C_GSS_CODE);
    display_status("minor", min_stat, GSS_C_MECH_CODE);
    exit(1);
}

static void import_service(const char *service, const char *hostname) {
    OM_uint32 maj_stat, min_stat;
    gss_buffer_desc buf_desc;

    if (snprintf(service_name, sizeof(service_name),
                 "%s@%s", service, hostname) >= sizeof(service_name))
        fatal("service name too long");

    buf_desc.value = service_name;
    buf_desc.length = strlen(service_name);

    maj_stat = gss_import_name(&min_stat, &buf_desc,
                               GSS_C_NT_HOSTBASED_SERVICE, &imported_service);
    if (maj_stat != GSS_S_COMPLETE)
        gss_fatal("gss_import_name", maj_stat, min_stat);
}

static void check_services(OM_uint32 flags) {
    debug("gss services: %sconf %sinteg %smutual %sreplay %ssequence",
            flags & GSS_C_CONF_FLAG     ? "+" : "-",
            flags & GSS_C_INTEG_FLAG    ? "+" : "-",
            flags & GSS_C_MUTUAL_FLAG   ? "+" : "-",
            flags & GSS_C_REPLAY_FLAG   ? "+" : "-",
            flags & GSS_C_SEQUENCE_FLAG ? "+" : "-");
    if (~flags & GSS_C_CONF_FLAG)
        fatal("confidentiality service required");
    if (~flags & GSS_C_INTEG_FLAG)
        fatal("integrity service required");
    if (~flags & GSS_C_MUTUAL_FLAG)
        fatal("mutual authentication required");
}

void server_acquire_creds(const char *service) {
    OM_uint32 maj_stat, min_stat;
    OM_uint32 time_rec;

    if (!strlen(fqdn.buf))
        fatal("empty fqdn");

    import_service(service, fqdn.buf);

    notice("acquiring credentials for %s", service_name);

    maj_stat = gss_acquire_cred(&min_stat, imported_service, GSS_C_INDEFINITE,
                                GSS_C_NULL_OID_SET, GSS_C_ACCEPT, &my_creds,
                                NULL, &time_rec);
    if (maj_stat != GSS_S_COMPLETE)
        gss_fatal("gss_acquire_cred", maj_stat, min_stat);

    /* Work around bug in libgssapi 2.0.25 / gssapi_krb5 2.2:
     *  The expiry time returned by gss_acquire_cred is always zero. */
    {
        int names_match = 0;
        gss_name_t cred_service;
        gss_cred_usage_t cred_usage;
        maj_stat = gss_inquire_cred(&min_stat, my_creds, &cred_service, &time_rec, &cred_usage, NULL);
        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_inquire_cred", maj_stat, min_stat);

        if (time_rec != GSS_C_INDEFINITE)
            fatal("credentials valid for %d seconds (oops)", time_rec);

        maj_stat = gss_compare_name(&min_stat, imported_service, cred_service, &names_match);

        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_compare_name", maj_stat, min_stat);

        if (!names_match)
            fatal("credentials granted for wrong service (oops)");

        if (!(cred_usage & GSS_C_ACCEPT))
            fatal("credentials lack usage GSS_C_ACCEPT (oops)");
    }
}

void client_acquire_creds(const char *service, const char *hostname) {
    import_service(service, hostname);
}

static char *princ_to_username(char *princ) {
    char *ret = xstrdup(princ);
    char *c = strchr(ret, '@');
    if (c)
        *c = '\0';
    return ret;
}

int process_server_token(gss_buffer_t incoming_tok, gss_buffer_t outgoing_tok) {
    OM_uint32 maj_stat, min_stat;
    OM_uint32 time_rec;
    gss_OID name_type;
    gss_buffer_desc peer_princ;

    if (complete)
        fatal("unexpected %zd-byte token from peer", incoming_tok->length);

    maj_stat = gss_accept_sec_context(&min_stat, &context_handle, my_creds,
            incoming_tok, GSS_C_NO_CHANNEL_BINDINGS, &peer_name, NULL,
            outgoing_tok, &ret_flags, &time_rec, NULL);
    if (maj_stat == GSS_S_COMPLETE) {
        check_services(ret_flags);

        complete = 1;

        maj_stat = gss_display_name(&min_stat, peer_name, &peer_princ, &name_type);
        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_display_name", maj_stat, min_stat);

        peer_principal = xstrdup((char *)peer_princ.value);
        peer_username = princ_to_username((char *)peer_princ.value);

        notice("client authenticated as %s", peer_principal);
        debug("context expires in %d seconds", time_rec);

        maj_stat = gss_release_buffer(&min_stat, &peer_princ);
        if (maj_stat != GSS_S_COMPLETE)
            gss_fatal("gss_release_buffer", maj_stat, min_stat);

    } else if (maj_stat != GSS_S_CONTINUE_NEEDED) {
        gss_fatal("gss_accept_sec_context", maj_stat, min_stat);
    }

    return complete;
}

int process_client_token(gss_buffer_t incoming_tok, gss_buffer_t outgoing_tok) {
    OM_uint32 maj_stat, min_stat;
    OM_uint32 time_rec;
    gss_OID_desc krb5 = *gss_mech_krb5;

    if (complete)
        fatal("unexpected token from peer");

    maj_stat = gss_init_sec_context(&min_stat, GSS_C_NO_CREDENTIAL, &context_handle,
                                    imported_service, &krb5, GSS_C_MUTUAL_FLAG |
                                    GSS_C_REPLAY_FLAG | GSS_C_SEQUENCE_FLAG,
                                    GSS_C_INDEFINITE, GSS_C_NO_CHANNEL_BINDINGS,
                                    incoming_tok, NULL, outgoing_tok, &ret_flags,
                                    &time_rec);
    if (maj_stat == GSS_S_COMPLETE) {
        notice("server authenticated as %s", service_name);
        notice("context expires in %d seconds", time_rec);

        check_services(ret_flags);

        complete = 1;

    } else if (maj_stat != GSS_S_CONTINUE_NEEDED) {
        gss_fatal("gss_init_sec_context", maj_stat, min_stat);
    }

    return complete;
}

int initial_client_token(gss_buffer_t outgoing_tok) {
    return process_client_token(GSS_C_NO_BUFFER, outgoing_tok);
}

char *client_principal(void) {
    if (!complete)
        fatal("authentication checked before finishing");
    return peer_principal;
}

char *client_username(void) {
    if (!complete)
        fatal("authentication checked before finishing");
    return peer_username;
}

void gss_encipher(struct strbuf *plain, struct strbuf *cipher) {
    OM_uint32 maj_stat, min_stat;
    gss_buffer_desc plain_tok, cipher_tok;
    int conf_state;

    plain_tok.value = plain->buf;
    plain_tok.length = plain->len;

    maj_stat = gss_wrap(&min_stat, context_handle, 1, GSS_C_QOP_DEFAULT,
                        &plain_tok, &conf_state, &cipher_tok);
    if (maj_stat != GSS_S_COMPLETE)
        gss_fatal("gss_wrap", maj_stat, min_stat);

    if (!conf_state)
        fatal("gss_encipher: confidentiality service required");

    strbuf_add(cipher, cipher_tok.value, cipher_tok.length);

    maj_stat = gss_release_buffer(&min_stat, &cipher_tok);
    if (maj_stat != GSS_S_COMPLETE)
        gss_fatal("gss_release_buffer", maj_stat, min_stat);
}

void gss_decipher(struct strbuf *cipher, struct strbuf *plain) {
    OM_uint32 maj_stat, min_stat;
    gss_buffer_desc plain_tok, cipher_tok;
    int conf_state;
    gss_qop_t qop_state;

    cipher_tok.value = cipher->buf;
    cipher_tok.length = cipher->len;

    maj_stat = gss_unwrap(&min_stat, context_handle, &cipher_tok,
                          &plain_tok, &conf_state, &qop_state);
    if (maj_stat != GSS_S_COMPLETE)
        gss_fatal("gss_unwrap", maj_stat, min_stat);

    if (!conf_state)
        fatal("gss_encipher: confidentiality service required");

    strbuf_add(plain, plain_tok.value, plain_tok.length);

    maj_stat = gss_release_buffer(&min_stat, &plain_tok);
    if (maj_stat != GSS_S_COMPLETE)
        gss_fatal("gss_release_buffer", maj_stat, min_stat);
}


