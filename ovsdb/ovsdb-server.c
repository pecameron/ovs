/* Copyright (c) 2009 Nicira Networks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "ovsdb.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include "command-line.h"
#include "daemon.h"
#include "fault.h"
#include "file.h"
#include "json.h"
#include "jsonrpc.h"
#include "jsonrpc-server.h"
#include "leak-checker.h"
#include "list.h"
#include "ovsdb-error.h"
#include "poll-loop.h"
#include "process.h"
#include "stream.h"
#include "svec.h"
#include "timeval.h"
#include "trigger.h"
#include "util.h"
#include "unixctl.h"

#include "vlog.h"
#define THIS_MODULE VLM_ovsdb_server

static const struct jsonrpc_server_cbs ovsdb_jsonrpc_cbs;

static void parse_options(int argc, char *argv[], char **file_namep,
                          struct svec *active, struct svec *passive);
static void usage(void) NO_RETURN;

int
main(int argc, char *argv[])
{
    struct unixctl_server *unixctl;
    struct ovsdb_jsonrpc_server *jsonrpc;
    struct svec active, passive;
    struct ovsdb_error *error;
    struct ovsdb *db;
    const char *name;
    char *file_name;
    bool do_chdir;
    int retval;
    size_t i;

    set_program_name(argv[0]);
    register_fault_handlers();
    time_init();
    vlog_init();
    signal(SIGPIPE, SIG_IGN);
    process_init();

    parse_options(argc, argv, &file_name, &active, &passive);

    if (get_detach() && is_chdir_enabled()) {
        /* We need to skip chdir("/") in daemonize() and do it later, because
         * we need to open the database and possible set up up Unix domain
         * sockets in the current working directory after we daemonize.  We
         * can't open the database before we daemonize because file locks
         * aren't inherited by child processes.  */
        do_chdir = true;
        set_no_chdir();
    } else {
        do_chdir = false;
    }
    die_if_already_running();
    daemonize();

    error = ovsdb_file_open(file_name, false, &db);
    if (error) {
        ovs_fatal(0, "%s", ovsdb_error_to_string(error));
    }

    jsonrpc = ovsdb_jsonrpc_server_create(db);
    SVEC_FOR_EACH (i, name, &active) {
        ovsdb_jsonrpc_server_connect(jsonrpc, name);
    }
    SVEC_FOR_EACH (i, name, &passive) {
        retval = ovsdb_jsonrpc_server_listen(jsonrpc, name);
        if (retval) {
            ovs_fatal(retval, "failed to listen on %s", name);
        }
    }
    svec_destroy(&active);
    svec_destroy(&passive);

    retval = unixctl_server_create(NULL, &unixctl);
    if (retval) {
        ovs_fatal(retval, "could not listen for control connections");
    }

    if (do_chdir) {
        chdir("/");
    }
    for (;;) {
        ovsdb_jsonrpc_server_run(jsonrpc);
        unixctl_server_run(unixctl);
        ovsdb_trigger_run(db, time_msec());

        ovsdb_jsonrpc_server_wait(jsonrpc);
        unixctl_server_wait(unixctl);
        ovsdb_trigger_wait(db, time_msec());
        poll_block();
    }

    return 0;
}

static void
parse_options(int argc, char *argv[], char **file_namep,
              struct svec *active, struct svec *passive)
{
    enum {
        OPT_DUMMY = UCHAR_MAX + 1,
        OPT_CONNECT,
        OPT_LISTEN,
        VLOG_OPTION_ENUMS,
        LEAK_CHECKER_OPTION_ENUMS
    };
    static struct option long_options[] = {
        {"connect",     required_argument, 0, OPT_CONNECT},
        {"listen",      required_argument, 0, OPT_LISTEN},
        {"help",        no_argument, 0, 'h'},
        {"version",     no_argument, 0, 'V'},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        LEAK_CHECKER_LONG_OPTIONS,
        {0, 0, 0, 0},
    };
    char *short_options = long_options_to_short_options(long_options);

    svec_init(active);
    svec_init(passive);
    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case OPT_CONNECT:
            svec_add(active, optarg);
            break;

        case OPT_LISTEN:
            svec_add(passive, optarg);
            break;

        case 'h':
            usage();

        case 'V':
            OVS_PRINT_VERSION(0, 0);
            exit(EXIT_SUCCESS);

        VLOG_OPTION_HANDLERS
        DAEMON_OPTION_HANDLERS
        LEAK_CHECKER_OPTION_HANDLERS

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    if (argc != 1) {
        ovs_fatal(0, "database file is only non-option argument; "
                "use --help for usage");
    }

    *file_namep = argv[0];
}

static void
usage(void)
{
    printf("%s: Open vSwitch database server\n"
           "usage: %s [OPTIONS] DATABASE\n"
           "where DATABASE is a database file in ovsdb format.\n",
           program_name, program_name);
    printf("\nJSON-RPC options (may be specified any number of times):\n"
           "  --connect=REMOTE        make active connection to REMOTE\n"
           "  --listen=LOCAL          passively listen on LOCAL\n");
    stream_usage("JSON-RPC", true, true);
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  -h, --help              display this help message\n"
           "  -V, --version           display version information\n");
    leak_checker_usage();
    exit(EXIT_SUCCESS);
}
