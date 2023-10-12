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

#include "QueuePerThreadPool.h"
#include "bf.h"
#include "debug.h"
#include "dbutils.h"
#include "external.h"
#include "template_db.h"
#include "utils.h"
#include "BottomUp.h"

#if defined(DEBUG) && defined(PER_THREAD_STATS)
#include "OutputBuffers.h"
struct OutputBuffers debug_output_buffers;
#endif

#define GETLINE_DEFAULT_SIZE 750 /* magic number */

#define MKDIR 1
#define RMDIR 2
#define RENAME 3 

struct ScoutArgs {
    struct input *in;  /* reference to PoolArgs */
    struct PoolArgs *pa;

    /* file descriptors */
    int move_changelog;
    int create_changelog;         
    int delete_changelog;
    int update_changelog;

    /* everything below is locked with print_mutex from debug.h */

    size_t *remaining; /* number of scouts still running */

    /* sum of counts from all changelog files */
    uint64_t *time;
    size_t *files;
    size_t *dirs;
    size_t *empty;     /* number of directories without files/links
                        * can still have child directories */
};

/* global to pool - passed around in "args" argument */
struct PoolArgs {
    struct input in;
    struct template_db db;
    struct template_db xattr;
    uint64_t *total_files;
};

struct NonDirArgs {
    struct input *in;

    /* thread args */
    struct template_db *temp_db;
    struct template_db *temp_xattr;
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
    sqlite3_stmt *xattrs_res;
    sqlite3_stmt *xattr_files_res;

    /* list of xattr dbs */
    sll_t xattr_db_list;
};

/* Data stored during first pass of input file */
struct row {
    int changelog;
    char *line;
    size_t len;
    int dir_op;  /* MKDIR or RMDIR or RENAME */
    long offset;
    size_t entries;
};

