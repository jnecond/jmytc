typedef struct Curl_data_ {
	char*		data;
	size_t	len;
} Curl_data_;

always_il void Curl_data_free_insides(Curl_data_* cd){
	free(cd->data);
	memset(cd, 0, sizeof(Curl_data_));
}

CURL*	_curl_ = 0;
B		_curl_init_done_ = 0;

always_il void Curl_quit(){
	if (_curl_ || _curl_init_done_){
		curl_easy_cleanup(_curl_);
		curl_global_cleanup();
		_curl_ = 0;
		_curl_init_done_ = 0;
	}
}

static size_t _curl_callback(void* received_data, size_t len, size_t num, void* userdata_dst){
	size_t received_len = len*num;
	if (received_len){
		Curl_data_* dst = (Curl_data_*)userdata_dst;
		if (dst->len + received_len > (1u << 23)){
			printf("curl_callback: (dst->len + received_len) > (8MiB) ???\nAborting download...\n");
			free(dst->data);
			dst->data = 0;
			dst->len = 0;
			return 0;
		}
		dst->data = Realloc(dst->data, dst->len + received_len + 1);
		memcpy(&dst->data[dst->len], received_data, received_len);
		dst->len += received_len;
		dst->data[dst->len] = 0;
	}
	return received_len;
}

static void _curl_init(){
	if (!_curl_init_done_){
		_curl_init_done_ = 1;
		if (_curl_){
			abort();
		}
		if (curl_global_init(0)){
			printf("curl_global_init() failed\n");
			abort();
		}
		_curl_ = curl_easy_init();
		if (!_curl_){
			printf("curl_easy_init() failed\n");
			abort();
		}
		if (curl_easy_setopt(_curl_, CURLOPT_HSTS_CTRL, CURLHSTS_ENABLE) != CURLE_OK){
			printf("curl_easy_setopt(CURLOPT_HSTS_CTRL, CURLHSTS_ENABLE) failed\n");
			abort();
		}
		if (curl_easy_setopt(_curl_, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL) != CURLE_OK){
			printf("curl_easy_setopt(CURLOPT_USE_SSL, CURLUSESSL_ALL) failed\n");
			abort();
		}
		if (curl_easy_setopt(_curl_, CURLOPT_WRITEFUNCTION, _curl_callback) != CURLE_OK){
			printf("curl_easy_setopt(CURLOPT_WRITEFUNCTION, curl_callback) failed\n");
			abort();
		}
		long v = 1L;
		if (curl_easy_setopt(_curl_, CURLOPT_FOLLOWLOCATION, v) != CURLE_OK){
			printf("curl_easy_setopt(CURLOPT_FOLLOWLOCATION, %ld) failed\n", v);
			abort();
		}
		v = 7L;
		if (curl_easy_setopt(_curl_, CURLOPT_MAXREDIRS, v) != CURLE_OK){
			printf("curl_easy_setopt(CURLOPT_MAXREDIRS, %ld) failed\n", v);
			abort();
		}
		if (curl_easy_setopt(_curl_, CURLOPT_REDIR_PROTOCOLS_STR, "https") != CURLE_OK){
			printf("curl_easy_setopt(CURLOPT_REDIR_PROTOCOLS_STR, \"https\") failed\n");
			abort();
		}
		char* ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/97.0.4692.20 Safari/537.36";
		if (curl_easy_setopt(_curl_, CURLOPT_USERAGENT, ua) != CURLE_OK){
			printf("curl_easy_setopt(CURLOPT_USERAGENT) failed\n");
			abort();
		}
	}
}

B Curl(Curl_data_* dst, char* src_url){
	if (!src_url){
		printf("curl(): !src_url\n");
		return 0;
	}
	if (!dst){
		printf("curl(%s): !dst\n", src_url);
		return 0;
	}
	
	if (!_curl_init_done_){
		_curl_init();
	}
	
	if (curl_easy_setopt(_curl_, CURLOPT_WRITEDATA, (void*)dst) != CURLE_OK){
		printf("curl(%s): curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void*)&dst) failed\n", src_url);
		return 0;
	}

	if (curl_easy_setopt(_curl_, CURLOPT_URL, src_url)){
		printf("curl(%s): curl_easy_setopt(_curl_, CURLOPT_URL, src_url) failed\n", src_url);
		return 0;
	}
	
	memset(dst, 0, sizeof(Curl_data_));
	
	if (curl_easy_perform(_curl_) != CURLE_OK){
		printf("curl(%s): curl_easy_perform() failed\n", src_url);
		return 0;
	}
	if (!dst->data || dst->len < 1){
		printf("curl(%s): (!dst->data || dst->len < 1)\n", src_url);
		return 0;
	}
	return 1;
}

