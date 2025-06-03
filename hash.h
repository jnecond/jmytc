#ifndef HASH_H 
#define HASH_H 1


U64 Hash64(const void* _data_, U64 data_size);



typedef struct Hash_set_entry_ {
	U64	hash;
	void* data;
	U32	data_size;
	B		free_data_on_remove;
} Hash_set_entry_;

typedef struct Hash_set_ {
	Hash_set_entry_* entries;
	U64 num_used;
	U64 num_allocd;
} Hash_set_;

void Hash_set_free(Hash_set_* hs);
Hash_set_* Hash_set_create(U64 initial_capacity);
Hash_set_entry_* Hash_set_find(Hash_set_* hs, void* data, U32 data_size);
Hash_set_entry_* Hash_set_add(Hash_set_* hs, void* data, U32 data_size, B make_copy);
void Hash_set_remove_by_entry(Hash_set_* hs, Hash_set_entry_* e);
void Hash_set_remove_by_data(Hash_set_* hs, void* data, U32 data_size);
void Hash_set_remove_by_index(Hash_set_* hs, U64 index);



typedef struct Dictionary_entry_ {
	U64	hash;
	void* key;
	void* value;
	U32	key_size;
	U32	value_size; // 8 ---
	U32	times_added;
	B		key_needs_free;
	B		value_needs_free;
} Dictionary_entry_;

typedef struct Dictionary_ {
	Dictionary_entry_* entries;
	U64 num_used;
	U64 num_allocd;
} Dictionary_;

B Dictionary_save(Dictionary_* dic, char* path);
B Dictionary_save_compressed(Dictionary_* dic, char* path);
Dictionary_* Dictionary_load_compressed(char* path);
Dictionary_* Dictionary_load(char* path);
void Dictionary_free(Dictionary_* dic);
Dictionary_* Dictionary_create(U64 initial_capacity);
Dictionary_entry_* Dictionary_find(Dictionary_* dic, void* key, U32 key_size);
Dictionary_entry_* Dictionary_add(Dictionary_* dic, void* key, U32 key_size, B copy_key, void* value, U32 value_size, B copy_value);


#endif
