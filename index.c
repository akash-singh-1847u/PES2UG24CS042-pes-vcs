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
// Implemented index_load to read entries from .pes/index
// Implemented index_save with sorting and safe file write using temp file
// Implemented index_add to add files into staging area
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "pes.h"
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
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
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
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
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
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
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
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
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

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // It's fine if it doesn't exist
    
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        IndexEntry *e = &index->entries[index->count];
        char hex[128];
        if (sscanf(line, "%o %64s %lu %u %511[^\n]", &e->mode, hex, &e->mtime_sec, &e->size, e->path) == 5) {
            if (hex_to_hash(hex, &e->hash) == 0) {
                index->count++;
            }
        }
    }
    fclose(f);
    return 0;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    if (index->count < 0 || index->count > MAX_INDEX_ENTRIES) return -1;
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    if (sorted->count > 1) {
    	qsort(sorted->entries, sorted->count,
          sizeof(IndexEntry), compare_index_entries);
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp, "w");
    if (!f) {
	    free(sorted);
	    return -1;
    }
    for (int i = 0; i < sorted->count; i++) {
        char hex[128];
        hash_to_hex(&sorted->entries[i].hash, hex);
        fprintf(f, "%o %s %lu %u %s\n", sorted->entries[i].mode, hex, sorted->entries[i].mtime_sec, sorted->entries[i].size, sorted->entries[i].path);
    }
    
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    if (rename(tmp, INDEX_FILE) != 0) { free(sorted); return -1; }
    free(sorted);
    return 0;
}

int index_add(Index *index, const char *path) {
    // 1. Load index safely
    if (index_load(index) != 0) return -1;

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    // 2. Read file
    uint8_t *buf = malloc(st.st_size);
    if (!buf && st.st_size > 0) {
        fclose(f);
        return -1;
    }

    if (st.st_size > 0 &&
        fread(buf, 1, st.st_size, f) != (size_t)st.st_size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 3. Write blob
    ObjectID id;
    if (object_write(OBJ_BLOB, buf, st.st_size, &id) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    // 4. Find or create entry
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    // 5. Fill entry
    e->mode = 100644;
    e->hash = id;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;

    // 6. Save index
    return index_save(index);
}
