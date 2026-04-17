// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Recursive helper: builds a tree object for a given "prefix" (subdirectory level).
// entries[] are ALL index entries. count is the total number.
// prefix is the directory path prefix we are currently building (e.g., "" for root, "src/" for src/).
// id_out receives the hash of the written tree object.
static int write_tree_level(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;

        // Only process entries under our current prefix
        size_t prefix_len = strlen(prefix);
        if (strncmp(path, prefix, prefix_len) != 0) {
            i++;
            continue;
        }

        // The relative name after stripping the prefix
        const char *rel = path + prefix_len;

        // Check if this entry is in a subdirectory (contains a '/')
        char *slash = strchr(rel, '/');

        if (slash == NULL) {
            // It's a file directly in this level
            TreeEntry *te = &tree.entries[tree.count];
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            tree.count++;
            i++;
        } else {
            // It's inside a subdirectory — get the subdirectory name
            size_t dir_name_len = slash - rel;
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) { i++; continue; }
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build new prefix for the subdirectory: prefix + dir_name + "/"
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);

            // Recursively build the subtree for this subdirectory
            ObjectID sub_tree_id;
            if (write_tree_level(entries, count, sub_prefix, &sub_tree_id) != 0)
                return -1;

            // Add the subtree entry (only once per unique subdir name at this level)
            // Check if we already added this directory
            int already_added = 0;
            for (int k = 0; k < tree.count; k++) {
                if (strcmp(tree.entries[k].name, dir_name) == 0) {
                    already_added = 1;
                    break;
                }
            }
            if (!already_added) {
                TreeEntry *te = &tree.entries[tree.count];
                strncpy(te->name, dir_name, sizeof(te->name) - 1);
                te->name[sizeof(te->name) - 1] = '\0';
                te->mode = MODE_DIR;
                te->hash = sub_tree_id;
                tree.count++;
            }

            // Skip all entries belonging to this subdirectory
            while (i < count && strncmp(entries[i].path, sub_prefix, strlen(sub_prefix)) == 0)
                i++;
        }
    }

    // Serialize this tree and write it to the object store
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree objects.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;

    if (index.count == 0) {
        // Empty tree — write an empty tree object
        Tree empty;
        empty.count = 0;
        void *tree_data;
        size_t tree_len;
        if (tree_serialize(&empty, &tree_data, &tree_len) != 0) return -1;
        int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
        free(tree_data);
        return rc;
    }

    return write_tree_level(index.entries, index.count, "", id_out);
}

