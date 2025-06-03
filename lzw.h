#ifndef LZW_H
#define LZW_H 1


typedef struct LZW_result_{
	U8* buf;
	S64 len;
} LZW_result_;

LZW_result_ LZW_encode(U8* src, S64 srclen);
void* LZW_decode(void* _src_, S64 srclen, S64 original_size);


#endif
