/*
This file is part of GUFI, which is part of MarFS, which is released
under the BSD license.


Copyright (c) 2017, Los Alamos National Security (LANS), LLC
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


From Los Alamos National Security, LLC:
LA-CC-15-039

Copyright (c) 2017, Los Alamos National Security, LLC All rights reserved.
Copyright 2017. Los Alamos National Security, LLC. This software was produced
under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National
Laboratory (LANL), which is operated by Los Alamos National Security, LLC for
the U.S. Department of Energy. The U.S. Government has rights to use,
reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR LOS
ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is
modified to produce derivative works, such modified software should be
clearly marked, so as not to confuse it with the version available from
LANL.

THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL SECURITY, LLC OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
*/



#include <stdlib.h>
#include <string.h>

#include "OutputBuffers.h"
#include "dbutils.h"
#include "histogram.h"
#include "print.h"

static void sub_help(void) {
    printf("db                       db file path\n");
    printf("SQL                      SQL statements to run\n");
    printf("\n");
    printf("If no SQL statements are passed in, will read from stdin\n");
    printf("\n");
}

int main(int argc, char *argv[]) {
    struct input in;
    int idx = parse_cmd_line(argc, argv, "hd:", 0, "[db [SQL]...]", &in);
    if (in.helped)
        sub_help();
    if (idx < 0) {
        input_fini(&in);
        return EXIT_FAILURE;
    }

    const int args_left = argc - idx;

    sqlite3 *db = NULL;;

    /* create in-memory db and read from stdin */
    if (args_left == 0) {
        db = opendb(":memory:", SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, 0, 1, NULL, NULL);
    }
    /* open db file */
    else if (args_left > 1) {
        const char *dbname = argv[idx++];
        db = opendb(dbname,     SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, 0, 1, NULL, NULL);
    }
    else {
        print_help(argv[0], "hd:", "[db [SQL]...]");
        sub_help();
        return EXIT_FAILURE;
    }

    if (!db) {
        return EXIT_FAILURE;
    }

    addqueryfuncs(db);
    addhistfuncs(db);

    /* no buffering */
    struct OutputBuffer ob;
    memset(&ob, 0, sizeof(ob));

    PrintArgs_t pa = {
        .output_buffer = &ob,
        .delim = in.delim,
        .mutex = NULL,
        .outfile = stdout,
        .rows = 0,
    };

    char *err = NULL;

    /* if using in-memory db or no SQL statements following db path, read from stdin */
    if (args_left < 2) {
        char *line = NULL;
        size_t len = 0;
        while (getline(&line, &len, stdin) != -1) {
            if (sqlite3_exec(db, line, print_parallel, &pa, &err) != SQLITE_OK) {
                sqlite_print_err_and_free(err, stderr, "Error: SQL error: %s\n", err);
                break;
            }
        }
        free(line);
    }
    else {
        for(int i = idx; i < argc; i++) {
            if (sqlite3_exec(db, argv[i], print_parallel, &pa, &err) != SQLITE_OK) {
                sqlite_print_err_and_free(err, stderr, "Error: SQL error: %s\n", err);
                break;
            }
        }
    }

    closedb(db);

    return err?EXIT_FAILURE:EXIT_SUCCESS;
}
