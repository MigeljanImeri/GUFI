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



#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <string.h>

#include "QueuePerThreadPool.h"
#include "bf.h"
#include "debug.h"
#include "dbutils.h"
#include "external.h"
#include "template_db.h"
#include "utils.h"
#include "BottomUp.h"

enum DirOp {
    MKDIR,
    RMDIR,
    RENAME,
    UPDATE,
};

/* global to pool - passed around in "args" argument */
struct PoolArgs {
    struct input in;
    struct template_db db;
    size_t indexbasename_len;
};

struct NonDirArgs {
    struct input *in;

    /* thread args */
    struct template_db *temp_db;
    struct work *work;
    struct entry_data ed;

    /* index path */
    char topath[MAXPATH];
    size_t topath_len;

    /* summary of the current directory */
    struct sum summary;

    /* db.db */
    sqlite3 *db;

    /* prepared statements */
    sqlite3_stmt *entries_res;
};

/* Data stored during first pass of input file */
struct row {
    char *line;
    size_t len;
    enum DirOp dir_op;  /* MKDIR or RMDIR or RENAME */
    long offset;
    size_t entries;
};

static struct row *row_init(char *line, const size_t len,
                            const int dir_op, const long offset) {
    struct row *row = malloc(sizeof(struct row));
    if (row) {
        row->line = line; /* takes ownership of line */
        row->len = len;
        row->dir_op = dir_op;
        row->offset = offset;
        row->entries = 0;
    }
    return row;
}

static void row_destroy(struct row *row) {
    if (row) {
        free(row->line);
        free(row);
    }
}

static int recursive_delete(struct input *in, char *path) {
    return parallel_bottomup(&path, 1, in->maxthreads,
                             sizeof(struct BottomUp),
                             NULL, rm_dir,
                             1,
                             0,
                             NULL);
}

static int process_nondir(struct work *entry, struct entry_data *ed, void *args) {
    struct NonDirArgs *nda = (struct NonDirArgs *) args;

    if (lstat(entry->name, &ed->statuso) != 0) {
        return 1;
    }

    if (ed->type == 'l') {
        readlink(entry->name, ed->linkname, MAXPATH);
        /* error? */
    }

    /* get entry relative path (use extra buffer to prevent memcpy overlap) */
    char relpath[MAXPATH];
    const size_t relpath_len = SNFORMAT_S(relpath, MAXPATH, 1,
                                          entry->name + entry->root_parent.len, entry->name_len - entry->root_parent.len);
    /* overwrite full path with relative path */
    entry->name_len = SNFORMAT_S(entry->name, MAXPATH, 1, relpath, relpath_len);

    /* update summary table */
    sumit(&nda->summary, ed);

    /* add entry names into bulk insert */
    insertdbgo(entry, ed, nda->entries_res);

    return 0;
}

/* process the work under one directory (no recursion)
 * assumes directory is already created in index.
 * */
