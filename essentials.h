#ifndef ESSENTIALS_H
#define ESSENTIALS_H 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>

typedef uint_least64_t  U64;
typedef uint_least32_t  U32;
typedef uint_least16_t  U16;
typedef uint_least8_t   U8;
typedef uint_least8_t   B;
typedef int_least64_t	S64;
typedef int_least32_t	S32;
typedef int_least16_t	S16;
typedef int_least8_t		S8;
typedef float     F32;
typedef double    F64;

#define U8_MAX		UINT8_MAX
#define U16_MAX	UINT16_MAX
#define U32_MAX	UINT32_MAX
#define U64_MAX	UINT64_MAX
#define S8_MAX		INT8_MAX
#define S16_MAX	INT16_MAX
#define S32_MAX	INT32_MAX
#define S64_MAX	INT64_MAX

#ifdef GARBAGE
	#define L(c)      ((S64)c ## LL)
	#define UL(c)     ((U64)c ## ULL)
	#define PLD       "%lld"
	#define PLU       "%llu"
	#define P016lX    "%016llX"
	#define P016lx    "%016llx"
	#define PATHSLASH '\\'
	#define labs      llabs
	#define atol      atoll
	#define strtol    strtoll
	#define SIGBUS    7
	#define SIGSYS    31
#else
	#define L(c)      ((S64)c ## L)
	#define UL(c)     ((U64)c ## UL)
	#define PLD       "%ld"
	#define PLU       "%lu"
	#define P016lX    "%016lX"
	#define P016lx    "%016lx"
	#define PATHSLASH '/'
#endif


#ifndef DNDEBUG
	#define DPRINT(X, ...)	printf(X, __VA_ARGS__);
#else
	#define DPRINT(X, ...)	((void)0)
#endif

#define LOWERCASE(c)    ((c)|' ')
#define UPPERCASE(c)    ((c)&'_')
#define TOGGLECASE(c)   ((c)^' ')

#define always_il __attribute__((always_inline)) static inline
#undef   static_assert
#define  static_assert(x)      _Static_assert(x);


always_il void* Malloc(U64 numBytes){
	void* s = malloc(numBytes);
	if (!s){
		perror("malloc");
		abort();
	}
	return s;
}

always_il void* Calloc(U64 num, U64 size){
	assert(num && size);
	void* s = calloc(num, size);
	if (!s){
		perror("calloc");
		abort();
	}
	return s;
}

always_il void* Realloc(void* ptr, U64 numBytes){
	assert(numBytes);
	ptr = realloc(ptr, numBytes);
	if (!ptr){
		perror("realloc");
		abort();
	}
	return ptr;
}

always_il FILE* Fopen(const char* path, const char* modes){
	assert(path && modes);
	FILE* fp = fopen(path, modes);
	if (!fp){
		fprintf(stderr, "Failed to open %s\n", path);
		perror("fopen");
		abort();
	}
	return fp;
}

always_il U64 Fwrite(void* src, U64 num, U64 size, FILE* fp){
	assert(src && fp && num && size);
	U64 n = fwrite(src, size, num, fp);
	if (n < num){
		perror("fwrite");
		abort();
	}
	return n;
}

always_il void Fclose(FILE* fp){
	assert(fp);
	if (fclose(fp)){
		perror("fclose");
		abort();
	}
}

always_il U64 Fread(void* dst, U64 num, U64 size, FILE* fp){
	assert(dst && fp && num && size);
	return fread(dst, size, num, fp);
}

always_il U64 File_len(const char* path){
	assert(path);
	struct stat st;
	if (!stat(path, &st)){
		return st.st_size;
	}
	return 0;
}

always_il void* QFread(const char* path, U64* ret_file_len){
	assert(path && ret_file_len);
	U64 len = File_len(path);
	if (!len){
		*ret_file_len = 0;
		return 0;
	}
	FILE* fp = fopen(path, "r");
	if (!fp){
		*ret_file_len = 0;
		return 0;
	}
	U8* buffer = Malloc(len+1);
	Fread(buffer, len, 1, fp);
	buffer[len] = 0;
	Fclose(fp);
	*ret_file_len = len;
	return buffer;
}

always_il U32 Hex_atoi(char c){
	if (c >= '0' && c <= '9') return c - 48;
	return LOWERCASE(c) - 87;
}

always_il B Is_pow2(U64 x){
	return (x > 0) && (!(x & (x - 1)));
}

always_il int Num_req_bits(U64 val){
	if (!val) return 1;
	#ifdef GARBAGE
	return 64 - __builtin_clzll(val);
	#else
	return 64 - __builtin_clzl(val);
	#endif
	/*U32 count = 0;
	while (val){
		val >>= 1;
		count++;
	}
	return count;*/
}

always_il B Bitfield_get_bit(const U8* bf, U64 bitpos){
	return (bf[bitpos>>3] >> (bitpos & 7)) & 1;
}

always_il void Bitfield_set_bit(U8* bf, U64 bitpos, B bit){
	assert(bit <= 1);
	bf[bitpos>>3] = (bf[bitpos>>3] & (~(1u << (bitpos & 7)))) | (bit << (bitpos & 7u));
}

always_il void Bitfield_set_0(U8* bf, U64 bitpos){
	bf[bitpos>>3] &= ~(1u << (bitpos & 7u));
}

always_il void Bitfield_set_1(U8* bf, U64 bitpos){
	bf[bitpos>>3] |= 1u << (bitpos & 7u);
}

always_il void Bitfield_flip_bit(U8* bf, U64 bitpos){
	bf[bitpos>>3] ^= 1u << (bitpos & 7u);
}

always_il U64 read_x_bits(U8* src, U64* bitpos, U8 num_bits){
	U64 value = 0;
	for (U64 i = 0; i < num_bits; i++){
		value <<= 1;
		value |= Bitfield_get_bit(src, *bitpos);
		*bitpos += 1;
	}
	return value;
}


always_il void set_x_bits_from_value(U8* dst, U64* bitpos, U64 value, U8 num_bits){
	for (U64 i = 0; i < num_bits; i++){
		if ((value & (1u << (num_bits-1-i))) > 0){
			dst[(*bitpos)>>3] |= ( 1u << ((*bitpos) & 7) );
		}
		*bitpos += 1;
	}
}


#endif
