/* Plain C interface to the IndexCraft engine, used by the FastAPI backend through ctypes.
 *
 * Conventions
 *  - Every function that produces data writes a malloc'd, NUL-terminated JSON string to
 *    *out_json. The caller must release it with ic_free().
 *  - Return value: 0 on success, otherwise a positive error code (enum indexcraft::Code).
 *    On error *out_json holds {"ok":false,"error":{"code":"...","message":"..."}}.
 *  - A handle is NOT thread-safe; callers must serialise access (the backend uses a lock).
 */
#ifndef INDEXCRAFT_C_API_H
#define INDEXCRAFT_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ic_handle ic_handle;

int ic_open(const char* path, size_t max_keys, ic_handle** out_handle, char** out_json);
void ic_close(ic_handle* h);
void ic_free(char* s);

/* force: 0 = automatic, 1 = full scan, 2 = index. max_rows 0 = unlimited. */
int ic_execute(ic_handle* h, const char* sql, int force, size_t max_rows, char** out_json);
int ic_compare(ic_handle* h, const char* sql, size_t max_rows, char** out_json);

int ic_insert(ic_handle* h, int64_t key, const char* payload, size_t payload_len, char** out_json);
int ic_delete(ic_handle* h, int64_t key, char** out_json);
int ic_get(ic_handle* h, int64_t key, char** out_json);
int ic_list(ic_handle* h, size_t offset, size_t limit, char** out_json);

int ic_stats(ic_handle* h, char** out_json);
int ic_tree(ic_handle* h, size_t max_nodes, char** out_json);
int ic_search_path(ic_handle* h, int64_t key, char** out_json);
int ic_validate(ic_handle* h, char** out_json);
int ic_check_consistency(ic_handle* h, char** out_json);
int ic_rebuild_index(ic_handle* h, char** out_json);
int ic_build_info(char** out_json);

#ifdef __cplusplus
}
#endif
#endif
