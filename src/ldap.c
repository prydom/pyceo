#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <sasl/sasl.h>
#include <krb5.h>

#define LDAP_DEPRECATED 1
#include <ldap.h>

#include "ldap.h"
#include "krb5.h"
#include "config.h"
#include "util.h"

extern char *prog;

LDAP *ld;

static void ldap_fatal(char *msg) {
    int errnum = 0;
    char *errstr = NULL;
    char *detail = NULL;

    if (ldap_get_option(ld, LDAP_OPT_ERROR_NUMBER, &errnum) != LDAP_SUCCESS)
        warn("ldap_get_option(LDAP_OPT_ERROR_NUMBER) failed");
    if (ldap_get_option(ld, LDAP_OPT_ERROR_STRING, &detail) != LDAP_SUCCESS)
        warn("ldap_get_option(LDAP_OPT_ERROR_STRING) failed");

    errstr = ldap_err2string(errnum);

    if (detail)
        fatal("%s: %s (%d): %s", msg, errstr, errnum, detail);
    else if (errnum)
        fatal("%s: %s (%d)", msg, errstr, errnum);
    else
        fatal("%s", msg);
}

static void ldap_err(char *msg) {
    int errnum = 0;
    char *errstr = NULL;
    char *detail = NULL;

    if (ldap_get_option(ld, LDAP_OPT_ERROR_NUMBER, &errnum) != LDAP_SUCCESS)
        warn("ldap_get_option(LDAP_OPT_ERROR_NUMBER) failed");
    if (ldap_get_option(ld, LDAP_OPT_ERROR_STRING, &detail) != LDAP_SUCCESS)
        warn("ldap_get_option(LDAP_OPT_ERROR_STRING) failed");

    errstr = ldap_err2string(errnum);

    if (detail)
        error("%s: %s (%d): %s", msg, errstr, errnum, detail);
    else if (errnum)
        error("%s: %s (%d)", msg, errstr, errnum);
    else
        error("%s", msg);
}

int ceo_add_group(char *cn, char *basedn, int no) {
    if (!cn || !basedn)
        fatal("addgroup: Invalid argument");

    LDAPMod *mods[8];
    int i = -1;
    int ret = 0;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "objectClass";
    char *objectClasses[] = { "top", "group", "posixGroup", NULL };
    mods[i]->mod_values = objectClasses;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "cn";
    char *uids[] = { cn, NULL };
    mods[i]->mod_values = uids;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "gidNumber";
    char idno[16];
    snprintf(idno, sizeof(idno), "%d", no);
    char *gidNumbers[] = { idno, NULL };
    mods[i]->mod_values = gidNumbers;

    mods[++i] = NULL;

    char dn[1024];
    snprintf(dn, sizeof(dn), "cn=%s,%s", cn, basedn);

    if (ldap_add_s(ld, dn, mods) != LDAP_SUCCESS) {
        ldap_err("addgroup");
        ret = -1;
    }

    for (i = 0; mods[i]; i++)
        free(mods[i]);

    return ret;
}

int ceo_add_group_sudo(char *group, char *basedn) {
    if (!group || !basedn)
        fatal("addgroup: Invalid argument");

    LDAPMod *mods[8];
    int i = -1;
    int ret = 0;

    char cn[17];
    snprintf(cn, sizeof(cn), "%%%s", group);

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "objectClass";
    char *objectClasses[] = { "top", "sudoRole", NULL };
    mods[i]->mod_values = objectClasses;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "cn";
    char *uids[] = { cn, NULL };
    mods[i]->mod_values = uids;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "sudoUser";
    char *sudouser[] = { cn, NULL };
    mods[i]->mod_values = sudouser;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "sudoHost";
    char *sudohost[] = { "ALL", NULL };
    mods[i]->mod_values = sudohost;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "sudoCommand";
    char *sudocommand[] = { "ALL", NULL };
    mods[i]->mod_values = sudocommand;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "sudoOption";
    char *sudooption[] = { "!authenticate", NULL };
    mods[i]->mod_values = sudooption;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "sudoRunAsUser";
    char *sudorunas[] = { group, NULL };
    mods[i]->mod_values = sudorunas;

    char dn[1024];
    snprintf(dn, sizeof(dn), "cn=%%%s,%s", group, basedn);

    mods[++i] = NULL;

    if (ldap_add_s(ld, dn, mods) != LDAP_SUCCESS) {
        ldap_err("addgroup");
        ret = -1;
    }

    for (i = 0; mods[i]; i++)
        free(mods[i]);

    return ret;
}

