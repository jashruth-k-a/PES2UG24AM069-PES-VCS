// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
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

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
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

// Print the status of the working directory.
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
            if (strcmp(ent->d_name, ".")    == 0) continue;
            if (strcmp(ent->d_name, "..")   == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes")  == 0) continue;
            if (strstr(ent->d_name, ".o")  != NULL) continue;

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

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index.
// If the file does not exist, initializes an empty index (not an error).
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0; // no index file yet = empty index, that's fine

    char hash_hex[65];
    unsigned int mode;
    long mtime;
    unsigned int size;  // uint32_t matches %u
    char path[512];

    // Parse each line: <mode-octal> <hash-hex> <mtime> <size> <path>
    while (fscanf(f, "%o %64s %ld %u %511s", &mode, hash_hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *e = &index->entries[index->count++];
        e->mode      = mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint32_t)size;
        strcpy(e->path, path);
        hex_to_hash(hash_hex, &e->hash);
    }

    fclose(f);
    return 0;
}

// Helper for qsort — sort index entries alphabetically by path.
static int compare_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically.
//
// THE KEY FIX: We must NOT do `Index sorted = *index` on the stack.
// Index is ~5.4 MB. This function is called from index_add, which is called
// from cmd_add — all of which already have Index locals on the stack.
// Stacking another 5.4 MB copy here overflows the 8 MB stack limit.
// Solution: heap-allocate the sorted copy with malloc.
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // Heap-allocate the sorted copy to avoid stack overflow
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;

    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_entries);

    FILE *f = fopen(".pes/index.tmp", "w");
    if (!f) {
        free(sorted);
        return -1;
    }

    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hex);

        // Use %u for size (uint32_t) and %ld for mtime (stored as uint64_t
        // but fits in long for any reasonable timestamp).
        fprintf(f, "%o %s %ld %u %s\n",
                sorted->entries[i].mode,
                hex,
                (long)sorted->entries[i].mtime_sec,
                sorted->entries[i].size,
                sorted->entries[i].path);
    }

    free(sorted);

    // Flush userspace buffers, sync to disk, then atomically replace the index
    if (fflush(f) != 0)          { fclose(f); return -1; }
    if (fsync(fileno(f)) != 0)   { fclose(f); return -1; }
    fclose(f);

    if (rename(".pes/index.tmp", ".pes/index") != 0) return -1;
    return 0;
}

// Stage a file for the next commit.
//
// Reads the file, writes it as a blob object, then updates (or creates)
// the index entry for this path and saves the index.
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // Sync with whatever is already on disk
    if (index_load(index) != 0) return -1;

    // Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: failed to add '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    void *data = NULL;
    if (file_size > 0) {
        data = malloc(file_size);
        if (!data) { fclose(f); return -1; }
        if (fread(data, 1, file_size, f) != (size_t)file_size) {
            free(data);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    // Write blob to the object store
    ObjectID id;
    if (object_write(OBJ_BLOB, data, (size_t)file_size, &id) != 0) {
        free(data);
        return -1;
    }
    free(data);  // done with file contents

    // Get metadata for fast-diff (mtime + size)
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    // Update existing entry or append a new one
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
    }

    e->mode      = (uint32_t)st.st_mode;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size      = (uint32_t)st.st_size;
    e->hash      = id;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';

    return index_save(index);
}