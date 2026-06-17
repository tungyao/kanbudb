#include "multi.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── SHM ──────────────────────────────────────────────────── */

static size_t shm_size(void) {
    return sizeof(struct kanbudb_shm_header);
}

void kanbudb_shm_init(struct kanbudb_shm_header* hdr) {
    memset(hdr, 0, shm_size());
    hdr->magic = KANBUDB_SHM_MAGIC;
}

int kanbudb_shm_open(const char* db_path, kanbudb_shm_t* out) {
    char shm_path[1024];
    snprintf(shm_path, sizeof(shm_path), "%s.shm", db_path);

    out->fd = open(shm_path, O_RDWR | O_CREAT, 0644);
    if (out->fd < 0) return KANBUDB_ERR_IO;

    size_t sz = shm_size();
    off_t flen = lseek(out->fd, 0, SEEK_END);
    if (flen == 0) {
        if (ftruncate(out->fd, (off_t)sz) < 0) {
            close(out->fd); return KANBUDB_ERR_IO;
        }
        void* ptr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
        if (ptr == MAP_FAILED) { close(out->fd); return KANBUDB_ERR_IO; }
        kanbudb_shm_init((struct kanbudb_shm_header*)ptr);
        munmap(ptr, sz);
    }

    void* ptr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
    if (ptr == MAP_FAILED) { close(out->fd); return KANBUDB_ERR_IO; }

    struct kanbudb_shm_header* hdr = (struct kanbudb_shm_header*)ptr;
    if (hdr->magic != KANBUDB_SHM_MAGIC) {
        kanbudb_shm_init(hdr);
    }

    out->mmap_len = sz;
    out->header = hdr;
    return KANBUDB_OK;
}

void kanbudb_shm_close(kanbudb_shm_t* shm) {
    if (!shm || shm->fd < 0) return;
    munmap(shm->header, shm->mmap_len);
    close(shm->fd);
    shm->fd = -1;
    shm->header = NULL;
}

uint64_t kanbudb_shm_reader_register(kanbudb_shm_t* shm) {
    struct kanbudb_shm_header* hdr = shm->header;
    pid_t my_pid = getpid();
    uint64_t my_tid = (uint64_t)(uintptr_t)(void*)pthread_self();

    for (int attempt = 0; attempt < 3; attempt++) {
        for (uint64_t i = 0; i < KANBUDB_MAX_READERS; i++) {
            uint64_t old = __sync_lock_test_and_set(&hdr->readers[i].txn_id, my_tid);
            if (old == 0) {
                hdr->readers[i].pid = my_pid;

                __sync_synchronize();

                uint64_t gen = hdr->sstable_generation;
                __sync_synchronize();

                uint64_t verify = hdr->readers[i].txn_id;
                if (verify != my_tid) {
                    continue;
                }
                return i | (gen << 32);
            }
        }
    }
    return UINT64_MAX;
}

void kanbudb_shm_reader_unregister(kanbudb_shm_t* shm, uint64_t slot) {
    if (slot >= KANBUDB_MAX_READERS) return;
    __sync_synchronize();
    shm->header->readers[slot].txn_id = 0;
    __sync_synchronize();
}

uint64_t kanbudb_shm_oldest_active_reader(kanbudb_shm_t* shm) {
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < KANBUDB_MAX_READERS; i++) {
        uint64_t tid = shm->header->readers[i].txn_id;
        if (tid > 0 && tid < oldest) oldest = tid;
    }
    return oldest;
}

/* ── LOCK ─────────────────────────────────────────────────── */

int kanbudb_lock_open(const char* db_path) {
    char lock_path[1024];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", db_path);
    int fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    return fd;
}

void kanbudb_lock_close(int fd) {
    if (fd >= 0) close(fd);
}

int kanbudb_lock_shared(int fd) {
    if (fd < 0) return KANBUDB_ERR_INVAL;
    if (flock(fd, LOCK_SH) < 0) return KANBUDB_ERR_BUSY;
    return KANBUDB_OK;
}

int kanbudb_lock_exclusive(int fd) {
    if (fd < 0) return KANBUDB_ERR_INVAL;
    if (flock(fd, LOCK_EX) < 0) return KANBUDB_ERR_BUSY;
    return KANBUDB_OK;
}

int kanbudb_unlock(int fd) {
    if (fd < 0) return KANBUDB_ERR_INVAL;
    flock(fd, LOCK_UN);
    return KANBUDB_OK;
}

/* ── MANIFEST ─────────────────────────────────────────────── */

#define MANIFEST_ENTRY_MAX 1024

static int manifest_parse(kanbudb_manifest_t* mf) {
    if (mf->fd < 0) return KANBUDB_ERR_INVAL;

    for (int i = 0; i < mf->num_entries; i++) {
        free(mf->paths[i]);
    }
    mf->num_entries = 0;

    lseek(mf->fd, 0, SEEK_SET);
    uint64_t gen = 0;
    int n;
    if (read(mf->fd, &gen, sizeof(gen)) != (ssize_t)sizeof(gen)) {
        mf->generation = 0;
        return KANBUDB_OK;
    }
    mf->generation = gen;

    if (read(mf->fd, &n, sizeof(n)) != (ssize_t)sizeof(n)) {
        return KANBUDB_OK;
    }
    if (n > MANIFEST_ENTRY_MAX || n < 0) return KANBUDB_ERR_CORRUPT;

    for (int i = 0; i < n && i < MANIFEST_ENTRY_MAX; i++) {
        uint16_t path_len;
        if (read(mf->fd, &path_len, sizeof(path_len)) != (ssize_t)sizeof(path_len))
            break;
        char* p = (char*)malloc((size_t)path_len + 1);
        if (!p) break;
        if (read(mf->fd, p, path_len) != (ssize_t)path_len) { free(p); break; }
        p[path_len] = '\0';

        int lvl = 0;
        uint64_t s = 0;
        if (read(mf->fd, &lvl, sizeof(lvl)) != (ssize_t)sizeof(lvl)) { free(p); break; }
        if (read(mf->fd, &s, sizeof(s)) != (ssize_t)sizeof(s)) { free(p); break; }

        mf->paths[mf->num_entries] = p;
        mf->levels[mf->num_entries] = lvl;
        mf->seqs[mf->num_entries] = s;
        mf->num_entries++;
    }
    return KANBUDB_OK;
}

