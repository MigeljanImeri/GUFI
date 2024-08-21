#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <lustre/lustreapi.h>

#include "QueuePerThreadPool.h"
#include "utils.h"

static const int RENAME_REC = 8;
static const int MKDIR_REC = 2;
static const int RMDIR_REC = 7;
static const int MAX_PATH = 4096;

enum rec_types {
    RENAME,
    MKDIR,
    RMDIR,
    UPDATES
};

typedef struct pfids {
    std::set<std::string> renames;
    std::set<std::string> mkdirs;
    std::set<std::pair<std::string, std::string>> rmdirs;
    std::set<std::string> updates;

    void insert(const pfids &p) {
        renames.insert(p.renames.begin(), p.renames.end());
        mkdirs.insert(p.mkdirs.begin(), p.mkdirs.end());
        rmdirs.insert(p.rmdirs.begin(), p.rmdirs.end());
        updates.insert(p.updates.begin(), p.updates.end());
    }

    std::vector<std::size_t> size() {
    return {
        renames.size(),
        mkdirs.size(),
        rmdirs.size(),
        updates.size()
        };
    }

} pfids_t;

typedef struct paths {
    std::set<std::string> renames;
    std::set<std::string> mkdirs;
    std::set<std::string> rmdirs;
    std::set<std::string> updates;

    void insert(const paths &p) {
        renames.insert(p.renames.begin(), p.renames.end());
        mkdirs.insert(p.mkdirs.begin(), p.mkdirs.end());
        rmdirs.insert(p.rmdirs.begin(), p.rmdirs.end());
        updates.insert(p.updates.begin(), p.updates.end());
    }
} paths_t;

struct PoolArgs {
    PoolArgs(const size_t max_chunks, const std::string &mnt, const int fd)
        : pfids(max_chunks),
          f_paths(max_chunks),
          mnt(mnt),
          fd(fd) // not taking ownership
        {}

    std::vector<pfids_t> pfids;
    std::vector<paths_t> f_paths;

    pfids_t pfids_merged;

    std::string mnt;
    int fd;
};

struct Range {
    Range(const off_t start = -1, const off_t end = -1)
        : start(start), end(end)
    {}

    off_t start, end;
};

/* Example changelog record
 *rec# operation_type(numerical/text) timestamp datestamp flags
 *t=target_FID ef=extended_flags u=uid:gid nid=client_NID p=parent_FID target_name
 */
struct changelog_record {
    int id;
    int type;
    std::string name;
    std::string time;
    std::string date;
    std::string flags;
    std::string tfid;
    std::string ext_flags;
    std::string uid_gid;
    std::string nid;
    std::string pfid;
    std::string tname;
};

static std::istream & operator>>(std::istream &s, changelog_record &rec) {
    // not checking for errors
    return (s >> rec.id >> rec.type >> rec.name >> rec.time >> rec.date >> rec.flags
            >> rec.tfid >> rec.ext_flags >> rec.uid_gid >> rec.nid >> rec.pfid >> rec.tname);
}

static int block_low(const int id, const std::size_t size, const int num_threads) {
    return (id * size) / num_threads;
}

static int block_high(const int id, const std::size_t size, const int num_threads) {
    return block_low(id + 1, size, num_threads) - 1;
}

static void assign_ranges(std::vector<Range> &ranges, const std::vector<std::size_t> &sizes, int num_threads, int id) {
    ranges[RENAME].start = block_low(id, sizes[RENAME], num_threads);
    ranges[RENAME].end = block_high(id, sizes[RENAME], num_threads);

    ranges[MKDIR].start = block_low(id, sizes[MKDIR], num_threads);
    ranges[MKDIR].end = block_high(id, sizes[MKDIR], num_threads);

    ranges[RMDIR].start = block_low(id, sizes[RMDIR], num_threads);
    ranges[RMDIR].end = block_high(id, sizes[RMDIR], num_threads);

    ranges[UPDATES].start = block_low(id, sizes[UPDATES], num_threads);
    ranges[UPDATES].end = block_high(id, sizes[UPDATES], num_threads);
}

/*
 * given /a/b/c/d, return {/a, /a/b, /a/b/c}
 */
static std::set<std::string> get_parents(const std::string &path) {
    std::set<std::string> parents;

    std::size_t pos = path.rfind("/");
    while (pos != 0) {
        parents.insert(path.substr(0, pos));
        pos = path.rfind("/", pos - 1);
    }

    // implicit move
    return parents;
}
/*
 * given a path and a set of paths, if any of the paths parents exist in the set,
 * we should return true, else false.
 *
 */
static int check_if_erase(const std::string &path, const std::set<std::string> &possible_parents) {
    std::set<std::string> parents = get_parents(path);
    for (const std::string parent: parents) {
        if (possible_parents.find(parent) != possible_parents.end()) {
            return 1;
        }
    }
    return 0;
}
/*
 * paths = {/a/b/c/foobar, a/b/foo}
 * paths_to_remove = {/a/b/c}
 * => paths = {/a/b/foo}
 */