static int processdir(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    /* Not checking arguments */

    (void) ctx; (void) id;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    struct input *in = &pa->in;
    struct row *w = (struct row *) data;
    struct NonDirArgs nda;
    memset(&nda, 0, sizeof(nda));
    nda.in = &pa->in;
    nda.temp_db = &pa->db;

    const size_t topath_parent_len = dirname_len(w->line, w->len);

    /* path of directory in index */
    char indexpath[MAXPATH];
    const size_t indexpath_len = SNFORMAT_S(indexpath, MAXPATH, 4,
                                            in->nameto.data, in->nameto.len,
                                            "/", (size_t) 1,
                                            in->name.data + pa->indexbasename_len, in->name.len - pa->indexbasename_len,
                                            w->line + in->name.len, w->len - in->name.len);

    if (lstat(w->line, &nda.ed.statuso) != 0) {
        fprintf(stderr, "Could not stat directory \"%s\"\n", w->line);
        goto cleanup;
    }

    w->line[topath_parent_len - 1] = '\0';
    struct stat topath_parent_stat;
    if (lstat(w->line, &topath_parent_stat) != 0) {
        fprintf(stderr, "Could not stat directory \"%s\"\n", w->line);
        goto cleanup;
    }
    w->line[topath_parent_len - 1] = '/';

    DIR *dir = opendir(w->line);
    if (!dir) {
        fprintf(stderr, "In processdir\nCould not open directory \"%s\"\n", w->line);
        goto cleanup;
    }

    /* check if directory exists in index, not necessarily an error if it doesn't.
     * likely caused by moves happening, will be fixed later on.
     * */
    DIR *indexdir = opendir(indexpath);
    if (indexdir == NULL) {
        const int err = errno;
        if (err == ENOENT) {
            nda.db = NULL;
            goto cleanup;
        }
    }
    closedir(indexdir);

    /* create the database name */
    char dbname[MAXPATH];
    SNFORMAT_S(dbname, MAXPATH, 3,
               indexpath, indexpath_len,
               "/", (size_t) 1,
               DBNAME, DBNAME_LEN);

    nda.db = template_to_db(&pa->db, dbname, nda.ed.statuso.st_uid, nda.ed.statuso.st_gid);

    if (!nda.db) {
        goto cleanup;
    }

    zeroit(&nda.summary);

    nda.entries_res = insertdbprep(nda.db, ENTRIES_INSERT);

    refstr_t parent = {
	    .data = w->line,
	    .len = w->len
    };

    struct work nda_parent;
    memset(&nda_parent, 0, sizeof(nda_parent));
    nda.work = &nda_parent;
    nda.work->pinode = topath_parent_stat.st_ino;
    nda.ed.type = 'd';

    /* Read through directory on indexed file system and add files to db */
    startdb(nda.db);
    if (nda.db) {
        nda.work->name_len = SNFORMAT_S(nda.work->name, sizeof(nda.work->name), 1, w->line, w->len);
        descend(ctx, id, pa, in, nda.work, nda.ed.statuso.st_ino, dir, 0,
                NULL, process_nondir, &nda, NULL);
    }
    stopdb(nda.db);

    insertdbfin(nda.entries_res);

    /* insert this directory's summary data */
    size_t index = trailing_match_index(parent.data, parent.len, "/", 1);
    insertsumdb(nda.db, parent.data + index,
                nda.work, &nda.ed, &nda.summary);

    closedb(nda.db);

    /* ignore errors */
    chmod(indexpath, nda.ed.statuso.st_mode);
    chown(indexpath, nda.ed.statuso.st_uid, nda.ed.statuso.st_gid);

cleanup:
    closedir(dir);
    row_destroy(w);

    return !nda.db;
}

