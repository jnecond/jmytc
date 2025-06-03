#include "essentials.h"
#include "lzw.h"



#define _lzw_max_sym_bits 28 //2^28
typedef struct {
	U64 cur_sym	: _lzw_max_sym_bits;
	U64 prev_sym : _lzw_max_sym_bits;
	U8	last_char;
} _lzw_entry_;
#ifdef GARBAGE
static_assert(sizeof(_lzw_entry_) == 16);
#else
static_assert(sizeof(_lzw_entry_) == 8);
#endif

typedef struct {
	U16 num_used;
	U16 num_allocd;
} _lzw_bucket_info_;

typedef struct {
	_lzw_entry_** entries;
	_lzw_bucket_info_* info;
	U64 mask;
} _lzw_table_;

static U64 _lzw_index(_lzw_table_* table, U64 prev_sym, U8 last_char){ 
	return ((((U64)last_char) << 9) ^ prev_sym) & table->mask;
}

static _lzw_entry_* _lzw_table_add(_lzw_table_* table, U32 new_sym, U32 cur_aka_prev_sym, U8 last_char){
	U64 ind = _lzw_index(table, cur_aka_prev_sym, last_char);
	if (table->info[ind].num_used >= table->info[ind].num_allocd){
		if (table->info[ind].num_allocd > 32767){ // should be at 15*2^12 = 61440
			printf("_lzw_table_add: table size too big for U16\n");
			abort();
		}
		table->info[ind].num_allocd *= 2;
		table->entries[ind] = Realloc(table->entries[ind], 
			sizeof(_lzw_entry_) * table->info[ind].num_allocd);
	}
	table->entries[ind][table->info[ind].num_used].cur_sym = new_sym;
	table->entries[ind][table->info[ind].num_used].prev_sym = cur_aka_prev_sym;
	table->entries[ind][table->info[ind].num_used].last_char = last_char;
	table->info[ind].num_used++;
	return &table->entries[ind][table->info[ind].num_used-1];
}

static _lzw_entry_* _lzw_table_find(_lzw_table_* table, U32 prev_sym, U8 last_char){
	U64 ind = _lzw_index(table, prev_sym, last_char);
	for (U64 x = 0; x < table->info[ind].num_used; x++){
		if (prev_sym == table->entries[ind][x].prev_sym && table->entries[ind][x].last_char == last_char){
			return &table->entries[ind][x];
		}
	}
	return 0;
}

static U64 _lzw_write(U8* dst, U64 bitpos, U64 cur_sym, U64 symbits){
	assert(cur_sym < (1u<<symbits));
	set_x_bits_from_value(dst, &bitpos, cur_sym, symbits);
	return bitpos;
}


LZW_result_ LZW_encode(U8* src, S64 srclen){
	assert(src && srclen);
	LZW_result_ res = {0};
	if (srclen < 5){
		return res;
	}
	res.buf = Calloc(srclen+8, 1);
	_lzw_table_ table = {0};
	{
		U64 buckets;
		U32 nrb = Num_req_bits(srclen)*2/3;
		if (nrb < 5){
			buckets = 32;
		}else{
			buckets = UL(1) << nrb;
		}
		table.mask = buckets-1;
		table.info = Malloc(buckets * sizeof(_lzw_bucket_info_));
		table.entries = Malloc(buckets * sizeof(void*));
		for (U64 i = 0; i < buckets; i++){
			table.info[i].num_allocd = 15;
			table.info[i].num_used = 0;
			table.entries[i] = Malloc(sizeof(_lzw_entry_) * table.info[i].num_allocd);
		}
	}
	U64 new_sym = 256;
	U64 symbits = 9;
	//res.buf[0] = max_sym_bits at the end
	res.buf[1] = src[0];
	U64 bitpos = 16; // == res.buf[2]
	U64 cur_sym = src[1]; // new_sym	prev_sym	last_char
	_lzw_table_add(&table, new_sym++,	src[0],	src[1]);
	_lzw_entry_* entry;
	for (S64 s = 2; s < srclen; s++){
		entry = _lzw_table_find(&table, cur_sym, src[s]);
		if (entry){
			cur_sym = entry->cur_sym;
		}else{
			_lzw_table_add(&table, new_sym++, cur_sym, src[s]);
			if (cur_sym >= (1u << symbits)-1){
				bitpos = _lzw_write(res.buf, bitpos, (1u << symbits)-1, symbits);
				if (++symbits > _lzw_max_sym_bits){
					printf("LZW_encode: symbits > _lzw_max_sym_bits\n");
					abort();
				}
			}
			assert(cur_sym < new_sym);
			bitpos = _lzw_write(res.buf, bitpos, cur_sym, symbits);
			cur_sym = src[s];
		}
	}
	assert(cur_sym < new_sym);
	bitpos = _lzw_write(res.buf, bitpos, cur_sym, symbits);
	res.buf[0] = symbits;
	res.len = bitpos/8;
	if (bitpos&7){
		res.len++;
	}
	{
		U64 ents = table.mask+1;
		for (U64 i = 0; i < ents; i++){
			free(table.entries[i]);
		}
		free(table.entries);
		free(table.info);
	}
	if (res.len+2 > srclen){
		free(res.buf);
		res.buf = 0;
		res.len = 0;
	}
	return res;
}


