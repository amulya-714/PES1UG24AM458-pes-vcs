// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── Comparator for qsort ────────────────────────────────────────────────────

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index into memory.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // File doesn't exist yet — that's fine, empty index
        return 0;
    }

    char hex[HASH_HEX_SIZE + 2]; // +2 for safety
    uint32_t mode;
    uint64_t mtime;
    uint64_t size;
    char path[512];

    while (index->count < MAX_INDEX_ENTRIES) {
        // Format: <mode-octal> <hex-hash> <mtime> <size> <path>
        int result = fscanf(f, "%o %64s %llu %llu %511s\n",
                            &mode, hex,
                            (unsigned long long *)&mtime,
                            (unsigned long long *)&size,
                            path);
        if (result == EOF || result < 5) break;

        IndexEntry *e = &index->entries[index->count];
        e->mode     = mode;
        e->mtime_sec = mtime;
        e->size     = size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        if (hex_to_hash(hex, &e->hash) != 0) continue; // skip corrupt line

        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically (temp file + rename).
int index_save(const Index *index) {
    qsort((void*)index->entries, index->count, sizeof(IndexEntry), compare_entries);

    char tmp_path[] = ".pes/index.tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = &index->entries[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);
        
        fprintf(f, "%o %s %llu %llu %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                (unsigned long long)e->size,
                e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, ".pes/index") != 0) return -1;
    return 0;
}

// Stage a file: read it, store as blob, update the index entry.
int index_add(Index *index, const char *path) {
    // 1. Read the file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) { fclose(f); return -1; }

    void *buf = malloc((size_t)file_size + 1);
    if (!buf) { fclose(f); return -1; }

    size_t read_bytes = fread(buf, 1, (size_t)file_size, f);
    fclose(f);

    if (read_bytes != (size_t)file_size) { free(buf); return -1; }

    // 2. Write the file content to the object store as a blob
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, buf, read_bytes, &blob_id) != 0) {
        free(buf); return -1;
    }
    free(buf);

    // 3. Get file metadata (mtime, size, mode)
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode;
    if (st.st_mode & S_IXUSR) mode = 0100755;
    else                       mode = 0100644;

    // 4. Update or insert the index entry
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        // Update existing entry
        existing->hash      = blob_id;
        existing->mode      = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint64_t)st.st_size;
    } else {
        // Add new entry
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        IndexEntry *e = &index->entries[index->count];
        e->hash      = blob_id;
        e->mode      = mode;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint64_t)st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        index->count++;
    }

    // 5. Save the updated index atomically
    return index_save(index);
}

