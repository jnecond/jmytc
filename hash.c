#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "essentials.h"
#include "hash.h"
#include "lzw.h"


always_il U64 load_U64(const void* x){
	U64 y;
	memcpy(&y, x, sizeof(y));
	return y;
}

always_il U32 load_U32(const void* x){
	U32 y;
	memcpy(&y, x, sizeof(y));
	return y;
}

always_il U16 load_U16(const void* x){
	U16 y;
	memcpy(&y, x, sizeof(y));
	return y;
}

always_il U64 u64rotl(U64 val, S32 num){
	return (val << num) | (val >> (64 - num));
}


U64 Hash64(const void* _data_, U64 data_size){
	if (!_data_ || !data_size) abort();
	const U8* data8 = _data_;
	U64 hash = UL(0xe3789eb1a9c372c1);
	U64 d = 0;
	U64 val;
	while(d+8 < data_size){
		val = load_U64(data8+d);
		if (!val){
			hash ^= u64rotl(hash, 7)*UL(28657);
		}else{
			hash ^= u64rotl(UL(99194853094755497)*val, 14)*UL(514229);
		}
		d += 8;
	}
	if (d+4 < data_size){
		val = UL(1)+load_U32(data8+d);
		hash ^= u64rotl(UL(99194853094755497)*val, 21)*UL(9369319);
		d += 4;
	}
	if (d+2 < data_size){
		val = UL(1)+load_U16(data8+d);
		hash ^= u64rotl(UL(99194853094755497)*val, 28)*UL(433494437);
		d += 2;
	}
	if (d < data_size){
		val = UL(1)+data8[d];
		hash ^= u64rotl(UL(99194853094755497)*val, 35)*UL(2971215073);
	}
	return hash;
}



void Hash_set_free(Hash_set_* hs){
	if (!hs) return;
	if (hs->entries){
		for (U64 i = 0; i < hs->num_allocd; i++){
			if (hs->entries[i].data && hs->entries[i].free_data_on_remove){
				free(hs->entries[i].data);
			}
		}
		free(hs->entries);
	}
	free(hs);
}

Hash_set_* Hash_set_create(U64 initial_capacity){
	initial_capacity &= UL(0xFFFFFFFFFFFFFFF);
	if (initial_capacity < 32){
		initial_capacity = 32;
	}else if (!Is_pow2(initial_capacity)){
		S32 bits = Num_req_bits(initial_capacity);
		initial_capacity = UL(1) << bits;
	}
	Hash_set_* set = Malloc(sizeof(Hash_set_));
	set->num_allocd = initial_capacity;
	set->entries = Calloc(set->num_allocd, sizeof(Hash_set_entry_));
	set->num_used = 0;
	return set;
}

Hash_set_entry_* Hash_set_find(Hash_set_* hs, void* data, U32 data_size){
	if (!hs || !data || !data_size){
		return 0;
	}
	U64 hash = Hash64(data, data_size);
	U64 ind = hash & (hs->num_allocd-1);
	while (hs->entries[ind].data){
		if (hs->entries[ind].hash == hash 
		&& hs->entries[ind].data_size == data_size
		&& !memcmp(hs->entries[ind].data, data, data_size)){
			return &hs->entries[ind];
		}
		ind++;
		if (ind >= hs->num_allocd){
			ind = 0;
		}
	}
	return 0;
}

Hash_set_entry_* Hash_set_add(Hash_set_* hs, void* data, U32 data_size, B make_copy){
	if (!hs || !data || !data_size){
		return 0;
	}
	if (hs->num_used >= (hs->num_allocd*3)>>2){
		U64 newsize = hs->num_allocd*2;
		Hash_set_entry_* new_entries = Calloc(newsize, sizeof(Hash_set_entry_)); 
		newsize--;
		U64 new_ind;
		for (U64 i = 0; i < hs->num_allocd; i++){
			if (hs->entries[i].data){
				new_ind = (hs->entries[i].hash & newsize);
				while (new_entries[new_ind].data){
					new_ind++;
					if (new_ind >= newsize){
						new_ind = 0;
					}
				}
				memcpy(&new_entries[new_ind], &hs->entries[i], sizeof(Hash_set_entry_));
			}
		}
		free(hs->entries);
		hs->entries = new_entries;
		hs->num_allocd = newsize+1;
	}
	U64 hash = Hash64(data, data_size);
	U64 ind = hash & (hs->num_allocd-1);
	while (hs->entries[ind].data){
		if (hs->entries[ind].hash == hash 
		&& hs->entries[ind].data_size == data_size
		&& !memcmp(hs->entries[ind].data, data, data_size)){
			//hs->entries[ind].times_added++;
			return &hs->entries[ind];
		}
		ind++;
		if (ind >= hs->num_allocd){
			ind = 0;
		}
	}
	hs->entries[ind].hash = hash;
	hs->entries[ind].data_size = data_size;
	//hs->entries[ind].times_added = 1;
	if (make_copy){
		hs->entries[ind].data = Malloc(data_size);
		memcpy(hs->entries[ind].data, data, data_size);
		hs->entries[ind].free_data_on_remove = 1;
	}else{
		hs->entries[ind].data = data;
		hs->entries[ind].free_data_on_remove = 0;
	}	
	hs->num_used++;
	return &hs->entries[ind];
}

