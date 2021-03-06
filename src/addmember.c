#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <libgen.h>
#include <syslog.h>

#include "util.h"
#include "config.h"
#include "ldap.h"
#include "krb5.h"
#include "kadm.h"
#include "ceo.pb-c.h"

char *prog = NULL;

static int use_stdin = 0;

static char *name = NULL;
static char *userid = NULL;
static char *program = NULL;
static char password[1024];

static struct option opts[] = {
    { "stdin", 0, NULL, 's' },
    { NULL, 0, NULL, '\0' },
};

const char *default_lib_dir = "/usr/lib/ceod";
const char *lib_dir;

static void usage() {
    fprintf(stderr, "Usage: %s userid realname [program]\n", prog);
    exit(2);
}

int addmember(void) {
    struct strbuf preq = STRBUF_INIT;
    struct strbuf pret = STRBUF_INIT;
    char cpath[1024];
    char *cargv[] = { "ceoc", "adduser", NULL };
    int ret = 0;

    if (snprintf(cpath, sizeof(cpath), "%s/ceoc", lib_dir) >= sizeof(cpath))
        fatal("path too long");

    if (ceo_read_password(password, sizeof(password), use_stdin))
        return 1;

    Ceo__AddUser req;
    ceo__add_user__init(&req);

    req.username = userid;
    req.password = password;
    req.program  = program;
    req.realname = name;
    req.type = CEO__ADD_USER__TYPE__MEMBER;

    strbuf_grow(&preq, ceo__add_user__get_packed_size(&req));
    strbuf_setlen(&preq, ceo__add_user__pack(&req, (uint8_t *)preq.buf));

    if (spawnvem(cpath, cargv, environ, &preq, &pret, 0))
        return 1;

    Ceo__AddUserResponse *resp = ceo__add_user_response__unpack(&protobuf_c_default_allocator,
                                                               pret.len, (uint8_t *)pret.buf);
    if (!resp)
        fatal("failed to unpack response");

    for (int i = 0; i < resp->n_messages; i++) {
        if (resp->messages[i]->status) {
            ret = -1;
            error("%s", resp->messages[i]->message);
        } else {
            notice("%s", resp->messages[i]->message);
        }
    }

    ceo__add_user_response__free_unpacked(resp, &protobuf_c_default_allocator);
    strbuf_release(&preq);
    strbuf_release(&pret);

    return ret;
}

int main(int argc, char *argv[]) {
    int opt;
    int ret;

    prog = xstrdup(basename(argv[0]));
    init_log(prog, LOG_PID, LOG_AUTHPRIV, 1);

    configure();

    while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (opt) {
            case 's':
                use_stdin = 1;
                break;
            case '?':
                usage();
                break;
            default:
                fatal("error parsing arguments");
        }
    }

    if (argc - optind != 2 && argc - optind != 3)
        usage();

    userid = argv[optind++];
    name = argv[optind++];

    if (argc - optind)
        program = argv[optind++];

    lib_dir = getenv("CEO_LIB_DIR") ?: default_lib_dir;

    ret = addmember();

    free_config();
    free(prog);

    return ret;
}
