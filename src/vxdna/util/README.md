# Util Library

Shared utility implementations for the vaccel-renderer project.

## Contents

### Hash Tables

Three specialized hash table implementations for different key types:

#### hash_table_u32 - 32-bit Integer Keys
- **Files**: `hash_table_u32.c`
- **Key Type**: `uint32_t`
- **Hash Algorithm**: Multiplicative hash
- **Used By**: Resource table, Context table
- **API**:
  ```c
  struct hash_table_u32 *hash_table_u32_create(void (*destroy_func)(void *));
  void hash_table_u32_destroy(struct hash_table_u32 *ht);
  int hash_table_u32_insert(struct hash_table_u32 *ht, uint32_t key, void *value);
  void *hash_table_u32_search(struct hash_table_u32 *ht, uint32_t key);
  int hash_table_u32_remove(struct hash_table_u32 *ht, uint32_t key);
  ```

#### hash_table_u64 - 64-bit Integer Keys
- **Files**: `hash_table_u64.c`
- **Key Type**: `uint64_t`
- **Hash Algorithm**: FNV-1a hash
- **Used By**: Fence table (timeline synchronization)
- **API**:
  ```c
  struct hash_table_u64 *hash_table_u64_create(void (*destroy_func)(void *));
  void hash_table_u64_destroy(struct hash_table_u64 *ht);
  int hash_table_u64_insert(struct hash_table_u64 *ht, uint64_t key, void *value);
  void *hash_table_u64_search(struct hash_table_u64 *ht, uint64_t key);
  int hash_table_u64_remove(struct hash_table_u64 *ht, uint64_t key);
  void hash_table_u64_foreach(struct hash_table_u64 *ht,
                               void (*callback)(uint64_t, void *, void *),
                               void *user_data);
  ```

#### hash_table_ptr - Pointer Keys
- **Files**: `hash_table_ptr.c`
- **Key Type**: `void *`
- **Hash Algorithm**: FNV-1a hash
- **Used By**: Device management (global cookie lookup)
- **API**:
  ```c
  struct hash_table_ptr *hash_table_ptr_create(void (*destroy_func)(void *));
  void hash_table_ptr_destroy(struct hash_table_ptr *ht);
  int hash_table_ptr_insert(struct hash_table_ptr *ht, void *key, void *value);
  void *hash_table_ptr_search(struct hash_table_ptr *ht, void *key);
  int hash_table_ptr_remove(struct hash_table_ptr *ht, void *key);
  ```

### OS File Utilities

#### os_dupfd_cloexec
- **Files**: `os_file.c`, `os_file.h`
- **Purpose**: Duplicate file descriptor with close-on-exec flag
- **Used By**: Fence table, DRM backend
- **API**:
  ```c
  int os_dupfd_cloexec(int fd);
  ```

## Design Principles

### 1. Type Safety
Each hash table type is distinct to prevent mixing key types accidentally.

### 2. Performance
- **Initial Size**: 32 buckets (configurable via `HASH_TABLE_INITIAL_SIZE`)
- **Load Factor**: 0.75 max (configurable via `HASH_TABLE_MAX_LOAD_FACTOR`)
- **Collision Handling**: Separate chaining with linked lists
- **Dynamic Growth**: Automatically doubles size when load factor exceeded

### 3. Memory Management
- Automatic value cleanup via destroy callbacks
- Proper cleanup of internal structures
- No memory leaks on destroy

### 4. Error Handling
Standard errno-style error codes:
- `0` - Success
- `-ENOMEM` - Out of memory
- `-EINVAL` - Invalid arguments
- `-ENOENT` - Key not found

## Usage Example

```c
#include "util/hash_table.h"

/* Define destructor for values */
static void my_destructor(void *value) {
    free(value);
}

/* Create hash table */
struct hash_table_u32 *ht = hash_table_u32_create(my_destructor);

/* Insert entries */
char *value1 = strdup("hello");
hash_table_u32_insert(ht, 1, value1);

/* Lookup entries */
char *found = hash_table_u32_search(ht, 1);
printf("Found: %s\n", found);  // "hello"

/* Remove entries */
hash_table_u32_remove(ht, 1);  // Calls my_destructor(value1)

/* Cleanup */
hash_table_u64_destroy(ht);  // Calls destructor for remaining values
```

## Thread Safety

Hash tables are **NOT** thread-safe by themselves. Users must provide external synchronization:

```c
mtx_t lock;
mtx_init(&lock, mtx_plain);

/* All operations must be protected */
mtx_lock(&lock);
void *value = hash_table_u32_search(ht, key);
mtx_unlock(&lock);
```

This design gives users control over the granularity of locking.

## Testing

To test hash table implementations:

```c
#include "util/hash_table.h"
#include <assert.h>

void test_basic_operations(void) {
    struct hash_table_u32 *ht = hash_table_u32_create(NULL);
    assert(ht != NULL);

    /* Test insert */
    int value = 42;
    assert(hash_table_u32_insert(ht, 1, &value) == 0);

    /* Test search */
    int *found = hash_table_u32_search(ht, 1);
    assert(found != NULL && *found == 42);

    /* Test remove */
    assert(hash_table_u32_remove(ht, 1) == 0);
    assert(hash_table_u32_search(ht, 1) == NULL);

    hash_table_u32_destroy(ht);
}
```

## Performance Considerations

### When to Use

✅ **Good for:**
- Small to medium datasets (< 1M entries)
- Mixed read/write workloads
- When iterator support is needed (u64)

❌ **Not optimal for:**
- Very large datasets (consider B-trees, skip lists)
- Write-heavy workloads (consider lock-free structures)
- Ordered traversal (consider sorted structures)

### Optimization Tips

1. **Pre-size if possible**: If you know the expected size, modify `HASH_TABLE_INITIAL_SIZE`
2. **Batch operations**: Hold locks across multiple operations
3. **Monitor load factor**: High load factor (> 0.9) indicates need for resize
4. **Profile hash function**: If many collisions, consider different hash

## Future Enhancements

Potential improvements:

1. **Open addressing**: Reduce memory overhead
2. **Robin Hood hashing**: Better cache locality
3. **SIMD hash functions**: Faster hashing for large keys
4. **Statistics API**: Monitor performance (collisions, load, etc.)
5. **Custom allocators**: Memory pool support
6. **Concurrent hash table**: Lock-free or fine-grained locking

## License

Same as main project (MIT).