void Hash_set_remove_by_entry(Hash_set_* hs, Hash_set_entry_* e){
	if (!hs) abort();
	if (e && e->data){
		if (e < hs->entries || e >= (hs->entries+(hs->num_allocd*sizeof(Hash_set_entry_)))){
			abort();
		}
		if (e->free_data_on_remove){
			free(e->data);
		}
		memset(e, 0, sizeof(Hash_set_entry_));
		hs->num_used--;
	}
}

void Hash_set_remove_by_data(Hash_set_* hs, void* data, U32 data_size){
	if (!hs) abort();
	Hash_set_entry_* e = Hash_set_find(hs, data, data_size);
	Hash_set_remove_by_entry(hs, e);
}

void Hash_set_remove_by_index(Hash_set_* hs, U64 index){
	if (!hs || index >= hs->num_allocd){
		abort();
	}
	Hash_set_entry_* e = &hs->entries[index];
	Hash_set_remove_by_entry(hs, e);
}





B Dictionary_save(Dictionary_* dic, char* path){
	if (!dic || !path){
		abort();
	}
	if (access(path, W_OK)){
		perror("access");
		return 0;
	}
	FILE* fp = Fopen(path, "w");
	Dictionary_entry_* es = dic->entries;
	Fwrite(&dic->num_allocd,		1, sizeof(dic->num_allocd),	fp);
	Fwrite(&dic->num_used,		1, sizeof(dic->num_used),		fp);
	U64 count = 0;
	for (U64 e = 0; e < dic->num_allocd; e++){
		if (es[e].key && es[e].key_size){
			Fwrite(&es[e].hash,			1, sizeof(es[e].hash),			fp);
			Fwrite(&es[e].key_size,		1, sizeof(es[e].key_size),		fp);
			Fwrite(es[e].key,				1, es[e].key_size,				fp);
			Fwrite(&es[e].value_size,	1, sizeof(es[e].value_size),	fp);
			if (es[e].value_size){
				Fwrite(es[e].value, 1, es[e].value_size, fp);
			}
			count++;
		}
	}
	Fclose(fp);
	return 1;
}


B Dictionary_save_compressed(Dictionary_* dic, char* path){
	if (!dic || !path){
		abort();
	}
	FILE* fp = Fopen(path, "w");
	U64 bufsize = 1u << 24;
	U64 bufpos = 0;
	Dictionary_entry_* es = dic->entries;
	void* buf = Malloc(bufsize);
	#define checkbufsize(dd) if(bufpos+sizeof(dd) >= bufsize){ bufsize *= 2; buf = Realloc(buf, bufsize); }
	#define mcpy(d) checkbufsize(d); memcpy(buf+bufpos, &d, sizeof(d)); bufpos += sizeof(d);
	mcpy(dic->num_allocd);
	mcpy(dic->num_used);
	for (U64 e = 0; e < dic->num_allocd; e++){
		if (es[e].key && es[e].key_size && es[e].value && es[e].value_size){
			mcpy(es[e].hash);
			mcpy(es[e].key_size);
			memcpy(buf+bufpos, es[e].key, es[e].key_size);
			bufpos += es[e].key_size;
			mcpy(es[e].value_size);
			if (es[e].value_size){
				memcpy(buf+bufpos, es[e].value, es[e].value_size);
				bufpos += es[e].value_size;
			}
		}
	}
	LZW_result_ lzw = LZW_encode(buf, bufpos);
	if (!lzw.buf || !lzw.len){
		free(buf);
		return 0;
	}
	Fwrite(&bufpos, sizeof(bufpos), 1, fp); //original size
	Fwrite(&lzw.len, sizeof(lzw.len), 1, fp); // lzw len
	Fwrite(lzw.buf, lzw.len, 1, fp);
	Fclose(fp);
	free(lzw.buf);
	free(buf);
	return 1;
	#undef mcpy
	#undef checkbufsize
}


