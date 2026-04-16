// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *t_str = (type == OBJ_BLOB) ? "blob" : (type == OBJ_TREE ? "tree" : "commit");
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", t_str, len);
    
    size_t full_len = hlen + 1 + len;
    uint8_t *buf = malloc(full_len);
    if (!buf) return -1;
    
    memcpy(buf, header, hlen + 1);
    memcpy(buf + hlen + 1, data, len);
    
    compute_hash(buf, full_len, id_out);
    
    if (object_exists(id_out)) {
        free(buf);
        return 0;
    }
    
    char path[512];
    object_path(id_out, path, sizeof(path));
    
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    
    mkdir(dir, 0755);
    
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    
    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(buf); return -1; }
    
    if (write(fd, buf, full_len) != (ssize_t)full_len) {
        close(fd); free(buf); return -1;
    }
    
    fsync(fd);
    close(fd);
    
    if (rename(tmp, path) != 0) { free(buf); return -1; }
    
    int dir_fd = open(dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    
    free(buf);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));
    
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    
    uint8_t *buf = malloc(size);
    if (!buf) { fclose(f); return -1; }
    
    if (fread(buf, 1, size, f) != (size_t)size) { free(buf); fclose(f); return -1; }
    fclose(f);
    
    ObjectID comp;
    compute_hash(buf, size, &comp);
    if (memcmp(id->hash, comp.hash, HASH_SIZE) != 0) { free(buf); return -1; }
    
    uint8_t *null_b = memchr(buf, '\0', size);
    if (!null_b) { free(buf); return -1; }
    
    char t_str[32];
    size_t dlen;
    if (sscanf((char *)buf, "%31s %zu", t_str, &dlen) != 2) { free(buf); return -1; }
    
    if (strcmp(t_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(t_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(t_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }
    
    *data_out = malloc(dlen);
    if (!*data_out) { free(buf); return -1; }
    
    *len_out = dlen;
    memcpy(*data_out, null_b + 1, dlen);
    free(buf);
    
    return 0;
}
