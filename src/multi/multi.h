#ifndef KANBUDB_MULTI_H
#define KANBUDB_MULTI_H

#include "macros.h"
#include <sys/types.h>
#include <stdint.h>

#define KANBUDB_MAX_READERS 128
#define KANBUDB_SHM_MAGIC   0x4B4D4C4D4D5058ULL /* "KMLMMPX" */

#pragma pack(push, 1)

struct kanbudb_reader_slot {
    pid_t  pid;
    uint64_t txn_id;
};

struct kanbudb_shm_header {
    uint64_t magic;
    uint64_t sstable_generation;
    uint64_t commit_seq;
    uint64_t wal_committed_end;
    uint64_t wal_checkpointed;
    struct kanbudb_reader_slot readers[KANBUDB_MAX_READERS];
};

#pragma pack(pop)

typedef struct {
    int fd;
    size_t mmap_len;
    struct kanbudb_shm_header* header;
} kanbudb_shm_t;

int  kanbudb_shm_open(const char* db_path, kanbudb_shm_t* out);
void kanbudb_shm_close(kanbudb_shm_t* shm);
void kanbudb_shm_init(struct kanbudb_shm_header* hdr);

uint64_t kanbudb_shm_reader_register(kanbudb_shm_t* shm);
void     kanbudb_shm_reader_unregister(kanbudb_shm_t* shm, uint64_t slot);
uint64_t kanbudb_shm_oldest_active_reader(kanbudb_shm_t* shm);

int  kanbudb_lock_open(const char* db_path);
void kanbudb_lock_close(int fd);
int  kanbudb_lock_shared(int fd);
int  kanbudb_lock_exclusive(int fd);
int  kanbudb_unlock(int fd);

typedef struct {
    int   fd;
    char* path;
    int   num_entries;
    char** paths;
    int*   levels;
    uint64_t* seqs;
    uint64_t generation;
} kanbudb_manifest_t;

int  kanbudb_manifest_open(const char* db_path, kanbudb_manifest_t* mf);
void kanbudb_manifest_close(kanbudb_manifest_t* mf);
int  kanbudb_manifest_reload(kanbudb_manifest_t* mf);
int  kanbudb_manifest_add(kanbudb_manifest_t* mf, const char* sst_path, int level, uint64_t seq);
int  kanbudb_manifest_remove(kanbudb_manifest_t* mf, const char* sst_path);

#endif