static void remove_overlap(std::set<std::string> &paths, const std::set<std::string> &paths_to_remove) {
    std::set<std::string>::iterator it = paths.begin();
    while (it != paths.end()) {
        if (check_if_erase(*it, paths_to_remove)) {
            it = paths.erase(it);
        }
        else {
            ++it;
        }
    }
}

static void remove_overlaps(paths_t &f_paths) {
    remove_overlap(f_paths.renames, f_paths.renames);
    remove_overlap(f_paths.mkdirs, f_paths.renames);
    remove_overlap(f_paths.rmdirs, f_paths.renames);
    remove_overlap(f_paths.updates, f_paths.renames);
    return;
}

static void write_output(const std::string &filename, const std::set<std::string> &paths) {
    std::ofstream out(filename);
    for (const std::string &p: paths) {
        out << p << "\n";
    }
}

static void write_outputs(const std::string &filename, const paths_t &paths) {
    write_output(filename + ".renames", paths.renames);
    write_output(filename + ".mkdirs",  paths.mkdirs);
    write_output(filename + ".rmdirs",  paths.rmdirs);
    write_output(filename + ".updates", paths.updates);
}
/*
 * 1 - is dir
 * 0 - not dir
 * -1 - path couldn't be statted
 * */
int check_if_dir(const std::string &path) {
    struct stat st;

    if (stat(path.c_str(), &st) != 0) {
        return -1;
    }

    return S_ISDIR(st.st_mode);
}

int fid2path(const std::string& mnt_path, const std::string& fid, char *path,
             int pathlen, long long *recno, int *linkno) {
    return llapi_fid2path(mnt_path.c_str(), fid.c_str(), path, pathlen, recno, linkno);
}

void process_range_rmdir(struct PoolArgs *pa, const Range &range, std::set<std::string> &paths,
                         std::set<std::pair<std::string, std::string>> &pfids) {
    std::set<std::pair<std::string, std::string>>::iterator start = pfids.begin();
    std::set<std::pair<std::string, std::string>>::iterator end = pfids.begin();

    std::advance(start, range.start);
    std::advance(end, range.end + 1);

    char buffer[MAX_PATH];
    while (start != end) {
        std::pair<std::string, std::string> pfid = *start;
        //not checking errors
        if (fid2path(pa->mnt, pfid.first, buffer, sizeof(buffer), NULL, NULL) == 0) {
            paths.insert(pa->mnt + "/" + buffer + "/" + pfid.second);
        }
        start = std::next(start);
    }
}

void process_range(struct PoolArgs *pa, const Range &range, std::set<std::string> &paths,
                   std::set<std::string> &pfids) {
    std::set<std::string>::iterator start = pfids.begin();
    std::set<std::string>::iterator end = pfids.begin();

    std::advance(start, range.start);
    std::advance(end, range.end + 1);

    char buffer[MAX_PATH];
    while(start != end) {
        std::string pfid = *start;
        //not checking errors
        if (fid2path(pa->mnt, pfid, buffer, sizeof(buffer), NULL, NULL) == 0) {
            paths.insert(pa->mnt + "/" + buffer);
        }
        start = std::next(start);
    }
}
int process_pfids(QPTPool_t *ctx, const std::size_t id, void *data, void *args) {
    (void) ctx;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    std::vector<paths_t> &paths = pa->f_paths;
    paths_t &path = paths[id];

    std::vector<Range> *ranges_ptr = (std::vector<Range> *) data;
    std::vector<Range> &ranges = *ranges_ptr;

    process_range(pa, ranges[RENAME], path.renames, pa->pfids_merged.renames);
    process_range(pa, ranges[MKDIR], path.mkdirs, pa->pfids_merged.mkdirs);
    process_range_rmdir(pa, ranges[RMDIR], path.rmdirs, pa->pfids_merged.rmdirs);
    process_range(pa, ranges[UPDATES], path.updates, pa->pfids_merged.updates);

    delete ranges_ptr;

    return 0;
}