int ceo_add_user(char *uid, char *basedn, char *objclass, char *cn, char *home, char *shell, int no, ...) {
    va_list args;

    if (!uid || !basedn || !cn || !home || !shell)
        fatal("adduser: Invalid argument");

    LDAPMod *mods[16];
    char *vals[16][2];
    int i = -1;
    int ret = 0;
    int classes = 4;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "objectClass";
    char *objectClasses[] = { "top", "account", "posixAccount", "shadowAccount", NULL, NULL, NULL, NULL };
    if (objclass != NULL)
        objectClasses[classes++] = objclass;
    mods[i]->mod_values = objectClasses;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "uid";
    char *uids[] = { uid, NULL };
    mods[i]->mod_values = uids;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "cn";
    char *cns[] = { cn, NULL };
    mods[i]->mod_values = cns;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "loginShell";
    char *shells[] = { shell, NULL };
    mods[i]->mod_values = shells;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "uidNumber";
    char idno[16];
    snprintf(idno, sizeof(idno), "%d", no);
    char *uidNumbers[] = { idno, NULL };
    mods[i]->mod_values = uidNumbers;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "gidNumber";
    mods[i]->mod_values = uidNumbers;

    mods[++i] = xmalloc(sizeof(LDAPMod));
    mods[i]->mod_op = LDAP_MOD_ADD;
    mods[i]->mod_type = "homeDirectory";
    char *homeDirectory[] = { home, NULL };
    mods[i]->mod_values = homeDirectory;

    va_start(args, no);
    char *attr;
    while ((attr = va_arg(args, char *))) {
        char *val = va_arg(args, char *);

        if (!val || !*val)
            continue;

        if (i == sizeof(mods) / sizeof(*mods) - 2) {
            error("too many attributes");
            return -1;
        }

        mods[++i] = xmalloc(sizeof(LDAPMod));
        mods[i]->mod_op = LDAP_MOD_ADD;
        mods[i]->mod_type = attr;
        vals[i][0] = val;
        vals[i][1] = NULL;
        mods[i]->mod_values = vals[i];
    }

    mods[++i] = NULL;

    char dn[1024];
    snprintf(dn, sizeof(dn), "uid=%s,%s", uid, basedn);

    if (ldap_add_s(ld, dn, mods) != LDAP_SUCCESS) {
        ldap_err("adduser");
        ret = -1;
    }

    for (i = 0; mods[i]; i++)
        free(mods[i]);

    return ret;
}

int ceo_new_uid(int min, int max) {
    char filter[64];
    char *attrs[] = { LDAP_NO_ATTRS, NULL };
    LDAPMessage *res;
    int i;

    for (i = min; i <= max; i++) {
        // id taken due to passwd
        if (getpwuid(i) != NULL)
            continue;

        // id taken due to group
        if (getgrgid(i) != NULL)
            continue;

        snprintf(filter, sizeof(filter), "(|(uidNumber=%d)(gidNumber=%d))", i, i);
        if (ldap_search_s(ld, ldap_users_base, LDAP_SCOPE_SUBTREE, filter, attrs, 1, &res) != LDAP_SUCCESS) {
            ldap_err("firstuid");
            return -1;
        }

        int count = ldap_count_entries(ld, res);
        ldap_msgfree(res);

        // id taken due to LDAP
        if (count)
            continue;

        return i;
    }

    return -1;
}

int ceo_user_exists(char *uid) {
    char *attrs[] = { LDAP_NO_ATTRS, NULL };
    LDAPMessage *msg = NULL;
    char filter[128];
    int count;

    if (!uid)
        fatal("null uid");

    snprintf(filter, sizeof(filter), "uid=%s", uid);

    if (ldap_search_s(ld, ldap_users_base, LDAP_SCOPE_SUBTREE, filter, attrs, 0, &msg) != LDAP_SUCCESS) {
        ldap_err("user_exists");
        return -1;
    }

    count = ldap_count_entries(ld, msg);
    ldap_msgfree(msg);

    return count > 0;
}

int ceo_group_exists(char *cn) {
    char *attrs[] = { LDAP_NO_ATTRS, NULL };
    LDAPMessage *msg = NULL;
    char filter[128];
    int count;

    if (!cn)
        fatal("null cd");

    snprintf(filter, sizeof(filter), "cn=%s", cn);

    if (ldap_search_s(ld, ldap_groups_base, LDAP_SCOPE_SUBTREE, filter, attrs, 0, &msg) != LDAP_SUCCESS) {
        ldap_err("group_exists");
        return -1;
    }

    count = ldap_count_entries(ld, msg);
    ldap_msgfree(msg);

    return count > 0;
}

static int ldap_sasl_interact(LDAP *ld, unsigned flags, void *defaults, void *in) {
    sasl_interact_t *interact = in;

    while (interact->id != SASL_CB_LIST_END) {
        switch (interact->id) {

            // GSSAPI doesn't require any callbacks

            default:
                interact->result = "";
                interact->len = 0;
        }

        interact++;
    }

    return LDAP_SUCCESS;
}

void ceo_ldap_init() {
    int proto = LDAP_DEFAULT_PROTOCOL;

    if (!ldap_admin_principal)
        fatal("not configured");

    if (ldap_initialize(&ld, ldap_server_url) != LDAP_SUCCESS)
        ldap_fatal("ldap_initialize");

    if (ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &proto) != LDAP_OPT_SUCCESS)
        ldap_fatal("ldap_set_option");

    if (ldap_sasl_interactive_bind_s(ld, NULL, ldap_sasl_mech, NULL, NULL,
                LDAP_SASL_QUIET, &ldap_sasl_interact, NULL) != LDAP_SUCCESS)
        ldap_fatal("Bind failed");
}

void ceo_ldap_cleanup() {
    ldap_unbind(ld);
}