int kanbudb_manifest_open(const char* db_path, kanbudb_manifest_t* mf) {
    memset(mf, 0, sizeof(*mf));
    char mf_path[1024];
    snprintf(mf_path, sizeof(mf_path), "%s.manifest", db_path);

    mf->path = strdup(mf_path);
    if (!mf->path) return KANBUDB_ERR_OOM;

    mf->fd = open(mf_path, O_RDWR | O_CREAT, 0644);
    if (mf->fd < 0) { free(mf->path); return KANBUDB_ERR_IO; }

    mf->paths = (char**)calloc(MANIFEST_ENTRY_MAX, sizeof(char*));
    mf->levels = (int*)calloc(MANIFEST_ENTRY_MAX, sizeof(int));
    mf->seqs = (uint64_t*)calloc(MANIFEST_ENTRY_MAX, sizeof(uint64_t));
    if (!mf->paths || !mf->levels || !mf->seqs) {
        kanbudb_manifest_close(mf); return KANBUDB_ERR_OOM;
    }

    return manifest_parse(mf);
}

void kanbudb_manifest_close(kanbudb_manifest_t* mf) {
    if (!mf) return;
    if (mf->fd >= 0) close(mf->fd);
    for (int i = 0; i < mf->num_entries; i++) free(mf->paths[i]);
    free(mf->paths);
    free(mf->levels);
    free(mf->seqs);
    free(mf->path);
    memset(mf, 0, sizeof(*mf));
}

int kanbudb_manifest_reload(kanbudb_manifest_t* mf) {
    return manifest_parse(mf);
}

int kanbudb_manifest_add(kanbudb_manifest_t* mf, const char* sst_path, int level, uint64_t seq) {
    if (mf->num_entries >= MANIFEST_ENTRY_MAX) return KANBUDB_ERR_OOM;

    int idx = mf->num_entries;
    mf->paths[idx] = strdup(sst_path);
    if (!mf->paths[idx]) return KANBUDB_ERR_OOM;
    mf->levels[idx] = level;
    mf->seqs[idx] = seq;
    mf->num_entries++;

    mf->generation++;
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", mf->path);

    int tmp_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) return KANBUDB_ERR_IO;

    write(tmp_fd, &mf->generation, sizeof(mf->generation));
    write(tmp_fd, &mf->num_entries, sizeof(mf->num_entries));
    for (int i = 0; i < mf->num_entries; i++) {
        uint16_t plen = (uint16_t)strlen(mf->paths[i]);
        write(tmp_fd, &plen, sizeof(plen));
        write(tmp_fd, mf->paths[i], plen);
        write(tmp_fd, &mf->levels[i], sizeof(mf->levels[i]));
        write(tmp_fd, &mf->seqs[i], sizeof(mf->seqs[i]));
    }
    fsync(tmp_fd);
    close(tmp_fd);

    if (rename(tmp_path, mf->path) < 0) return KANBUDB_ERR_IO;

    mf->fd = open(mf->path, O_RDWR, 0644);
    if (mf->fd < 0) return KANBUDB_ERR_IO;

    return KANBUDB_OK;
}

int kanbudb_manifest_remove(kanbudb_manifest_t* mf, const char* sst_path) {
    int found = -1;
    for (int i = 0; i < mf->num_entries; i++) {
        if (strcmp(mf->paths[i], sst_path) == 0) { found = i; break; }
    }
    if (found < 0) return KANBUDB_ERR_NOTFOUND;

    free(mf->paths[found]);
    for (int i = found; i < mf->num_entries - 1; i++) {
        mf->paths[i] = mf->paths[i + 1];
        mf->levels[i] = mf->levels[i + 1];
        mf->seqs[i] = mf->seqs[i + 1];
    }
    mf->num_entries--;
    mf->generation++;

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", mf->path);

    int tmp_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) return KANBUDB_ERR_IO;

    write(tmp_fd, &mf->generation, sizeof(mf->generation));
    write(tmp_fd, &mf->num_entries, sizeof(mf->num_entries));
    for (int i = 0; i < mf->num_entries; i++) {
        uint16_t plen = (uint16_t)strlen(mf->paths[i]);
        write(tmp_fd, &plen, sizeof(plen));
        write(tmp_fd, mf->paths[i], plen);
        write(tmp_fd, &mf->levels[i], sizeof(mf->levels[i]));
        write(tmp_fd, &mf->seqs[i], sizeof(mf->seqs[i]));
    }
    fsync(tmp_fd);
    close(tmp_fd);

    if (rename(tmp_path, mf->path) < 0) return KANBUDB_ERR_IO;

    mf->fd = open(mf->path, O_RDWR, 0644);
    if (mf->fd < 0) return KANBUDB_ERR_IO;

    return KANBUDB_OK;
}