int process_lines(QPTPool_t *ctx, const std::size_t id, void *data, void *args) {
    (void) ctx;

    struct PoolArgs *pa = (struct PoolArgs *) args;
    std::vector<pfids_t> &pfids = pa->pfids;
    pfids_t &pfid = pfids[id];

    Range *range = (Range *) data;


    char *line = NULL;
    std::size_t size = 0;
    while ((range->start < range->end) &&
           (getline_fd(&line, &size, pa->fd, &range->start, GETLINE_DEFAULT_SIZE) > 0))  {

        changelog_record rec;
        std::stringstream s(line);
        s >> rec;

        switch (rec.type) {
            case MKDIR_REC:
                pfid.mkdirs.insert(rec.tfid.c_str() + 2);
                pfid.updates.insert(rec.tfid.c_str() + 2);
                break;
            case RMDIR_REC:
                pfid.rmdirs.insert({rec.pfid.c_str() + 2, rec.tname});
                break;
            case RENAME_REC:
                {
                    std::string sfid;
                    std::string spfid;
                    s >> sfid >> spfid;

                    char path[MAX_PATH];
                    int is_dir;
                    if (fid2path(pa->mnt, sfid.c_str() + 2, path, sizeof(path), NULL, NULL) == 0) {
                        is_dir = check_if_dir(pa->mnt + '/' + path);

                        //path doesn't exist, have to treat as if directory due to rename case
                        if (is_dir == -1) {
                            is_dir = 1;
                        }
                    }
                    //fid2path doesn't resolve, have to assume fid being renamed is a directory
                    else {
                        is_dir = 1;
                    }

                    if (is_dir == 1) {
                        pfid.renames.insert(rec.pfid.c_str() + 2);
                        pfid.renames.insert(spfid.c_str() + 3);
                    }
                    else {
                        pfid.updates.insert(rec.pfid.c_str() + 2);
                        pfid.updates.insert(spfid.c_str() + 3);
                    }
                }
                break;
            default:
                pfid.updates.insert(rec.pfid.c_str() + 2);
                break;
        }
    }

    free(line);
    delete range;

    return 0;
}

int main(int argc, char *argv[]) {
    /* TODO: Parse argv */
    const std::size_t count = 32;
    const std::string changelog_name = "/home/mimeri/repos/scripts/changelog";
    const std::string mnt = "/mnt/lustre";

    int changelog = open(changelog_name.c_str(), O_RDONLY);
    if (changelog < 0) {
        const int err = errno;
        fprintf(stderr, "Could not open changelog file: %s (%d)\n",
                strerror(err), err);
        return 1;
    }

    struct stat st;
    if (fstat(changelog, &st) != 0) {
        const int err = errno;
        fprintf(stderr, "Could not fstat changelog file: %s (%d)\n",
                strerror(err), err);
        close(changelog);
        return 1;
    }

    struct PoolArgs pa(count, mnt, changelog);

    QPTPool_t *pool = QPTPool_init(count, &pa);
    if (QPTPool_start(pool) != 0) {
        fprintf(stderr, "Error: Failed to start thread pool with %zu threads\n", count);
        QPTPool_destroy(pool);
        close(changelog);
        return 1;
    }

    /* approximate number of bytes per chunk */
    const std::size_t jump = (st.st_size / count) + !!(st.st_size % count);

    off_t start = 0;
    off_t end = start + jump;
    char *line = NULL;
    std::size_t size = 0;
    while ((start < st.st_size) &&
           (getline_fd(&line, &size, changelog, &end, GETLINE_DEFAULT_SIZE)) > 0) {

        Range *range = new Range(start, end);
        QPTPool_enqueue(pool, 0, process_lines, range);

        /* jump to next chunk */
        start = end;
        end += jump;
    }

    if (start < st.st_size) {
        Range *range = new Range(start, st.st_size);
        QPTPool_enqueue(pool, 0, process_lines, range);
    }

    QPTPool_wait(pool);

    // don't need changelog any more
    close(changelog);

    for (const pfids_t &pfid: pa.pfids) {
        pa.pfids_merged.insert(pfid);
    }

    printf("Sets of pfids before converting to paths\n");
    printf("rename size: %zu\n", pa.pfids_merged.renames.size());
    printf("mkdirs size: %zu\n", pa.pfids_merged.mkdirs.size());
    printf("rmdirs size: %zu\n", pa.pfids_merged.rmdirs.size());
    printf("updates size: %zu\n", pa.pfids_merged.updates.size());

    const std::vector<std::size_t> merge_sizes = pa.pfids_merged.size();

    for (std::size_t i = 0; i < count; i++) {
        std::vector<Range> *ranges = new std::vector<Range>(4);
        assign_ranges(*ranges, merge_sizes, count, i);

        QPTPool_enqueue(pool, 0, process_pfids, ranges);
    }

    QPTPool_wait(pool);

    paths_t f_paths_merge;

    for (const paths_t path: pa.f_paths) {
        f_paths_merge.insert(path);
    }

    printf("Sets of paths after converting from pfids\n");
    printf("rename path size: %zu\n", f_paths_merge.renames.size());
    printf("mkdirs path size: %zu\n", f_paths_merge.mkdirs.size());
    printf("rmdirs path size: %zu\n", f_paths_merge.rmdirs.size());
    printf("updates path size: %zu\n", f_paths_merge.updates.size());

    remove_overlaps(f_paths_merge);

    printf("Sets of paths after removing overlapping work\n");
    printf("rename path size: %zu\n", f_paths_merge.renames.size());
    printf("mkdirs path size: %zu\n", f_paths_merge.mkdirs.size());
    printf("rmdirs path size: %zu\n", f_paths_merge.rmdirs.size());
    printf("updates path size: %zu\n", f_paths_merge.updates.size());

    write_outputs("changelog", f_paths_merge);

    QPTPool_stop(pool);
    QPTPool_destroy(pool);

    return 0;

}