// decomp ---------
typedef struct {
	U32 prev_sym;
	U8	last_char;
} _lzw_sym;

static_assert(sizeof(_lzw_sym) == 8);

static U8 _lzw_decode_sym(_lzw_sym* table, U32 sym, U8* dst, S64* dst_pos, U32 next_sym){
	U8 firstChar, ch;
	if (sym >= 256){
		ch = table[sym-256].last_char;
		sym = table[sym-256].prev_sym;
		firstChar = _lzw_decode_sym(table, sym, dst, dst_pos, next_sym);
	}else{ 
		ch = sym;
		firstChar = sym;
	}
	dst[*dst_pos] = ch;
	*dst_pos += 1;
	return firstChar;
}

static U32 _lzw_get_sym(U8* src, S64* src_bitpos, U8 symbits){
	return read_x_bits(src, (U64*)src_bitpos, symbits);
}

void* LZW_decode(void* _src_, S64 srclen, S64 original_size){
	U8* src = _src_;
	if (!src || src[0] > _lzw_max_sym_bits){
		printf("LZW_decode: !src || src[0] > 28\n");
		abort();
	}
	if (srclen < 2) abort();
	if (!original_size) abort();
	U8 max_symbits = src[0];
	U8* dst = Malloc(original_size);
	if (max_symbits <= 8){
		for (S64 i = 1; i < srclen; i++){
			dst[i-1] = src[i];
		}
	}
	if (max_symbits > 27){
		abort();
	}
	U32 table_size = 1u << max_symbits;
	_lzw_sym* table = Malloc(sizeof(_lzw_sym) * table_size);
	for (U64 i = 0; i < table_size; i++){
		static_assert(sizeof(table[i].prev_sym) == 4);
		table[i].prev_sym = U32_MAX;
	}
	S64 dst_pos = 0;
	S64 src_bits = srclen*8;
	S64 src_bitpos = 16;
	U32 prev_sym = dst[dst_pos++] = src[1];
	U32 next_sym = 256;
	U32 cur_sym;
	U8	symbits = 9;
	U8	ch = prev_sym;
	while (dst_pos < original_size && src_bits - src_bitpos >= symbits){
		cur_sym = _lzw_get_sym(src, &src_bitpos, symbits);
		if (cur_sym >= (1u << symbits)-1){
			symbits++;
			if (symbits > max_symbits){
				abort();
			}
			continue;
		}
		if (cur_sym < next_sym){
			if (dst_pos >= original_size){
				abort();
			}
			ch = _lzw_decode_sym(table, cur_sym, dst, &dst_pos, next_sym);
		}else{
			if (cur_sym != next_sym){
				abort();
			}
			ch = _lzw_decode_sym(table, prev_sym, dst, &dst_pos, next_sym);
			if (dst_pos >= original_size){
				abort();
			}
			dst[dst_pos++] = ch;
		}
		table[next_sym-256].prev_sym = prev_sym;
		table[next_sym-256].last_char = ch;
		next_sym++;
		prev_sym = cur_sym;
		if (cur_sym >= next_sym){
			abort();
		}
	}
	free(table);
	return dst;
}