static int processdir_recursive(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    int rc = 0;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    struct input *in = &pa->in;

    struct NonDirArgs nda;
    nda.in         = &pa->in;
    nda.temp_db    = &pa->db;
    nda.work       = (struct work *) data;
    memset(&nda.ed, 0, sizeof(nda.ed));
    nda.ed.type    = 'd';

    DIR *dir = NULL;

    if (lstat(nda.work->name, &nda.ed.statuso) != 0) {
        fprintf(stderr, "Could not stat directory \"%s\"\n", nda.work->name);
        rc = 1;
        goto cleanup;
    }

    dir = opendir(nda.work->name);
    if (!dir) {
        fprintf(stderr, "In processdir_recursive:\nCould not open directory \"%s\"\n", nda.work->name);
        rc = 1;
        goto cleanup;
    }

    nda.topath_len = SNFORMAT_S(nda.topath, MAXPATH, 4,
                                in->nameto.data, in->nameto.len,
                                "/", (size_t) 1,
                                in->name.data + pa->indexbasename_len, in->name.len - pa->indexbasename_len,
                                nda.work->name + in->name.len, strlen(nda.work->name + in->name.len));

    
    /* Delete everything under this parent directory and reindex */
    if (nda.work->level == 0) {
        recursive_delete(in, nda.topath);
    }

    if (mkdir(nda.topath, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0) {
        const int err = errno;
        if (err != EEXIST) {
            fprintf(stderr, "mkdir %s failure: %d %s\n", nda.topath, err, strerror(err));
            /* If mkdir fails, likely due to move that happened above it that has deleted everything
             * below it, will eventually reindex this directory through move that occured above this one */
            rc = 1;
            goto cleanup;
        }
    }

    /* create the database name */
    char dbname[MAXPATH];
    SNFORMAT_S(dbname, MAXPATH, 3,
               nda.topath, nda.topath_len,
               "/", (size_t) 1,
               DBNAME, DBNAME_LEN);

    nda.db = template_to_db(nda.temp_db, dbname, nda.ed.statuso.st_uid, nda.ed.statuso.st_gid);

    if (!nda.db) {
        rc = 1;
        goto cleanup;
    }

    /* prepare to insert into the database */
    zeroit(&nda.summary);

    /* prepared statements within db.db */
    nda.entries_res = insertdbprep(nda.db, ENTRIES_INSERT);

    startdb(nda.db);
    descend(ctx, id, pa, in, nda.work, nda.ed.statuso.st_ino, dir, 0,
            processdir_recursive, process_nondir, &nda, NULL);
    stopdb(nda.db);

    insertdbfin(nda.entries_res);

    /* insert this directory's summary data */
    insertsumdb(nda.db, nda.work->name + nda.work->name_len - nda.work->basename_len,
                nda.work, &nda.ed, &nda.summary);

    closedb(nda.db);
    nda.db = NULL;

    /* ignore errors */
    chmod(nda.topath, nda.ed.statuso.st_mode);
    chown(nda.topath, nda.ed.statuso.st_uid, nda.ed.statuso.st_gid);

  cleanup:
    closedir(dir);

    free(data);

    return rc;
}

static int reshape_tree(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    /* Not checking arguments */

    (void) ctx; (void) id;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    struct input *in = &pa->in;
    struct row *w = (struct row *) data;
    const enum DirOp op = w->dir_op;

    /* path of directory in index */
    char indexpath[MAXPATH];

    if ((op == RMDIR) || (op == MKDIR)) {
        SNFORMAT_S(indexpath, MAXPATH, 4,
                   in->nameto.data, in->nameto.len,
                   "/", (size_t) 1,
                   in->name.data + pa->indexbasename_len, in->name.len - pa->indexbasename_len,
                   w->line + in->name.len, w->len - in->name.len);
    }

    row_destroy(w);

    /* delete directory in index as it has been deleted in indexed fs */
    if (op == RMDIR) {
	    return recursive_delete(in, indexpath);
    }

    /* create directory if it doesn't exist in index */
    if (op == MKDIR) {
	    struct stat st;
	    st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
	    st.st_uid = geteuid();
	    st.st_gid = getegid();

	    if (dupdir(indexpath, &st)) {
            const int err = errno;
            fprintf(stderr, "Dupdir failure: \"%s\": %s (%d)\n",
                    indexpath, strerror(err), err);
            return 1;
	    }
    }

    return 0;
}

static int process_renames(QPTPool_t *ctx, const size_t id,
                           const char *changelog_name, const enum DirOp op,
                           QPTPool_f func, const char *wait_msg) {
    (void) op;

    const int fd = open(changelog_name, O_RDONLY);
    if (fd < 0) {
        const int err = errno;
        fprintf(stderr, "Could not open \"%s\": %s (%d)\n", changelog_name, strerror(err), err);
        return 1;
    }

     char *line = NULL;
     size_t size = 0;
     ssize_t len = 0;
     off_t offset = 0;

     while ((len = getline_fd(&line, &size, fd, &offset, GETLINE_DEFAULT_SIZE)) > 0) {
         if (line[0] != '/') {
             continue;
         }

         /* ******************************************* */
         /* validate_source */
         struct stat st;
         if (lstat(line, &st) < 0) {
             fprintf(stderr, "Could not stat source directory \"%s\"\n", line);
             continue;
         }

         /* check that the input path is a directory */
         if (!S_ISDIR(st.st_mode)) {
             fprintf(stderr, "Source path is not a directory \"%s\"\n", line);
             continue;
         }

         struct work *work = calloc(1, sizeof(*work));
         work->name_len = SNFORMAT_S(work->name, MAXPATH, 1, line, len);
         work->root_parent.data = line;
         work->root_parent.len = dirname_len(line, work->name_len);
         work->basename_len = work->name_len - work->root_parent.len;
         /* ******************************************* */

         /* put the current line into a new work item */
         QPTPool_enqueue(ctx, id, func, work);

         /* reuse line buffer */
     }

     free(line);

     printf("%s\n", wait_msg);
     QPTPool_wait(ctx);

     return 0;
}

/* need better function name */
static int process_others(QPTPool_t *ctx, const size_t id,
                          const char *changelog_name, const enum DirOp op,
                          QPTPool_f func, const char *wait_msg) {
    const int fd = open(changelog_name, O_RDONLY);
    if (fd < 0) {
        const int err = errno;
        fprintf(stderr, "Could not open \"%s\": %s (%d)\n", changelog_name, strerror(err), err);
        return 1;
     }

    char *line = NULL;
    size_t size = 0;
    ssize_t len = 0;
    off_t offset = 0;

    while ((len = getline_fd(&line, &size, fd, &offset, GETLINE_DEFAULT_SIZE)) > 0) {
        if (line[0] != '/') {
            free(line);
            line = NULL;
            size = 0;
            continue;
        }

        struct row *row = row_init(line, len, op, offset);
        QPTPool_enqueue(ctx, id, func, row);

        /* have getline allocate a new buffer */
        line = NULL;
        size = 0;
        len = 0;
    }

    free(line);

    printf("%s\n", wait_msg);
    QPTPool_wait(ctx);

    return 0;
}

static void sub_help() {
    printf("changelog_files	parsed changelog, generated from contrib/gen_changelog.py \n");
    printf("indexed_fs	    root of indexed file system\n");
    printf("current_index	update GUFI index here\n");
    printf("\n");
}

int main(int argc, char *argv[]) {
    /* have to call clock_gettime explicitly to get start time and epoch */
    struct start_end main_func;
    clock_gettime(CLOCK_MONOTONIC, &main_func.start);
    epoch = since_epoch(&main_func.start);

    struct PoolArgs pa;
    int idx = parse_cmd_line(argc, argv, "hHn:", 6,
                             "move_changelog_file "
                             "create_changelog_file "
                             "delete_changelog_file "
                             "update_changelog_file "
                             "indexed_fs "
                             "current_index", &pa.in);
    int rc = 0;
    if (pa.in.helped)
        sub_help();
    if (idx < 0) {
        input_fini(&pa.in);
        return -1;
    }
    else {
        /* parse positional args, following the options */
        INSTALL_STR(&pa.in.name,   argv[argc - 2]);
        INSTALL_STR(&pa.in.nameto, argv[argc - 1]);
    }

    init_template_db(&pa.db);
    if (create_dbdb_template(&pa.db) != 0) {
        fprintf(stderr, "Could not create template file\n");
        rc = -1;
        goto cleanup;
    }

    struct stat st;
    st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    st.st_uid = geteuid();
    st.st_gid = getegid();

    if (dupdir(pa.in.nameto.data, &st)) {
        fprintf(stderr, "Could not create directory %s\n", pa.in.nameto.data);
        rc = -1;
        goto close_db_template;
    }

    pa.indexbasename_len = trailing_match_index(pa.in.name.data, pa.in.name.len, "/", 1);

    QPTPool_t *pool = QPTPool_init(pa.in.maxthreads, &pa);
    if (QPTPool_start(pool) != 0) {
        fprintf(stderr, "Error: Failed to start thread pool\n");
        QPTPool_destroy(pool);
        rc = -1;
        goto close_db_template;
    }

    fprintf(stdout, "Updating GUFI Index %s with %zu threads\n", pa.in.nameto.data, pa.in.maxthreads);

    process_renames(pool, 0, argv[idx + 0], RENAME, processdir_recursive, "waiting for moves");
    process_others (pool, 0, argv[idx + 1], MKDIR,  reshape_tree,         "waiting for mkdir");
    process_others (pool, 0, argv[idx + 2], RMDIR,  reshape_tree,         "waiting for removes");
    process_others (pool, 0, argv[idx + 3], UPDATE, processdir,           "waiting for updates");

    QPTPool_stop(pool);

    clock_gettime(CLOCK_MONOTONIC, &main_func.end);
    const long double processtime = sec(nsec(&main_func));

    /* don't count as part of processtime */
    QPTPool_destroy(pool);

    /* set top level permissions */
    chmod(pa.in.nameto.data, S_IRWXU | S_IRWXG | S_IRWXO);

    fprintf(stdout, "Time Spent Indexing: %.2Lfs\n", processtime);

  close_db_template:
    close_template_db(&pa.db);

  cleanup:
    input_fini(&pa.in);

    return rc;
}