static struct row *row_init(const int changelog, char *line, const size_t len, 
		const int dir_op, const long offset) {
    struct row *row = malloc(sizeof(struct row));
    if (row) {
        //row->changelog = changelog;
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
    int rc; 
    struct stat sb;

    if (lstat(path, &sb) == -1) {
	    rc = -1;
	    return rc;
    }
    char** root_names = malloc(1 * sizeof(char*));
    root_names[0] = path;
    rc = parallel_bottomup(root_names, 1, in->maxthreads,
    		sizeof(struct BottomUp),
    		NULL, rm_dir,
    		1,
    		0,
    		NULL);
    free(root_names);
    return rc;

}

static int process_nondir(struct work *entry, struct entry_data *ed, void *args) {
    struct NonDirArgs *nda = (struct NonDirArgs *) args;
    struct input *in = nda->in;

    if (lstat(entry->name, &ed->statuso) != 0) {
        return 1;
    }

    if (ed->type == 'l') {
        readlink(entry->name, ed->linkname, MAXPATH);
        /* error? */
    }

    if (in->process_xattrs) {
        insertdbgo_xattrs(in, &nda->ed.statuso, entry, ed,
                          &nda->xattr_db_list, nda->temp_xattr,
                          nda->topath, nda->topath_len,
                          nda->xattrs_res, nda->xattr_files_res);
    }

    /* get entry relative path (use extra buffer to prevent memcpy overlap) */
    char relpath[MAXPATH];
    const size_t relpath_len = SNFORMAT_S(relpath, MAXPATH, 1,
                                          entry->name + entry->root_parent.len, entry->name_len - entry->root_parent.len);
    /* overwrite full path with relative path */
    entry->name_len = SNFORMAT_S(entry->name, MAXPATH, 1, relpath, relpath_len);

    /* update summary table */
    sumit(&nda->summary, ed);

    /* add entry + xattr names into bulk insert */
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
    size_t nondirs_processed = 0;
    char *temp;
    int rc;

    struct NonDirArgs nda;
    nda.in = &pa->in;
    nda.temp_db = &pa->db;
    nda.temp_xattr = &pa->xattr;

    temp = realpath(w->line, NULL);

    if (!temp) {
	const int err = errno;
	fprintf(stderr, "realpath failure: \"%s\": %s (%d)\n",
		w->line, strerror(err), err);
	row_destroy(w);
	return 1;
    }

    free(w->line);
    w->line = temp;
    w->len = strlen(w->line);
    temp = NULL;

    char index_input[MAXPATH];
    const size_t index_len = SNFORMAT_S(index_input, MAXPATH, 1, 
		    			in->name.data, in->name.len);
    //get basename of index 
    size_t indexbasename_index = trailing_match_index(index_input, index_len, "/", 1);

    /* actual path of directory that exists in indexed system*/
    char topath[MAXPATH];
    const size_t topath_len = SNFORMAT_S(topath, MAXPATH, 1, w->line, w->len);

    /* parent of path of directory that exists in indexed system*/
    char topath_parent[MAXPATH];
    const size_t topath_parent_len = SNFORMAT_S(topath_parent, MAXPATH, 1, 
		    topath, dirname_len(topath, topath_len));

    /* path of directory in index*/
    char indexpath[MAXPATH];
    const size_t indexpath_len = SNFORMAT_S(indexpath, MAXPATH, 4,
		    			in->nameto.data, in->nameto.len,
					"/", (size_t) 1,
					index_input + indexbasename_index, index_len - indexbasename_index,
					topath + in->name.len, strlen(topath + in->name.len));

    refstr_t parent = {
	    .data = topath, 
	    .len = topath_len
    };

    if (lstat(topath, &nda.ed.statuso) != 0) {
	fprintf(stderr, "Could not stat directory \"%s\"\n", topath);		
	rc = 1;
	goto cleanup;
    }

    struct work nda_parent;
    memset(&nda_parent, 0, sizeof(nda_parent));

    struct stat topath_parent_stat;

    if (lstat(topath_parent, &topath_parent_stat) != 0) {
	fprintf(stderr, "Could not stat directory \"%s\"\n", topath_parent);		
	rc = 1;
	goto cleanup;
    }
    

    nda.work = &nda_parent;
    nda.work->root_parent = parent;
    nda.work->pinode = topath_parent_stat.st_ino;
    nda.ed.type = 'd';

    DIR *dir = opendir(topath);

    if (!dir) {
	fprintf(stderr, "In processdir\nCould not open directory \"%s\"\n", topath);		
	rc = 1;
	goto cleanup;
    }

    /* check if directory exists in index, not necessarily an error if it doesn't.
     * likely caused by moves happening, will be fixed later on.
     * */
    if (opendir(indexpath) == NULL) {
	const int err = errno;
	if (err == ENOENT) {
	    nda.db = NULL;
	    goto cleanup;
	}
    }

    /* create the database name */
    char dbname[MAXPATH];
    SNFORMAT_S(dbname, MAXPATH, 3,
               indexpath, indexpath_len,
	       "/", (size_t) 1,
	       DBNAME, DBNAME_LEN);

    nda.db = template_to_db(&pa->db, dbname, nda.ed.statuso.st_uid, nda.ed.statuso.st_gid);

    if (!nda.db) {
        rc = 1;
	printf("Going to cleanup\n");
        goto cleanup;
    }
 
    zeroit(&nda.summary);

    nda.entries_res = insertdbprep(nda.db, ENTRIES_INSERT);
    nda.xattrs_res = NULL;
    nda.xattr_files_res = NULL;

    if (in->process_xattrs) {
	 nda.xattrs_res = insertdbprep(nda.db, XATTRS_PWD_INSERT);
	 nda.xattr_files_res = insertdbprep(nda.db, EXTERNAL_DBS_PWD_INSERT);

	 /* external per-user and per-group dbs */
	 sll_init(&nda.xattr_db_list);
    }

    /* Read through directory on indexed file system and add files to db */
    startdb(nda.db);
    if (nda.db) {

	struct dirent *dir_child = NULL;
        while ((dir_child = readdir(dir))) {
            const size_t len = strlen(dir_child->d_name);

            /* skip . and .. and *.db */
	    const int skip = (trie_search(in->skip, dir_child->d_name, len, NULL) ||
			    (0 && (len >= 3) && (strncmp(dir_child->d_name + len - 3, ".db", 3) == 0)));

            if (skip) {
                continue;
            }


            struct work child;
            memset(&child, 0, sizeof(child));

            struct entry_data child_ed;
            memset(&child_ed, 0, sizeof(child_ed));

            /* get child path */
            child.name_len = SNFORMAT_S(child.name, MAXPATH, 3,
                                        topath, topath_len,
                                        "/", (size_t) 1,
                                        dir_child->d_name, len);
            child.basename_len = len;
            child.root_parent = parent;
            child.pinode = nda.ed.statuso.st_ino;

            if (lstat(child.name, &child_ed.statuso) < 0) {
                continue;
            }

            /* ignore subdirectories  */
            if (S_ISDIR(child_ed.statuso.st_mode)) {
                child_ed.type = 'd';
                continue;
            }
            /* non directories */
            else if (S_ISLNK(child_ed.statuso.st_mode)) {
                child_ed.type = 'l';
                readlink(child.name, child_ed.linkname, MAXPATH);
            }
            else if (S_ISREG(child_ed.statuso.st_mode)) {
                child_ed.type = 'f';
            }
            else {
                /* other types are not stored */
                continue;
            }

            if (in->process_xattrs) {
                xattrs_setup(&child_ed.xattrs);
                xattrs_get(child.name, &child_ed.xattrs);
            }

            process_nondir(&child, &child_ed, &nda);
	    nondirs_processed++;

            if (in->process_xattrs) {
                xattrs_cleanup(&child_ed.xattrs);
            }
        }

    }
    stopdb(nda.db);

    if (in->process_xattrs) {
        /* write out per-user and per-group xattrs */
        sll_destroy(&nda.xattr_db_list, destroy_xattr_db);

        /* keep track of per-user and per-group xattr dbs */
        insertdbfin(nda.xattr_files_res);

        /* pull this directory's xattrs because they were not pulled by the parent */
        xattrs_setup(&nda.ed.xattrs);
        xattrs_get(parent.data, &nda.ed.xattrs);

        /* directory xattrs go into the same table as entries xattrs */
        insertdbgo_xattrs_avail(&nda.ed, nda.xattrs_res);
        insertdbfin(nda.xattrs_res);
    }
    insertdbfin(nda.entries_res);

    /* insert this directory's summary data */
    /* the xattrs go into the xattrs_avail table in db.db */
    size_t index = trailing_match_index(parent.data, parent.len, "//", 1);
    insertsumdb(nda.db, parent.data + index,
                nda.work, &nda.ed, &nda.summary);
    if (in->process_xattrs) {
        xattrs_cleanup(&nda.ed.xattrs);
    }

    closedb(nda.db);
    nda.db = NULL;

    /* ignore errors */
    chmod(indexpath, nda.ed.statuso.st_mode);
    chown(indexpath, nda.ed.statuso.st_uid, nda.ed.statuso.st_gid);


cleanup:
    closedir(dir);
    pa->total_files[id] += nondirs_processed;
    row_destroy(w);

    return !nda.db;
}

static int processdir_recursive(QPTPool_t *ctx, const size_t id, void *data, void *args) {

    int rc = 0;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    struct input *in = &pa->in;

    struct work work_src;

    struct NonDirArgs nda;
    nda.in         = &pa->in;
    nda.temp_db    = &pa->db;
    nda.temp_xattr = &pa->xattr;
    nda.work = (struct work *) data;
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

    /* offset by work->root_len to remove prefix */
    /*
    nda.topath_len = SNFORMAT_S(nda.topath, MAXPATH, 3,
                                in->nameto.data, in->nameto.len,
                                "/", (size_t) 1,
                                nda.work->name + nda.work->root_parent.len, nda.work->name_len - nda.work->root_parent.len);
				*/

    size_t index_basename_index = trailing_match_index(in->name.data, in->name.len, "/", 1);

    nda.topath_len = SNFORMAT_S(nda.topath, MAXPATH, 4,
		    		in->nameto.data, in->nameto.len,
				"/", (size_t) 1,
				in->name.data + index_basename_index, in->name.len - index_basename_index,
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
	     * below it, will eventually reindex this directory through move that occured above this one*/
	    /*
	    if (err != ENOENT) {
	    }
	    */
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
    nda.xattrs_res = NULL;
    nda.xattr_files_res = NULL;

    if (in->process_xattrs) {
        nda.xattrs_res = insertdbprep(nda.db, XATTRS_PWD_INSERT);
        nda.xattr_files_res = insertdbprep(nda.db, EXTERNAL_DBS_PWD_INSERT);

        /* external per-user and per-group dbs */
        sll_init(&nda.xattr_db_list);
    }

    struct descend_counters ctrs;
    startdb(nda.db);
    descend(ctx, id, pa, in, nda.work, nda.ed.statuso.st_ino, dir, in->skip, 0, 0,
            processdir_recursive, process_nondir, &nda, &ctrs);
    stopdb(nda.db);

    /* entries and xattrs have been inserted */
    if (in->process_xattrs) {
        /* write out per-user and per-group xattrs */
        sll_destroy(&nda.xattr_db_list, destroy_xattr_db);

        /* keep track of per-user and per-group xattr dbs */
        insertdbfin(nda.xattr_files_res);

        /* pull this directory's xattrs because they were not pulled by the parent */
        xattrs_setup(&nda.ed.xattrs);
        xattrs_get(nda.work->name, &nda.ed.xattrs);

        /* directory xattrs go into the same table as entries xattrs */
        insertdbgo_xattrs_avail(&nda.ed, nda.xattrs_res);
        insertdbfin(nda.xattrs_res);
    }
    insertdbfin(nda.entries_res);

    /* insert this directory's summary data */
    /* the xattrs go into the xattrs_avail table in db.db */
    insertsumdb(nda.db, nda.work->name + nda.work->name_len - nda.work->basename_len,
                nda.work, &nda.ed, &nda.summary);
    if (in->process_xattrs) {
        xattrs_cleanup(&nda.ed.xattrs);
    }

    closedb(nda.db);
    nda.db = NULL;

    /* ignore errors */
    chmod(nda.topath, nda.ed.statuso.st_mode);
    chown(nda.topath, nda.ed.statuso.st_uid, nda.ed.statuso.st_gid);

  cleanup:
    closedir(dir);

    free_struct(nda.work, data, nda.work->recursion_level);

    pa->total_files[id] += ctrs.nondirs_processed;

    return rc;
}

static int reshape_tree(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    /* Not checking arguments */

    (void) ctx; (void) id;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    struct input *in = &pa->in;
    struct row *w = (struct row *) data;
    int rc;

    char index[MAXPATH];
    const size_t index_len = SNFORMAT_S(index, MAXPATH, 1, 
		    			in->name.data, in->name.len);
    //get basename of index 
    char *indexbasename = basename(index);
    
    /* path of directory in index*/
    char indexpath[MAXPATH];
    const size_t indexpath_len = SNFORMAT_S(indexpath, MAXPATH, 4,
		    			in->nameto.data, in->nameto.len,
					"/", (size_t) 1,
					indexbasename, (size_t) strlen(indexbasename),
					w->line + in->name.len, w->len - in->name.len);

    /* delete directory in index as it has been deleted in indexed fs*/
    if (w->dir_op == RMDIR) {
	    return recursive_delete(in, indexpath);
    }

    /* create directory if it doesn't exist in index*/
    if (w->dir_op == MKDIR) {
	    struct stat st;
	    st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
	    st.st_uid = geteuid();
	    st.st_gid = getegid();

	    if (dupdir(indexpath, &st)) {
		const int err = errno;
		fprintf(stderr, "Dupdir failure: \"%s\": %s (%d)\n",
			indexpath, strerror(err), err);
		row_destroy(w);
		return 1;
	    }
    }
}

static int validate_source(struct input *in, const char *path, struct work *work) {
    memset(work, 0, sizeof(*work));

    /* get input path metadata */
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "Could not stat source directory \"%s\"\n", path);
        return 1;
    }

    /* check that the input path is a directory */
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Source path is not a directory \"%s\"\n", path);
        return 1;
    }

    work->name_len = SNFORMAT_S(work->name, MAXPATH, 1, path, strlen(path));
    work->root_parent.data = path;
    work->root_parent.len = dirname_len(path, work->name_len);

    char expathin[MAXPATH];
    char expathout[MAXPATH];
    char expathtst[MAXPATH];

    SNPRINTF(expathtst, MAXPATH,"%s/%s", in->nameto.data, work->root_parent.data + work->root_parent.len);
    realpath(expathtst, expathout);
    realpath(work->root_parent.data, expathin);

    if (!strcmp(expathin, expathout)) {
        fprintf(stderr,"You are putting the index dbs in input directory\n");
    }

    return 0;
}