Dictionary_* Dictionary_load_compressed(char* path){
	void* buf = 0;
	U64 filesize = 0;
	void* filebuf = QFread(path, &filesize);
	if (!filebuf || filesize < 16) return 0;
	
	U64 bufsize = 0;
	U64 lzw_len = 0;
	memcpy(&bufsize, filebuf,	8); //original size
	memcpy(&lzw_len, filebuf+8, 8);
	
	buf = LZW_decode(filebuf+16, lzw_len, bufsize);
	free(filebuf);
	filebuf = 0;
	if (!buf){
		printf("Dictionary_load_compressed: LZW_decode failed\n");
		goto _error;
	}
	
	U64 bufpos = 0;
	Dictionary_* dic = Calloc(1, sizeof(Dictionary_));
	#define checkbufsize(dd) if(bufpos+sizeof(dd) > bufsize) goto _error;
	#define mcpy(d) checkbufsize(d); memcpy(&d, buf+bufpos, sizeof(d)); bufpos += sizeof(d);
	mcpy(dic->num_allocd);
	mcpy(dic->num_used);
	dic->entries = Calloc(dic->num_allocd, sizeof(Dictionary_entry_));
	Dictionary_entry_* es = dic->entries;
	U64 hash, e;
	U64 mask = dic->num_allocd-1;
	U64 num = 0;
	while (bufpos < bufsize){
		if (num >= dic->num_used){
			printf("num >= dic->num_used)\n("PLU" >= "PLU")\n", num, dic->num_used);
			abort();
		}
		mcpy(hash);
		e = hash & mask;
		U64 count = 0;
		while (dic->entries[e].key){
			e++;
			if (e >= dic->num_allocd){
				e = 0;
			}
			if (++count > dic->num_allocd){
				printf("ind++ "PLU", dic= "PLU"/"PLU"\n", count, dic->num_used, dic->num_allocd);
				abort();
			}
		}
		es[e].hash = hash;
		mcpy(es[e].key_size);
		es[e].key = Malloc(es[e].key_size);
		if (bufpos+es[e].key_size >= bufsize) goto _error;
		memcpy(es[e].key, buf+bufpos, es[e].key_size);
		bufpos += es[e].key_size;
		mcpy(es[e].value_size);
		if (es[e].value_size){
			es[e].value = Malloc(es[e].value_size);
			if (bufpos+es[e].value_size > bufsize) goto _error;
			memcpy(es[e].value, buf+bufpos, es[e].value_size);
			bufpos += es[e].value_size;
		}
		es[e].key_needs_free = 1;
		es[e].value_needs_free = 1;
		num++;
	}
	free(buf);
	return dic;
	_error:
	free(buf);
	abort();
	#undef mcpy
	#undef checkbufsize
}


Dictionary_* Dictionary_load(char* path){
	if (!path){
		abort();
	}
	FILE* fp = fopen(path, "r");
	if (!fp) return 0;
	Dictionary_* dic = Calloc(1, sizeof(Dictionary_));
	if (!Fread(&dic->num_allocd,	1, sizeof(dic->num_allocd), fp)) goto _error;
	if (!Fread(&dic->num_used,		1, sizeof(dic->num_used), fp)) goto _error;
	dic->entries = Calloc(dic->num_allocd, sizeof(Dictionary_entry_));
	Dictionary_entry_* es = dic->entries;
	U64 hash, e;
	U64 mask = dic->num_allocd-1;
	U64 num = 0;
	while (Fread(&hash, 1, 8, fp)){
		if (num >= dic->num_used){
			printf("num >= dic->num_used)\n("PLU" >= "PLU")\n", num, dic->num_used);
			abort();
		}
		e = hash & mask;
		U64 count = 0;
		while (dic->entries[e].key){
			e++;
			if (e >= dic->num_allocd){
				e = 0;
			}
			if (++count > dic->num_allocd){
				printf("ind++ count "PLU", dic= "PLU"/"PLU"\n", count, dic->num_used, dic->num_allocd);
				abort();
			}
		}
		es[e].hash = hash;
		if (!Fread(&es[e].key_size, 1, sizeof(es[e].key_size), fp)) goto _error;
		es[e].key = Malloc(es[e].key_size);
		if (!Fread(es[e].key, 1, es[e].key_size, fp)) goto _error;
		es[e].key_needs_free = 1;
		if (!Fread(&es[e].value_size, 1, sizeof(es[e].value_size), fp)) goto _error;
		if (es[e].value_size){
			es[e].value = Malloc(es[e].value_size);
			if (!Fread(es[e].value, 1, es[e].value_size, fp)) goto _error;
			es[e].value_needs_free = 1;
		}
		num++;
	}
	Fclose(fp);
	return dic;
	_error:
	Fclose(fp);
	if (dic){
		free(dic->entries);
	}
	free(dic);
	return 0;
}