static int scout_function(QPTPool_t *ctx, const size_t id, void *data, void *args) {
    struct start_end scouting;
    clock_gettime(CLOCK_MONOTONIC, &scouting.start);

    /* skip argument checking */
    struct ScoutArgs *sa = (struct ScoutArgs *) data;
    struct input *in = sa->in;

    (void) id; (void) args;

    char *line = NULL;
    size_t size = 0;
    ssize_t len = 0;
    off_t offset = 0;
    int dir_op = 0;
    struct row* work;
    size_t file_count = 0;
    size_t dir_count = 1; /* always start with a directory */
    size_t empty = 0;

    size_t index_basename_index = trailing_match_index(in->name.data, in->name.len, "/", 1);

    /* dealing with moves, delete everything under source and destination parent as suspect
     * then recursively index  */
    while ((len = getline_fd(&line, &size, sa->move_changelog, &offset, GETLINE_DEFAULT_SIZE)) > 0) {

	if (line[0] != '/') {
		continue;
	}
    
	struct work root;
	if (validate_source(sa->in, line, &root) != 0) {
	    continue;
	}

        root.basename_len = root.name_len - root.root_parent.len;

        /* put the current line into a new work item */
	struct work *copy = compress_struct(sa->in->compress, &root, sizeof(root));
        QPTPool_enqueue(ctx, id, processdir_recursive, copy);

        /* have getline allocate a new buffer */
        line = NULL;
        size = 0;
        len = 0;
    }

    printf("waiting for moves\n");
    while (QPTPool_incomplete(ctx) != 1) {
	usleep(1);
    }

    offset = 0;
    dir_op = MKDIR;
    /* create directories */
    while ((len = getline_fd(&line, &size, sa->create_changelog, &offset, GETLINE_DEFAULT_SIZE)) > 0) {

	if (line[0] != '/') {
		continue;
	}

	work = row_init(sa->create_changelog, line, len, dir_op, offset);

        /* put the current line into a new work item */
        QPTPool_enqueue(ctx, id, reshape_tree, work);

        /* have getline allocate a new buffer */
        line = NULL;
        size = 0;
        len = 0;
    }

    printf("waiting for mkdir\n");
    while (QPTPool_incomplete(ctx) != 1) {
	usleep(1);
    }

    offset = 0;
    dir_op = RMDIR;
    /* delete directories */
    while ((len = getline_fd(&line, &size, sa->delete_changelog, &offset, GETLINE_DEFAULT_SIZE)) > 0) {

	if (line[0] != '/') {
		continue;
	}

	work = row_init(sa->create_changelog, line, len, dir_op, offset);

        /* put the current line into a new work item */
        QPTPool_enqueue(ctx, id, reshape_tree, work);

        /* have getline allocate a new buffer */
        line = NULL;
        size = 0;
        len = 0;
    }

    printf("waiting for removes\n");
    while (QPTPool_incomplete(ctx) != 1) {
	usleep(1);
    }

    offset = 0;
    /* update indexes */
    while ((len = getline_fd(&line, &size, sa->update_changelog, &offset, GETLINE_DEFAULT_SIZE)) > 0) {

    	if (line[0] != '/') {
		continue;
	}


        /* put the current line into a new work item */
        work = row_init(sa->update_changelog, line, len, dir_op, offset);
        QPTPool_enqueue(ctx, id, processdir, work);

        dir_count++;
        empty += !work->entries;

        /* have getline allocate a new buffer */
        line = NULL;
        size = 0;
        len = 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &scouting.end);

    pthread_mutex_lock(&print_mutex);
    *sa->time += nsec(&scouting);
    *sa->files += file_count;
    *sa->dirs += dir_count;
    *sa->empty += empty;

    (*sa->remaining)--;

    /* print here to print as early as possible instead of after thread pool completes */
    /*
    if ((*sa->remaining) == 0) {
        fprintf(stdout, "Scouts took total of %.2Lf seconds\n", sec(*sa->time));
        fprintf(stdout, "Dirs:                %zu (%zu empty)\n", *sa->dirs, *sa->empty);
        fprintf(stdout, "Files:               %zu\n", *sa->files);
        fprintf(stdout, "Total:               %zu\n", *sa->files + *sa->dirs);
        fprintf(stdout, "\n");
    }
    */
    pthread_mutex_unlock(&print_mutex);

    free(sa);

    return 0;
}

static void sub_help() {
   printf("changelog_files	parsed changelog, generated from contrib/gen_changelog.py \n");
   printf("indexed_fs	root of indexed file system\n");
   printf("current_index	update GUFI index here\n");
   printf("\n");
}

int main(int argc, char *argv[]) {
    /* have to call clock_gettime explicitly to get start time and epoch */
    struct start_end main_func;
    clock_gettime(CLOCK_MONOTONIC, &main_func.start);
    epoch = since_epoch(&main_func.start);

    struct PoolArgs pa;
    int idx = parse_cmd_line(argc, argv, "hHn:d:M:x" ":q", 6, "move_changelog_file create_changelog_file\
	delete_changelog_file update_changelog_file indexed_fs current_index", &pa.in);
    int rc;
    if (pa.in.helped)
        sub_help();
    if (idx < 0)
        return -1;
    else {
        /* parse positional args, following the options */
        INSTALL_STR(&pa.in.nameto, argv[argc - 1]);
        INSTALL_STR(&pa.in.name, argv[argc - 2]);
    }

    int move_changelog = open(argv[idx], O_RDONLY);
    if (move_changelog < 0) {
        const int err = errno;
        fprintf(stderr, "Could not open \"%s\": %s (%d)\n", argv[idx], strerror(err), err);
	rc = err;
	goto close_move_changelog;
    }

    int create_changelog = open(argv[idx + 1], O_RDONLY);
    if (create_changelog < 0) {
        const int err = errno;
        fprintf(stderr, "Could not open \"%s\": %s (%d)\n", argv[idx + 1], strerror(err), err);
	rc = err;
	goto close_create_changelog;
    }

    int delete_changelog = open(argv[idx + 2], O_RDONLY);
    if (delete_changelog < 0) {
        const int err = errno;
        fprintf(stderr, "Could not open \"%s\": %s (%d)\n", argv[idx + 2], strerror(err), err);
	rc = err;
	goto close_delete_changelog;
    }

    int update_changelog = open(argv[idx + 3], O_RDONLY);
    if (update_changelog < 0) {
        const int err = errno;
        fprintf(stderr, "Could not open \"%s\": %s (%d)\n", argv[idx + 3], strerror(err), err);
	rc = err;
	goto close_update_changelog;
    }

    init_template_db(&pa.db);
    if (create_dbdb_template(&pa.db) != 0) {
        fprintf(stderr, "Could not create template file\n");
	rc = -1;
	goto close_update_changelog;
    }

    struct stat st;
    st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    st.st_uid = geteuid();
    st.st_gid = getegid();

    if (dupdir(pa.in.nameto.data, &st)) {
        fprintf(stderr, "Could not create directory %s\n", pa.in.nameto.data);
	rc = -1;
	goto close_update_changelog;
    }

    /*
    if (setup_directory_skip(&pa.skip, pa.in.skip->data) != 0) {
        rc = EXIT_FAILURE;
	goto close_update_changelog;
    }
    */

    /*
     * create empty db.db in index parent (this file is placed in
     * "${dst}/db.db"; index is placed in "${dst}/$(basename ${src}))"
     * so that when querying "${dst}", no error is printed
     */
    if (create_empty_dbdb(&pa.db, &pa.in.nameto, geteuid(), getegid()) != 0) {
	rc = -1;
	goto close_update_changelog;
    }

    init_template_db(&pa.xattr);
    if (create_xattrs_template(&pa.xattr) != 0) {
        fprintf(stderr, "Could not create xattr template file\n");
	rc = -1;
	goto close_db_template;
    }

    const uint64_t queue_depth = pa.in.target_memory_footprint / sizeof(struct work) / pa.in.maxthreads;
    QPTPool_t *pool = QPTPool_init_with_props(pa.in.maxthreads, &pa, NULL, NULL, queue_depth, 1, 2);
    if (QPTPool_start(pool) != 0) {
        fprintf(stderr, "Error: Failed to start thread pool\n");
        QPTPool_destroy(pool);
	rc = -1;
	goto close_db_template;
    }

    fprintf(stdout, "Updating GUFI Index %s with %zu threads\n", pa.in.nameto.data, pa.in.maxthreads);

    pa.total_files = calloc(pa.in.maxthreads, sizeof(uint64_t));

    /* parse the changelog files */
    size_t remaining = 1;
    uint64_t scout_time = 0;
    size_t files = 0;
    size_t dirs = 0;
    size_t empty = 0;

    /* freed by scout_function */
    struct ScoutArgs *sa = malloc(sizeof(struct ScoutArgs));
    sa->in = &pa.in;
    sa->move_changelog = move_changelog;
    sa->create_changelog = create_changelog;
    sa->delete_changelog = delete_changelog;
    sa->update_changelog = update_changelog;
    sa->remaining = &remaining;
    sa->time = &scout_time;
    sa->files = &files;
    sa->dirs = &dirs;
    sa->empty = &empty;

    sa->pa = &pa;

    /* scout_function pushes more work into the queue */
    QPTPool_enqueue(pool, 0, scout_function, sa);

    QPTPool_wait(pool);

    clock_gettime(CLOCK_MONOTONIC, &main_func.end);
    const long double processtime = sec(nsec(&main_func));

    /* don't count as part of processtime */
    QPTPool_destroy(pool);

    /* set top level permissions */
    chmod(pa.in.nameto.data, S_IRWXU | S_IRWXG | S_IRWXO);

    files = 0;
    for (size_t i = 0; i < pa.in.maxthreads; i++) {
	    files += pa.total_files[i];
    }

    fprintf(stdout, "Total Dirs:          %" PRIu64 "\n", dirs);
    fprintf(stdout, "Total Files:         %" PRIu64 "\n", files);
    fprintf(stdout, "Time Spent Indexing: %.2Lfs\n",      processtime);
    fprintf(stdout, "Dirs/Sec:            %.2Lf\n",       dirs / processtime);
    fprintf(stdout, "Files/Sec:           %.2Lf\n",       files / processtime);

    input_fini(&pa.in);
close_xattr_template:
    close_template_db(&pa.xattr);
close_db_template:
    close_template_db(&pa.db);
close_update_changelog:
    close(update_changelog);
close_delete_changelog:
    close(delete_changelog);
close_create_changelog:
    close(create_changelog);
close_move_changelog:
    close(move_changelog);

    return rc;
}