void Dictionary_free(Dictionary_* dic){
	if (!dic) return;
	if (dic->entries){
		for (U64 i = 0; i < dic->num_allocd; i++){
			if (dic->entries[i].key_needs_free){
				free(dic->entries[i].key);
			}
			if (dic->entries[i].value_needs_free){
				free(dic->entries[i].value);
			}
		}
		free(dic->entries);
	}
	free(dic);
}


Dictionary_* Dictionary_create(U64 initial_capacity){
	initial_capacity &= UL(0xFFFFFFFFFFFFFFF);
	if (initial_capacity < 32){
		initial_capacity = 32;
	}else if (!Is_pow2(initial_capacity)){
		S32 bits = Num_req_bits(initial_capacity);
		initial_capacity = UL(1) << bits;
	}
	Dictionary_* dic = Malloc(sizeof(Dictionary_));
	dic->num_allocd = initial_capacity;
	dic->entries = Calloc(dic->num_allocd, sizeof(Dictionary_entry_));
	dic->num_used = 0;
	return dic;
}


Dictionary_entry_* Dictionary_find(Dictionary_* dic, void* key, U32 key_size){
	if (!dic || !key || !key_size){
		return 0;
	}
	U64 hash = Hash64(key, key_size);
	U64 ind = hash & (dic->num_allocd-1);
	while (dic->entries[ind].key){
		if (dic->entries[ind].hash == hash 
		&& dic->entries[ind].key_size == key_size
		&& !memcmp(dic->entries[ind].key, key, key_size)){
			return &dic->entries[ind];
		}
		ind++;
		if (ind >= dic->num_allocd){
			ind = 0;
		}
	}
	return 0;
}


static void _dic_rebuild(Dictionary_* dic, U64 newsize){
	Dictionary_entry_* new_entries = Calloc(newsize, sizeof(Dictionary_entry_)); 
	newsize--; // ------ !!!!!
	U64 new_ind;
	for (U64 i = 0; i < dic->num_allocd; i++){
		if (dic->entries[i].key){
			new_ind = (dic->entries[i].hash & newsize); // !
			while (new_entries[new_ind].key){
				new_ind++;
				if (new_ind >= newsize){
					new_ind = 0;
				}
			}
			memcpy(&new_entries[new_ind], &dic->entries[i], sizeof(Dictionary_entry_));
		}
	}
	free(dic->entries);
	dic->entries = new_entries;
	dic->num_allocd = newsize+1; // !!!
}


Dictionary_entry_* Dictionary_add(Dictionary_* dic, void* key, U32 key_size, B copy_key, 
void* value, U32 value_size, B copy_value){
	if (!dic || !key || !key_size || !value || !value_size){
		return 0;
	}
	if (dic->num_used >= (dic->num_allocd*3)>>2){
		_dic_rebuild(dic, dic->num_allocd*2);
	}
	U64 hash = Hash64(key, key_size);
	U64 ind = hash & (dic->num_allocd-1);
	while (dic->entries[ind].key){
		if (dic->entries[ind].hash == hash 
		&& dic->entries[ind].key_size == key_size
		&& !memcmp(dic->entries[ind].key, key, key_size)){
			dic->entries[ind].times_added++;
			dic->entries[ind].value_size = value_size;
			if (dic->entries[ind].value_needs_free){
				free(dic->entries[ind].value);
			}
			if (copy_value){
				dic->entries[ind].value = Malloc(value_size);
				memcpy(dic->entries[ind].value, value, value_size);
				dic->entries[ind].value_needs_free = 1;
			}else{
				dic->entries[ind].value = value;
				dic->entries[ind].value_needs_free = 0;
			}
			return &dic->entries[ind];
		}
		ind++;
		if (ind >= dic->num_allocd){
			ind = 0;
		}
	}
	dic->entries[ind].hash = hash;
	dic->entries[ind].key_size = key_size;
	dic->entries[ind].value_size = value_size;
	dic->entries[ind].times_added = 1;
	if (copy_key){
		dic->entries[ind].key = Malloc(key_size);
		memcpy(dic->entries[ind].key, key, key_size);
		dic->entries[ind].key_needs_free = 1;
	}else{
		dic->entries[ind].key = key;
		dic->entries[ind].key_needs_free = 0;
	}
	if (copy_value){
		dic->entries[ind].value = Malloc(value_size);
		memcpy(dic->entries[ind].value, value, value_size);
		dic->entries[ind].value_needs_free = 1;
	}else{
		dic->entries[ind].value = value;
		dic->entries[ind].value_needs_free = 0;
	}
	dic->num_used++;
	return &dic->entries[ind];
}

