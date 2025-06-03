#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <stdint.h>
#include <wchar.h>
#include <locale.h>

#include <curl/curl.h>

#include "essentials.h"
#include "hash.h"
#include "curl.c"

#define	PROGRAM_NAME	"jmytc"


static const char help_str[] = "-- "PROGRAM_NAME" version 1.00 --\n\
Options:\n\
 [-a]  (adds a channel subscription, takes ID, @name or link)\n\
 [-u]  (updates the list of new videos)\n\
 [-d]  (downloads video[s] with your download_cmd and marks them)\n\
 [-w]  (streams video[s] with your watch_cmd and marks them)\n\
 [-k]  (streams video[s] with your listen_cmd and marks them)\n\
 [-m]  (marks video[s] as watched/old)\n\
 [-p]  (prints video description)\n\
 [-l]  (lists all new/non-watched videos)\n\
 [-o]  (treat ID as an old/seen/marked video)\n\
 [-f]  (finds old videos with matching strings)\n\
 [-v]  (verbose output)\n\
 [-h]  (prints this and exits)\n\
\n\
No video id after option:\n\
 [-d], [-m] select all new videos\n\
\n\
Partial id for new videos:\n\
 [-d Xy], [-m Xy], [-w Xy], [-p Xy] select a new video of which id starts with Xy\n\
\n\
All options need to be in the first argument!\n\
Any following - will be treated as a part of a video id.\n\
\n\
Usage examples:\n\
"PROGRAM_NAME" -ul      (updates and lists new videos)\n\
"PROGRAM_NAME" -d -Xy   (downloads & marks new videos of which id starts with \"-Xy\")\n\
"PROGRAM_NAME" -m       (marks all new videos as watched)\n\
\n\
You can edit %s to remove, rename or manually add channels. The fields must be separated by tabs.\n";


_Noreturn static void __fail(char* file, const char* func, S32 line){
	printf("Aborted at %s : %s : %d\n", file, func, line);
	for (;;) exit(EXIT_FAILURE);
}
#define die()			__fail(__FILE__, __func__, __LINE__)


typedef struct Channel_{
	char* id;
	char* name;
	char* name_truncated;
	char* at_name;
	char* path_db_dir;
	char* path_vid_info_file;	  // compressed descriptions, titles, etc...
	char* path_new_vid_ids_file; // a text file in which vid_ids are appended to
	char* path_old_vid_ids_file; // a text file in which vid_ids are appended to
	Dictionary_* video_info_dict;
	Hash_set_* new_ids;
	Hash_set_* old_ids;
	B video_info_dict_modified;
}	Channel_;

Channel_* channels = 0;
int channels_count = 0;

// key = id
typedef enum Video_info_value {
	V_STR_DURA,
	V_STR_RELEASE_DATE,
	V_STR_TITLE,
	V_STR_DESC,
	V_COUNT
} Video_info_value;

static char* get_value_from_dict_entry(Dictionary_entry_* e, Video_info_value val){
	if (!e) die();
	U16* vp = e->value;
	if (vp && val >= 0 && val < V_COUNT && vp[val] > 0){
		return e->value + vp[val];
	}
	return 0;
}


#define READ_BUF_SIZE	4095
char  READ_BUF[READ_BUF_SIZE+1] = {0};


typedef struct Options_ {
	B		old;
	B		help;
	B		verbose;
	B		update;
	B		list;
	char	action;
} Options_;

Options_ Options = {0};


typedef struct Paths_ {
	char* config_dir;
	char* config_file;
	char* database_dir;
	char* channels_file;
} Paths_;

Paths_ Paths = {0};



#define YT_VIDEOLINK_SIZE     127
#define YT_VIDEOLINK_POS      28
char yt_video_link[YT_VIDEOLINK_SIZE+1] = "https://youtube.com/watch?v=XXXXXXXXXXX";
always_il void yt_video_link_set(char* chid){
	snprintf(yt_video_link+YT_VIDEOLINK_POS, YT_VIDEOLINK_SIZE-YT_VIDEOLINK_POS, "%s", chid);
}

#define FEEDLINK_SIZE   127
#define FEEDLINK_POS    52
char yt_feed_link[FEEDLINK_SIZE+1] = "https://www.youtube.com/feeds/videos.xml?channel_id=UCXXXXXXXXXXXXXXXXXXXXXX";
always_il void yt_feed_link_set(char* chid){
	snprintf(yt_feed_link+FEEDLINK_POS, FEEDLINK_SIZE-FEEDLINK_POS, "%s", chid);
}

#define THUMBLINK_SIZE        127
#define THUMBLINK_POS         24
#define THUMBLINK_REM_SIZE    (THUMBLINK_SIZE-THUMBLINK_POS-1)
char yt_thumb_link[THUMBLINK_SIZE+1] = "https://i1.ytimg.com/vi/XXXXXXXXXXX...";// /maxresdefault.jpg";
always_il void yt_thumb_link_set(char* id){
	if (!id) die();
	snprintf(yt_thumb_link+THUMBLINK_POS, THUMBLINK_REM_SIZE, "%s/maxresdefault.jpg", id);
}



typedef struct Config_ {
	int max_channel_name_columns;
} Config_;

Config_ Config = {0};

char* default_download_cmd[] = {
	"yt-dlp",
	"-f", "bestvideo[height<1088][fps>33]+bestaudio/bestvideo[height<1088]+bestaudio",
	"-o", "~/Youtube/%(uploader).50B　%(title).150B　%(id)s.%(ext)s",
	yt_video_link, 0
};

char* default_watch_cmd[] = {
	"mpv", "--keep-open=no",
	"--ytdl-format=bestvideo[height<1088][fps>33]+bestaudio/bestvideo[height<1088]+bestaudio",
	yt_video_link, 0
};

char* default_listen_cmd[] = {
	"mpv", "--no-video", "--keep-open=no",
	"--ytdl-format=bestaudio",
	yt_video_link, 0
};

enum CONFIG {
	CONF_DOWNLOAD_CMD,
	CONF_WATCH_CMD,
	CONF_LISTEN_CMD,
	CONF_MAX_CHANNEL_NAME_COLUMNS,
	CONF_COUNT
};

char** commands[] = {
	[CONF_DOWNLOAD_CMD] = default_download_cmd,
	[CONF_WATCH_CMD] = default_watch_cmd,
	[CONF_LISTEN_CMD] = default_listen_cmd,
};

static const char* config_strings[] = {
	[CONF_DOWNLOAD_CMD] = "download_cmd=",
	[CONF_WATCH_CMD] = "watch_cmd=",
	[CONF_LISTEN_CMD] = "listen_cmd=",
	[CONF_MAX_CHANNEL_NAME_COLUMNS] = "max_channel_name_columns=",
};


B	COLOR = 1;
#define COLOR_ADD    "\e[48;2;12;30;0m"
#define COLOR_REMOVE "\e[0m"





static B str_match_beginning(const char* str, const char* beg){
	if (!str || !beg || !beg[0]) die();
	for(S64 i = 0; ; i++){
		if (!beg[i]) return 1;
		if (!str[i]) return 0;
		if (str[i] != beg[i]) return 0;
	}
}


typedef struct winsize ws_;
static ws_ ws_get(){
	ws_ ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0){
		perror("ioctl");
		ws.ws_col = 92;
		ws.ws_row = 25;
	}
	return ws;
}

static void print_videos_found_count(S64 count, S64 shown){
	ws_ ws = ws_get();
	if (count > shown){
		int cols = 0;
		if ((COLOR^=1)){
			printf(COLOR_ADD);
		}
		cols = printf("%ld videos found. %ld hidden. Use -v to show all.", count, count-shown);
		if (COLOR){
			for (; cols < ws.ws_col; cols++){
				printf(" ");
			}
			printf(COLOR_REMOVE);
		}
		printf("\n");
	}
	else if (count > ws.ws_row){
		printf("%ld videos found.\n", count);
	}
}


static void write_default_config(){
	printf("Writing default config.\n");
	FILE* fp = Fopen(Paths.config_file, "w");
	fprintf(fp, config_strings[CONF_DOWNLOAD_CMD]);
	int i;
	for (i = 0; default_download_cmd[i]; i++){
		if (!str_match_beginning(default_download_cmd[i], "https://")){
			fprintf(fp, "%s ", default_download_cmd[i]);
		}
	}
	fprintf(fp, "\n");
	fprintf(fp, config_strings[CONF_WATCH_CMD]);
	for (i = 0; default_watch_cmd[i]; i++){
		if (!str_match_beginning(default_watch_cmd[i], "https://")){
			fprintf(fp, "%s ", default_watch_cmd[i]);
		}
	}
	fprintf(fp, "\n");
	fprintf(fp, config_strings[CONF_LISTEN_CMD]);
	for (i = 0; default_listen_cmd[i]; i++){
		if (!str_match_beginning(default_listen_cmd[i], "https://")){
			fprintf(fp, "%s ", default_listen_cmd[i]);
		}
	}
	fprintf(fp, "\n");
	fprintf(fp, config_strings[CONF_MAX_CHANNEL_NAME_COLUMNS]);
	fprintf(fp, "%d", Config.max_channel_name_columns);
	fprintf(fp, "\n");
	
	Fclose(fp);
}

static int stringlist_match(const char* str, const char** list, int first, int last, int* last_linechar_return){
	if (!str || !list) return -1;
	int s, sc;
	for (int lc = 0; str[lc]; lc++){
		for (s = first; s <= last; s++){
			for (sc = 0; list[s][sc]; sc++){
				if (str[lc+sc] != list[s][sc]){
					goto _fail;
				}
			}
			if (last_linechar_return) *last_linechar_return = lc+sc;
			return s;
			_fail: continue;
		}
	}
	if (last_linechar_return) *last_linechar_return = -1;
	return -1;
}

static char* Catpath(char* a, char* b){
	assert(a && b);
	char* cat = Malloc(strlen(a)+strlen(b)+2);
	sprintf(cat, "%s/%s", a, b);
	return cat;
}


static void load_new_ids_file(int c){
	#define idbuf_size 63
	char idbuf[idbuf_size+1] = {0};
	Hash_set_free(channels[c].new_ids);
	channels[c].new_ids = Hash_set_create(32);
	if (!access(channels[c].path_new_vid_ids_file, R_OK)){
		FILE* ids = Fopen(channels[c].path_new_vid_ids_file, "r");	
		while (fgets(idbuf, idbuf_size, ids)){
			char* nl = strchr(idbuf, '\n');
			if (nl){
				nl[0] = 0;
				Hash_set_add(channels[c].new_ids, idbuf, strlen(idbuf)+1, 1);
			}
		}
		Fclose(ids);
	}
}

static void channels_load(){
	FILE* fp = fopen(Paths.channels_file, "r");
	if (!fp){
		printf("No subscribed channels found. Use -a to add them.\n");
		exit(0);
	}
	channels_count = 0;
	while (fgets(READ_BUF, READ_BUF_SIZE, fp)){
		channels_count++;
	}
	if (!channels_count){
		Fclose(fp);
		printf("No subscribed channels found. Use -a to add them.\n");
		exit(0);
	}
	channels = Calloc(channels_count, sizeof(Channel_));
	rewind(fp);
	int lc, c = 0;
	while (fgets(READ_BUF, READ_BUF_SIZE, fp)){
		if (READ_BUF[0] != 'U' || READ_BUF[1] != 'C'){
			continue;
		}
		for(lc = 0; READ_BUF[lc] != '\t'; lc++){
			if (!READ_BUF[lc]){
				die();
			}
		}
		READ_BUF[lc++] = 0;
		channels[c].id = strdup(READ_BUF);
		int start = lc;
		for(; READ_BUF[lc] != '\t'; lc++){
			if (!READ_BUF[lc]){
				die();
			}
		}
		READ_BUF[lc++] = 0;
		channels[c].at_name = strdup(READ_BUF+start);
		start = lc;
		for(; READ_BUF[lc] != '\n'; lc++){
			if (!READ_BUF[lc]){
				die();
			}
		}
		READ_BUF[lc] = 0;
		channels[c].name = strdup(READ_BUF+start);
		channels[c].path_db_dir = Catpath(Paths.database_dir, channels[c].id);
		if (mkdir(channels[c].path_db_dir, 0777)){
			if (errno != EEXIST){
				perror("mkdir");
				die();
			}else{
				DIR* d = opendir(channels[c].path_db_dir);
				if (!d){
					perror("opendir");
					die();
				}
				closedir(d);
			}
		}
		channels[c].path_new_vid_ids_file = Catpath(channels[c].path_db_dir, "new");
		channels[c].path_old_vid_ids_file = Catpath(channels[c].path_db_dir, "old");
		channels[c].path_vid_info_file = Catpath(channels[c].path_db_dir, "data");

		if (!access(channels[c].path_vid_info_file, R_OK)){
			channels[c].video_info_dict = Dictionary_load_compressed(channels[c].path_vid_info_file);
		}else{
			channels[c].video_info_dict = Dictionary_create(32);
		}
		
		char idbuf[idbuf_size+1] = {0};
		load_new_ids_file(c);
				
		channels[c].old_ids = Hash_set_create(128);
		if (!access(channels[c].path_old_vid_ids_file, R_OK)){
			FILE* ids = Fopen(channels[c].path_old_vid_ids_file, "r");	
			while (fgets(idbuf, idbuf_size, ids)){
				char* nl = strchr(idbuf, '\n');
				if (nl){
					nl[0] = 0;
					Hash_set_add(channels[c].old_ids, idbuf, strlen(idbuf)+1, 1);
				}
			}
			Fclose(ids);
		}
		c++;
	}
	Fclose(fp);
	if (Options.verbose){
		for (c = 0; c < channels_count; c++){
			printf("%s | %s | %s\n", channels[c].name, channels[c].at_name, channels[c].id);
		}
	}
}



static U64 str_get_U64(const char* str){
	if (!str) return 0;
	U64 c = 0;
	while (str[c] == ' ' || str[c] == '\t'){
		c++;
	}
	U64 start = c;
	U64 val = 0;
	while (str[c] >= '0' && str[c] <= '9'){
		if (c > start+20){
			return 0;
		}
		val *= 10;
		val += str[c++]-'0';
	}
	return val;
}


static void config_set_defaults(){
	Config.max_channel_name_columns = 14;
}

static void config_load(){
	FILE* fp = Fopen(Paths.config_file, "r");
	int ind, lc, arg, len, c;
	while (fgets(READ_BUF, READ_BUF_SIZE, fp)){
		if((ind = stringlist_match(READ_BUF, config_strings, 0, CONF_COUNT-1, &lc)) >= 0){
			switch(ind){
				case CONF_WATCH_CMD:
				case CONF_DOWNLOAD_CMD:
				case CONF_LISTEN_CMD: {
					#define max_args 255
					#define max_arg_len 2047
					commands[ind] = Malloc((max_args+2)*sizeof(void*));
					arg = 0;
					c = lc;
					while (READ_BUF[c] && READ_BUF[c] != '\n' && c < READ_BUF_SIZE && arg < max_args){
						len = 0;
						commands[ind][arg] = Malloc(max_arg_len+1);
						for (; c < max_arg_len && READ_BUF[c] && READ_BUF[c] != '\n' && READ_BUF[c] != ' '; c++){
							commands[ind][arg][len++] = READ_BUF[c];
						}
						commands[ind][arg][len] = 0;
						commands[ind][arg] = Realloc(commands[ind][arg], len+1);
						arg++;
						for (; READ_BUF[c] == ' '; c++){
							
						}
					}
					commands[ind][arg++] = yt_video_link;
					commands[ind][arg++] = 0;
					commands[ind] = Realloc(commands[ind], arg*sizeof(void*));
				} break;
				case CONF_MAX_CHANNEL_NAME_COLUMNS:{
					Config.max_channel_name_columns = str_get_U64(READ_BUF+lc) & 0xFFFF;
				} break;
			}
		}
	}
	Fclose(fp);
	if (Options.verbose){
		printf("%s ", config_strings[CONF_DOWNLOAD_CMD]);
		for (int i = 0; commands[CONF_DOWNLOAD_CMD][i]; i++){
			if (str_match_beginning(commands[CONF_DOWNLOAD_CMD][i], "https://")){
				break;
			}
			printf("%s ", commands[CONF_DOWNLOAD_CMD][i]);
		}
		printf("\n");
		printf("%s ", config_strings[CONF_WATCH_CMD]);
		for (int i = 0; commands[CONF_WATCH_CMD][i]; i++){
			if (str_match_beginning(commands[CONF_WATCH_CMD][i], "https://")){
				break;
			}
			printf("%s ", commands[CONF_WATCH_CMD][i]);
		}
		printf("\n");
	}
}



static S32 Num_digits(U64 val){
	if (val < UL(10)) return 1;
	if (val < UL(100)) return 2;
	if (val < UL(1000)) return 3;
	if (val < UL(10000)) return 4;
	if (val < UL(100000)) return 5;
	if (val < UL(1000000)) return 6;
	if (val < UL(10000000)) return 7;
	if (val < UL(100000000)) return 8;
	if (val < UL(1000000000)) return 9;
	if (val < UL(10000000000)) return 10;
	if (val < UL(100000000000)) return 11;
	if (val < UL(1000000000000)) return 12;
	if (val < UL(10000000000000)) return 13;
	if (val < UL(100000000000000)) return 14;
	if (val < UL(1000000000000000)) return 15;
	if (val < UL(10000000000000000)) return 16;
	if (val < UL(100000000000000000)) return 17;
	if (val < UL(1000000000000000000)) return 18;
	if (val < UL(10000000000000000000)) return 19;
	return 20;
}

static U8 utoa(U64 uval, char* dst, S32 minimum_digits){
	assert(dst);
	if (!uval){
		for (S32 i = 0; i < minimum_digits; i++){
			*dst++ = '0';
		}
		*dst = 0;
		return minimum_digits;
	}
	S32 ret, ndigits;
	ndigits = Num_digits(uval);
	if (ndigits < minimum_digits){
		ndigits = minimum_digits;
	}
	ret = ndigits;
	dst += ndigits;
	*dst-- = 0;
	while(uval){
		*dst-- = '0' + (uval % 10U);
		uval /= 10U;
		ndigits--;
	}
	while (ndigits > 0){
		*dst-- = '0';
		ndigits--;
	}
	return ret;
}

static void Timestring(char* dst, U64 dst_size, U64 t_ms, U8 decimals){
	if (!dst || dst_size < 12) die();
	U32 hh = (t_ms / 3600000u);
	if (hh >= 100){
		strcpy(dst, ">99:59:59");
		return;
	}
	U32 rem,mm,ss;
	rem = (t_ms % 3600000u);
	mm = rem / 60000;
	rem %= 60000;
	ss = rem / 1000;
	U8 p;
	if (hh){ // 1:01:30.75
		p = utoa(hh, dst, 1);
		dst[p++] = ':';
		p += utoa(mm, dst+p, 2);
		dst[p++] = ':';
		p += utoa(ss, dst+p, 2);
	}else if (mm){
		p = utoa(mm, dst, 1);
		dst[p++] = ':';
		p += utoa(ss, dst+p, 2);
	}else{
		p = utoa(ss, dst, 1);
	}
	if (decimals){
		dst[p++] = '.';
		rem %= 1000;
		if (decimals == 2){
			p += utoa(rem/10, dst+p, 2);
		}else if (decimals == 3){
			p += utoa(rem, dst+p, 3);
		}else{
			p += utoa(rem/100, dst+p, 1);
		}
	}
	dst[p] = 0;
}


static int display_columns(const char* str) {
	if (!str) die();
	size_t len = strlen(str);
	wchar_t* wstr = Malloc((len+1)*sizeof(wchar_t));
	mbstowcs(wstr, str, len + 1);
	int cols = wcswidth(wstr, len);
	free(wstr);
	return cols >= 0 ? cols : strlen(str);
}

static int truncate_to_max_cols(const char* src, int max_cols, char* dst, size_t dst_size){
	if (!src || !dst || !dst_size) {
		die();
	}
	if (max_cols <= 0){
		dst[0] = 0;
		return 0;
	}
	size_t len = strlen(src);
	wchar_t* wstr = Malloc((len + 1) * sizeof(wchar_t));
	mbstowcs(wstr, src, len + 1);
	int current_width = 0;
	size_t i;
	for (i = 0; wstr[i]; i++) {
		int char_width = wcwidth(wstr[i]);
		if (char_width < 0) char_width = 1;
		if (current_width + char_width > max_cols){
			break;
		}
		current_width += char_width;
	}
	wstr[i] = 0;
	if (wcstombs(dst, wstr, dst_size) < 0){
		dst[0] = 0;
	}
	dst[dst_size-1] = 0;
	free(wstr);
	return current_width;
}


static void print_video_line(int channel, char* video_id){
	if (!video_id){
		die();
	}
	ws_ ws = ws_get();
	Dictionary_entry_* e = Dictionary_find(channels[channel].video_info_dict, 
		video_id, strlen(video_id)+1);
	if (!e){
		return;
	}
	char* title = get_value_from_dict_entry(e, V_STR_TITLE);
	if (!title){
		printf("print_video_line: (!title)\n");
		return;
	}
	char dura[16];
	char* v_dura = get_value_from_dict_entry(e, V_STR_DURA);
	if (!v_dura || !v_dura[0] || v_dura[0] == '?'){
		snprintf(dura, 15, "?");
	}else{
		snprintf(dura, 15, "%s", v_dura);
	}
	if ((COLOR^=1)){
		printf(COLOR_ADD);
	}

	int n = printf("%s ", video_id);
	printf("%s", channels[channel].name_truncated);
	int chname_cols = display_columns(channels[channel].name_truncated);
	n += chname_cols;
	for (int i = chname_cols; i < Config.max_channel_name_columns+1; i++){
		printf(" ");
		n++;
	}
	n += printf("[%s] ", dura);
	
	int remaining_cols = ws.ws_col - n;
	if (remaining_cols < 0) remaining_cols = 0;

	remaining_cols -= truncate_to_max_cols(title, remaining_cols, READ_BUF, READ_BUF_SIZE);
	printf("%s", READ_BUF);      
	if (COLOR){
		for (int i = 0; i < remaining_cols; i++){
			printf(" ");
		}
		printf(COLOR_REMOVE);
	}
	printf("\n");
}



static B download_yt_video_page(Curl_data_* dst, char* video_id){
	if (!dst || !video_id){
		printf("(!dst || !video_id)\n");
		die();
	}
	yt_video_link_set(video_id);
	Curl(dst, yt_video_link);
	if (!dst->data || dst->len < 1024){
		return 0;
	}
	return 1;
}


static char* get_unescaped_xml_ec_copy(const char* xml, const char* start_tag, const char* end_tag, S64* ret_size){
	if (!xml || !start_tag || !end_tag || !ret_size|| !start_tag[0] || !end_tag[0] ){
		die();
	}
	*ret_size = 0;
	char* start = strstr(xml, start_tag);
	if (!start) return 0;
	start += strlen(start_tag);
	char* end = strstr(start, end_tag);
	if (!end) return 0;
	S64 src_len = end-start;
	S64 copy_size = src_len+8;
	char* copy = Malloc(copy_size);
	S64 s,c;
	s = c = 0;
	while (s < src_len && start[s]){
		if (start[s] != '&'){
			copy[c++] = start[s++];
			continue;
		}
		if (s+1 < src_len && start[s+1] == '#'){ //&#x410;  &#20013;
			U64 val = 0;
			if (s+2 < src_len && (start[s+2] == 'x' || start[s+2] == 'X')){
				s += 3;
				while(s < src_len &&
					(     (start[s] >= '0' && start[s] <= '9')
						|| (start[s] >= 'A' && start[s] <= 'F')
						|| (start[s] >= 'a' && start[s] <= 'f')
					)
				){
					val = (val << 4) | Hex_atoi(start[s++]);
				}
			}
			else{
				s += 2;
				while(s < src_len && (start[s] >= '0' && start[s] <= '9')){
					val = (val*10) + (start[s]-48);
					s++;
				}
			}
			if (start[s] == ';'){
				s++;
				if (val){
					if (val <= 0x10FFFF){
						if (val <= 127){ //0x7F
							if (val == '\n' || val == '\r'){
								copy[c++] = '\n';
							}
							if (val >= 32 && val < 127){
								copy[c++] = val;
							}
						}
						else if (val <= 0x7FF){
							copy[c++] = (0xC0 | (val >> 6));
							copy[c++] = (0x80 | (val & 0x3F));
						}
						else if (val <= 0xFFFF){
							copy[c++] = (0xE0 | (val >> 12));
							copy[c++] = (0x80 | ((val >> 6) & 0x3F));
							copy[c++] = (0x80 | (val & 0x3F));
						}
						else{
							copy[c++] = (0xF0 | (val >> 18));
							copy[c++] = (0x80 | ((val >> 12) & 0x3F));
							copy[c++] = (0x80 | ((val >> 6) & 0x3F));
							copy[c++] = (0x80 | (val & 0x3F));
						}
					}
				}
				continue;
			}
			goto _error;
		}
		if(s+4 < src_len && str_match_beginning(start+s+1, "amp;")){
			copy[c++] = '&';
			s += 5;
			continue;
		}
		if(s+5 < src_len && str_match_beginning(start+s+1, "apos;")){
			copy[c++] = '\'';
			s += 6;
			continue;
		}
		if(s+5 < src_len && str_match_beginning(start+s+1, "quot;")){
			copy[c++] = '"';
			s += 6;
			continue;
		}
		if(s+3 < src_len && str_match_beginning(start+s+1, "lt;")){
			copy[c++] = '<';
			s += 4;
			continue;
		}
		if(s+3 < src_len && str_match_beginning(start+s+1, "gt;")){
			copy[c++] = '>';
			s += 4;
			continue;
		}
		goto _error;
	}
	copy[c++] = 0;
	*ret_size = c;
	copy = Realloc(copy, *ret_size);
	return copy;
_error:
	free(copy);
	return 0;
}

static char* get_xml_ec_copy(const char* xml, const char* start_tag, const char* end_tag, S64* ret_size){
	if (!xml || !start_tag || !end_tag || !ret_size){
		die();
	}
	*ret_size = 0;
	char* start = strstr(xml, start_tag);
	if (!start) return 0;
	start += strlen(start_tag);
	char* end = strstr(start, end_tag);
	if (!end) return 0;
	*ret_size = end-start+1;
	char* copy = Malloc(*ret_size);
	memcpy(copy, start, *ret_size-1);
	copy[*ret_size-1] = 0;
	return copy;
}

static char* get_xml_ec_ptr(char* xml, const char* start_tag, const char* end_tag, S64* ret_size){
	if (!xml || !start_tag || !end_tag || !ret_size){
		die();
	}
	*ret_size = 0;
	char* start = strstr(xml, start_tag);
	if (!start) return 0;
	start += strlen(start_tag);
	char* end = strstr(start, end_tag);
	if (!end) return 0;
	end[0] = 0;
	*ret_size = end-start+1;
	return start;
}


static void feed_check(int c, Curl_data_ xml){
	char* remaining = xml.data;
	//#define _xmldec	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	B save_xml = 0;
	
	for(;;){
		S64 entry_size = 0;
		char* entry = get_xml_ec_ptr(remaining, "<entry>", "</entry>", &entry_size);
		if (!entry){
			break;
		}
		if (entry_size < 1){
			remaining = entry+1;
			continue;
		}
		S64 id_size = 0;
		char* id = get_xml_ec_copy(entry, "<yt:videoId>", "</yt:videoId>", &id_size);
		if (!id){
			printf("\nFailed to get video ID from XML (%s)\n", channels[c].name);
			remaining = entry+entry_size+1;
			continue;
		}
		if (id_size < 1){
			printf("\n(id_size < 1)\n");
			die();
		}
		if(Dictionary_find(channels[c].video_info_dict, id, id_size)){
			free(id);
			remaining = entry+entry_size+1;
			continue;
		}
		void* values[V_COUNT] = {0};
		S64 sizes[V_COUNT] = {0};
		S64 pos[V_COUNT] = {0};
		values[V_STR_RELEASE_DATE] = get_xml_ec_copy(entry, 
			"<published>", "</published>", &sizes[V_STR_RELEASE_DATE]);
		if (!values[V_STR_RELEASE_DATE]){
			printf("\nFailed to get <published> from XML (%s)\n", channels[c].name);
			goto _error;
		}
		//<media:group> non copy
		{
			S64 media_size;
			void* media = get_xml_ec_ptr(entry, "<media:group>", "</media:group>", &media_size);
			if (!media){
				printf("\nFailed to get <media:group> from XML (%s)\n", channels[c].name);
				goto _error;
			}
			values[V_STR_TITLE] = get_unescaped_xml_ec_copy(media, "<media:title>", "</media:title>", &sizes[V_STR_TITLE]);
			if (!values[V_STR_TITLE] || values[V_STR_TITLE] > media+media_size){
				printf("\nFailed to get <media:title> from XML (%s)\n", channels[c].name);
				goto _error;
			}
			values[V_STR_DESC] = get_unescaped_xml_ec_copy(media, 
				"<media:description>", "</media:description>", &sizes[V_STR_DESC]);
			if (!values[V_STR_DESC]){
				printf("\nFailed to get <media:description> from XML (%s)\n", channels[c].name);
				goto _error;
			}
		}
		Curl_data_ page = {0};
		if (!values[V_STR_DURA]){
			memset(&page, 0, sizeof(page));
			if (download_yt_video_page(&page, id)){
				#define yt_dura_tag "\"lengthSeconds\":\"" //0","
				char* dura = strstr(page.data, yt_dura_tag);
				if (dura){
					dura += sizeof(yt_dura_tag)-1;
					if (dura[0] == ':') die();
					if (dura[0] == '"') die();
					U64 duraint = str_get_U64(dura);
					if (duraint){
						values[V_STR_DURA] = Calloc(1, 16);
						Timestring(values[V_STR_DURA], 16, duraint*1000, 0);
					}else{
						values[V_STR_DURA] = strdup("?");
					}
				}else{
					printf("\nyoutube page missing dura tag (%s)\n", yt_dura_tag);
				}
				free(page.data);
			}else{
				printf("\nyoutube page dl failed\n");
			}
		}
		if (!values[V_STR_DURA]){
			values[V_STR_DURA] = strdup("?");
		}
		sizes[V_STR_DURA] = 1 + strlen(values[V_STR_DURA]);
		static_assert(V_STR_DESC == V_COUNT-1);
		int total_max_size = U16_MAX;
		int remaining_for_desc = total_max_size;
		for (int v = 0; v < V_COUNT; v++){
			if (!values[v]){
				printf("\nERROR: !values[%d]\n", v);
				goto _error;
			}
			if (sizes[v] < 1){
				printf("\nERROR: size[%d] (%ld) < 1\n", v, sizes[v]);
				goto _error;
			}
			if (v != V_STR_DESC){
				remaining_for_desc -= sizes[v];
			}
			if (((char*)values[v])[sizes[v]-1] != 0){
				((char*)values[v])[sizes[v]-1] = 0;
			}
		}
		if (sizes[V_STR_DESC] > remaining_for_desc){
			sizes[V_STR_DESC] = remaining_for_desc;
			((char*)values[V_STR_DESC])[sizes[V_STR_DESC]-1] = 0;
		}
		pos[0] = 2*V_COUNT;
		for (int v = 1; v < V_COUNT; v++){
			if (!pos[v-1] || !sizes[v-1]) die();
			pos[v] = pos[v-1] + sizes[v-1];
			if (pos[v] >= U16_MAX){
				printf("\n(pos[%d] (%ld) >= U16_MAX)\n", v, pos[v]);
				die();
			}
		}
		U64 total_value_size = pos[V_COUNT-1] + sizes[V_COUNT-1];
		if (total_value_size > U16_MAX) die();
		void* value = Malloc(total_value_size);
		U16* vp = (U16*)value;
		for (int v = 0; v < V_COUNT; v++){
			vp[v] = pos[v];
			memcpy(value+pos[v], values[v], sizes[v]);
			free(values[v]);
		}
		Dictionary_entry_* e = Dictionary_add(channels[c].video_info_dict, id, id_size, 0, value, total_value_size, 0);
		if (!e) die();
		e->key_needs_free = 1;
		e->value_needs_free = 1;
		channels[c].video_info_dict_modified = 1;
		
		Hash_set_add(channels[c].new_ids, id, id_size, 1);
		FILE* fp = Fopen(channels[c].path_new_vid_ids_file, "a");
		fprintf(fp, "%s\n", id);
		Fclose(fp);
		
		print_video_line(c, id);
		
		entry[entry_size-1] = '<';
		remaining = entry+entry_size;
		continue;
		//----------------
		_error:
		save_xml = 1;
		free(id);
		for (int i = 0; i < V_COUNT; i++){
			free(values[i]);
		}
		entry[entry_size-1] = '<';
		remaining = entry+entry_size;
	}
	if (save_xml){
		char* xp = Catpath(channels[c].path_db_dir, "error.xml");
		FILE* fp = fopen(xp, "w");
		if (fp){
			printf("saving xml to %s\n", xp);
			fwrite(xml.data, 1, xml.len, fp);
			fprintf(fp, "\n");
			Fclose(fp);
		}
		free(xp);
	}
}

static B download_feed(int c, Curl_data_* xml){
	if (c < 0 || c >= channels_count || !xml){
		printf("(c < 0 || c >= channels_count || !xml)\n");
		printf("%d, %p\n", c, xml);
		die();
	}
	yt_feed_link_set(channels[c].id);
	if (!Curl(xml, yt_feed_link)){
		return 0;
	}
	if (!xml->data || xml->len < 512){
		printf("(!xml->data || xml->size < 512)\n");
		return 0;
	}
	return 1;
}

static void Update(){
	Curl_data_ xml = {0};
	for (int c = 0; c < channels_count && channels[c].name && channels[c].id; c++){
		if (download_feed(c, &xml)){
			feed_check(c, xml);
			free(xml.data);
			memset(&xml, 0, sizeof(xml));
		}else{
			printf("Downloading feed for \"%s\" failed\n", channels[c].name);
		}
	}
}



static void set_truncated_channel_names(B include_channels_without_new_videos){
	int c, max = 0;
	for (c = 0; c < channels_count; c++){
		if (include_channels_without_new_videos
		|| channels[c].new_ids->num_used > 0){
			int cols = display_columns(channels[c].name);
			if (cols > max){
				max = cols;
			}
			if (max >= Config.max_channel_name_columns){
				max = Config.max_channel_name_columns;
				break;
			}
		}
	}
	if (!max){
		max = Config.max_channel_name_columns;
	}
	Config.max_channel_name_columns = max;
	for (c = 0; c < channels_count; c++){
		free(channels[c].name_truncated);
		size_t size = max*2+1;
		channels[c].name_truncated = Malloc(size);
		truncate_to_max_cols(channels[c].name, max, channels[c].name_truncated, size);
	}
}

static void cmd_list_videos(){
	if (!Options.old){
		printf(COLOR_ADD"New:"COLOR_REMOVE"\n");
	}
	if (channels_count < 1){
		return;
	}
	ws_ ws = ws_get();
	int ei, c;
	S64 count = 0;
	S64 shown = ws.ws_row-3;
	if (Options.verbose) shown = S64_MAX;
	char* chname = Calloc(Config.max_channel_name_columns*2+2, 1);
	for (c = 0; c < channels_count; c++){
		Hash_set_* set = channels[c].new_ids;
		if (Options.old){
			set = channels[c].old_ids;
		}
		Hash_set_entry_* e;
		for (ei = 0; ei < set->num_allocd; ei++){
			e = &set->entries[ei];
			if (e->data){
				if (count++ < shown){
					print_video_line(c, e->data);
				}
			}
		}
	}
	free(chname);
	print_videos_found_count(count, shown);
}


static void print_video_detailed(int c, char* video_id, U32 id_size){
	Dictionary_entry_* e = Dictionary_find(channels[c].video_info_dict, video_id, id_size);
	if (!e) return;
	printf("%s\n%s\n", channels[c].name, channels[c].at_name);
	char* title = get_value_from_dict_entry(e, V_STR_TITLE);
	char* dura = get_value_from_dict_entry(e, V_STR_DURA);
	printf("[%s] %s\n", dura, title);
	char* v_date = get_value_from_dict_entry(e, V_STR_RELEASE_DATE);
	if (v_date){
		char date[32];
		snprintf(date, 31, "%s", v_date);
		char* T = strchr(date, 'T');
		if (T){
			*T = ' ';
		}
		printf("%s\n", date);
	}
	yt_video_link_set(video_id);
	yt_thumb_link_set(video_id);
	printf("%s\n", yt_video_link);
	printf("%s\n", yt_thumb_link);
	char* desc = get_value_from_dict_entry(e, V_STR_DESC);
	if (!desc) return;
	printf("\n");
	if (desc && desc[0]){
		printf("%s\n", desc);
	}
}

static void cmd_print(char** ids, int idc){
	S64 matches = 0;
	S64 shown_matches = 1;
	if (Options.verbose){
		shown_matches = S64_MAX;
	}
	if (idc < 1 || !ids || !ids[0]){
		for (int c = 0; c < channels_count; c++){
			Hash_set_* set = channels[c].new_ids;
			if (Options.old){
				set = channels[c].old_ids;
			}
			for (int e = 0; e < set->num_allocd; e++){
				if (set->entries[e].data){
					if (matches++ < shown_matches){
						print_video_detailed(c, set->entries[e].data, 
							set->entries[e].data_size);
					}
				}
			}
		}
	}else if (ids){
		for (int c = 0; c < channels_count; c++){
			Hash_set_* set = channels[c].new_ids;
			if (Options.old){
				set = channels[c].old_ids;
			}
			for (int e = 0; e < set->num_allocd; e++){
				if (set->entries[e].data){
					for (int i = 0; i < idc && ids[i]; i++){
						if (str_match_beginning(set->entries[e].data, ids[i])){
							if (matches++ < shown_matches){
								print_video_detailed(c, set->entries[e].data, 
									set->entries[e].data_size);
							}
						}
					}
				}
			}
		}
	}
	print_videos_found_count(matches, shown_matches);
}



static S32 Exec_wait(char** cmd){
	if (!cmd){
		die();
	}
	pid_t pid = fork();
	if (pid == -1){
		perror("fork: ");
		for(;;)exit(EXIT_FAILURE);
	}
	if (pid == 0){ //child
		execvp(cmd[0], cmd);
		for(;;)exit(EXIT_FAILURE);//exec never returns
	} 
	S32 status = -1;
	waitpid(-1, &status, 0);
	return status;
}

static B Listen(char* id){
	if (!id) die();
	yt_video_link_set(id);
	int status = Exec_wait(commands[CONF_LISTEN_CMD]);
	return (status == 0);
}

static B watch(char* id){
	if (!id) die();
	yt_video_link_set(id);
	//Exec(commands[CONF_WATCH_CMD]);
	int status = Exec_wait(commands[CONF_WATCH_CMD]);
	return (status == 0);
}

static B download(char* id){
	if (!id) die();
	yt_video_link_set(id);
	int status = Exec_wait(commands[CONF_DOWNLOAD_CMD]);
	return (status == 0);
}

static void marking_action(char a, char** ids, int idc){
	switch (a){
		case 'd':
		case 'w': 
		case 'k': {
			break;
		}
		default: {
			printf("invalid action (%c) in marking_action\n", a);
			die();
		} break;
	}
	if (Options.old){
		if (idc < 1 || !ids || !ids[0]){
			printf("Can't watch/dl all old videos\n");
			return;
		}
		for (int c = 0; c < channels_count; c++){
			for (int e = 0; e < channels[c].old_ids->num_allocd; e++){
				if (channels[c].old_ids->entries[e].data){
					for (int i = 0; i < idc; i++){
						if (str_match_beginning(channels[c].old_ids->entries[e].data, ids[i])){
							switch(a){
								case 'd':
									download(channels[c].old_ids->entries[e].data);
								break;
								case 'w':
									watch(channels[c].old_ids->entries[e].data);
								break;
								case 'k':
									Listen(channels[c].old_ids->entries[e].data);
								break;
								default:
									die();
								break;
							}
							break;
						}
					}
				}
			}
		}
		return;
	}
	B need_match = 1;
	if (idc < 1 || !ids || !ids[0]){
		need_match = 0; // all new videos
	}
	for (int c = 0; c < channels_count; c++){
		//_start:
		if (channels[c].new_ids->num_used < 1){
			continue;
		}
		for (int e = 0; e < channels[c].new_ids->num_allocd; e++){
			if (!channels[c].new_ids->entries[e].data){
				continue;
			}
			B mark = 0;
			if (need_match){
				for (S32 i = 0; i < idc; i++){
					if (str_match_beginning(channels[c].new_ids->entries[e].data, ids[i])){
						goto _take_action;
					}
				}
				continue;
			}
			_take_action:
			switch(a){
				case 'd':
					mark = download(channels[c].new_ids->entries[e].data);
				break;
				case 'w':
					mark = watch(channels[c].new_ids->entries[e].data);
				break;
				case 'k':
					mark = Listen(channels[c].new_ids->entries[e].data);
				break;
				default:
					die();
				break;
			}
			if (mark){
				Hash_set_entry_* ent = Hash_set_add(channels[c].old_ids, 
					channels[c].new_ids->entries[e].data,
					channels[c].new_ids->entries[e].data_size, 0);
				ent->free_data_on_remove = 1;
				// TODO: reread old ids in case mark was used while downloading/watching
				FILE* fp = Fopen(channels[c].path_old_vid_ids_file, "a");
				fprintf(fp, "%s\n", (char*)channels[c].new_ids->entries[e].data);
				Fclose(fp);
				
				// TODO: reread new ids in case update was used while downloading/watching
				// load_new_ids_file(c);
				// have to jump back if the set is modified... or something
				channels[c].new_ids->entries[e].free_data_on_remove = 0;
				Hash_set_remove_by_index(channels[c].new_ids, e);
				fp = Fopen(channels[c].path_new_vid_ids_file, "w");
				for (int e = 0; e < channels[c].new_ids->num_allocd; e++){
					if (channels[c].new_ids->entries[e].data){
						fprintf(fp, "%s\n", (char*)channels[c].new_ids->entries[e].data);
					}
				}
				Fclose(fp);
			
				printf(COLOR_ADD"Marked as watched:"COLOR_REMOVE"\n");
				COLOR = 1;
				print_video_line(c, ent->data);
			}
		}
	}
}


static void cmd_mark_videos(char** ids, int idc){
	if (Options.old){
		printf("Can't mark old videos\n");
		return; 
	}
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0){
		perror("ioctl");
		ws.ws_col = 92;
	}
	B need_match = 1;
	if (idc < 1 || !ids || !ids[0]){
		need_match = 0; // mark all
	}
	Hash_set_entry_*** marked = Calloc(channels_count, sizeof(void*));
	int* marked_count = Calloc(channels_count, sizeof(int));
	for (int c = 0; c < channels_count; c++){
		if (channels[c].new_ids->num_used < 1){
			continue;
		}
		marked[c] = Calloc(channels[c].new_ids->num_used, sizeof(void*));
		marked_count[c] = 0;
		B new_ids_file_needs_rewrite = 0;
		for (int e = 0; e < channels[c].new_ids->num_allocd; e++){
			if (!channels[c].new_ids->entries[e].data){
				continue;
			}
			if (need_match){
				for (int i = 0; i < idc; i++){
					if (str_match_beginning(channels[c].new_ids->entries[e].data, ids[i])){
						goto _mark;
					}
				}
				continue;
			}
			_mark:
			marked[c][marked_count[c]++] = Hash_set_add(channels[c].old_ids, channels[c].new_ids->entries[e].data, 
				channels[c].new_ids->entries[e].data_size, 1);
			FILE* fp = Fopen(channels[c].path_old_vid_ids_file, "a");
			fprintf(fp, "%s\n", (char*)channels[c].new_ids->entries[e].data);
			Fclose(fp);
			Hash_set_remove_by_index(channels[c].new_ids, e);
			new_ids_file_needs_rewrite = 1;
		}
		if (new_ids_file_needs_rewrite){
			FILE* fp = Fopen(channels[c].path_new_vid_ids_file, "w");
			for (int e = 0; e < channels[c].new_ids->num_allocd; e++){
				if (channels[c].new_ids->entries[e].data){
					fprintf(fp, "%s\n", (char*)channels[c].new_ids->entries[e].data);
				}
			}
			Fclose(fp);
		}
	}
	printf(COLOR_ADD"Marked as watched:"COLOR_REMOVE"\n");
	for (int c = 0; c < channels_count; c++){
		for (int m = 0; m < marked_count[c]; m++){
			if (marked[c][m] && marked[c][m]->data){
				print_video_line(c, marked[c][m]->data);
			}
		}
		free(marked[c]);
	}
	free(marked);
	free(marked_count);
}



static void cmd_channel_add(char** channels, int count){
	if (count < 1){
		return;
	}
	char** ch_ids = Malloc(count * sizeof(void*));
	char** at_names = Malloc(count * sizeof(void*));
	char** ch_names = Malloc(count * sizeof(void*));
	B* found = Calloc(count, sizeof(B));
	#define LINK_MAX 255
	char link[LINK_MAX+1] = {0};
	Curl_data_ page = {0};
	for (int i = 0; i < count; i++){
		if (channels[i][0] == '@'){
			snprintf(link, LINK_MAX, "https://www.youtube.com/%s", channels[i]);
			char* v = strchr(link, '@');
			if (v){
				v = strchr(v, '/');
				if (v){
					v = 0;
				}
			}
			if (Options.verbose){
				printf("Downloading page: %s\n", link);
			}
			if (!Curl(&page, link)){
				fprintf(stderr, "Failed to download page \"%s\"\n", link);
				die();
			}
		}
		else if (channels[i][0] == 'U' && channels[i][1] == 'C'){ // ID
			if (strlen(channels[i]) != 24){
				fprintf(stderr, "channel ID length should be 24 characters.\n");
				die();
			}
			snprintf(link, LINK_MAX, "https://www.youtube.com/channel/%s", channels[i]);
			if (Options.verbose){
				printf("Downloading page: %s\n", link);
			}
			if (!Curl(&page, link)){
				fprintf(stderr, "Failed to download page \"%s\"\n", link);
				die();
			}
		}
		else if (strstr(channels[i], "youtube.com") || strstr(channels[i], "youtu.be")){
			if (Options.verbose){
				printf("Downloading page: %s\n", channels[i]);
			}
			if (!Curl(&page, channels[i])){
				fprintf(stderr, "Failed to download page \"%s\"\nTry with the channel ID or @name only?\n", channels[i]);
				die();
			}
		}
		else{
			fprintf(stderr, "cmd_channel_add: a valid channel ID, @name or link required\n(arg was: %s)\n", channels[i]);
			die();
		}
		
		char* page_end = page.data+page.len-3;
		// get id, @name, name
		#define _link_rel		"<link rel=\"canonical\""
		char* a = strstr(page.data, _link_rel);
		if (!a){
			fprintf(stderr, _link_rel" not found!\n");
			die();
		}
		char* end;
		
		#define UC_ID_LEN 24  // UC14QT5j2nQI8lKBCGtrrBQA
		#define _ch_uc		"/channel/UC"
		char* b = strstr(a, _ch_uc);
		if (!b){
			fprintf(stderr, _ch_uc" not found!\n");
			die();
		}
		b += strlen(_ch_uc) - 2;
		if (b[UC_ID_LEN] != '"'){
			fprintf(stderr, "(b[24] != '\"')\n");
			die();
		}
		b[UC_ID_LEN] = 0;
		ch_ids[i] = strdup(b);
		b[UC_ID_LEN] = '"';
		if (Options.verbose){
			printf("ID:    %s\n", ch_ids[i]);
		}
		b += 25;
		if (b > page_end){
			fprintf(stderr, "(b > page_end)\n");
			die();
		}
		
		#define _chname	"<meta property=\"og:title\" content=\""
		S64 size = 0;
		ch_names[i] = get_unescaped_xml_ec_copy(b, _chname, "\">", &size);
		if (!ch_names[i]){
			fprintf(stderr, _chname" not found!\n");
			die();
		}
		if (Options.verbose){
			printf("name:  %s\n", ch_names[i]);	
		}
		
		char* atname = strstr(b, "webCommandMetadata\":{\"url\":\"/@");
		if (!atname){
			atname = strstr(b, "canonicalBaseUrl\":\"/@");
		}
		if (!atname){
			atname = strstr(b, "youtube.com/@");
		}
		if (!atname){
			fprintf(stderr, "@name not found!\n");
			FILE* fff = Fopen("test", "w");
			fwrite(page.data, 1, page.len, fff);
			fprintf(fff, "\n");
			Fclose(fff);
			die();
		}
		atname = strchr(atname, '@');
		end = strchr(atname, '/');
		if (!end){
			end = strchr(atname, '"');
		}
		if (!end){
			fprintf(stderr, "(!end)\n");
			die();
		}
		*end = 0;
		at_names[i] = strdup(atname);
		if (Options.verbose){
			printf("@name: %s\n", at_names[i]);
		}
		
		Curl_data_free_insides(&page);
		printf("adding: %s - %s - %s\n", at_names[i], ch_names[i], ch_ids[i]);
	}
	
	FILE* fp = fopen(Paths.channels_file, "r+");
	if (!fp){
		fp = Fopen(Paths.channels_file, "w+");
	}
	while (fgets(READ_BUF, READ_BUF_SIZE, fp)){
		for (int i = 0; i < count; i++){
			if (strstr(READ_BUF, ch_ids[i])){
				found[i] = 1;
				printf("already subscribed: %s - %s - %s\n", at_names[i], ch_names[i], ch_ids[i]);
			}
		}
	}
	fseek(fp, 0, SEEK_END);
	for (int i = 0; i < count; i++){
		if (!found[i]){
			fprintf(fp, "%s\t%s\t%s\n", ch_ids[i], at_names[i], ch_names[i]);
		}
		free(ch_ids[i]);
		free(at_names[i]);
		free(ch_names[i]);
	}
	free(found);
	free(ch_ids);
	free(ch_names);
	free(at_names);
	Fclose(fp);
}



static void cmd_find(char** phrases, int pc){
	if (!phrases || pc < 1){
		return;
	}
	printf(COLOR_ADD"Found:"COLOR_REMOVE"\n");
	int matches = 0;
	ws_ ws = ws_get();
	int shown_matches = ws.ws_row-3;
	if (Options.verbose){
		shown_matches = INT_MAX;
	}
	int c, ei, p, v;
	for (c = 0; c < channels_count; c++){
		for (ei = 0; ei < channels[c].old_ids->num_allocd; ei++){
			if (!channels[c].old_ids->entries[ei].data){
				continue;
			}
			Dictionary_entry_* e = Dictionary_find(channels[c].video_info_dict, 
				channels[c].old_ids->entries[ei].data, channels[c].old_ids->entries[ei].data_size);
			if (e->key){
				for (p = 0; p < pc; p++){
					static_assert(V_COUNT == 4); // and they are all strings
					B found = 0;
					for (v = 0; v < V_COUNT; v++){
						char* valstr = get_value_from_dict_entry(e, v);
						if (strstr(valstr, phrases[p])){
							found = 1;
							break;
						}
					}
					if (!found) goto _next_entry;
				}
				if (matches++ < shown_matches){
					print_video_line(c, e->key);
				}
			}
			_next_entry: continue;
		}
	}
	if ((COLOR^=1)){
		printf(COLOR_ADD);
	}
	int cols = 0;
	if (!Options.verbose && matches >= shown_matches){
		cols = printf("%d matches found. %d hidden. Use -v to show all", matches, matches-shown_matches);
	}else{
		cols = printf("%d matches found.", matches);
	}
	if (COLOR){
		ws = ws_get();
		for (; cols < ws.ws_col; cols++){
			printf(" ");
		}
		printf(COLOR_REMOVE);
	}
	printf("\n");
}




int main(int argc, char** argv){
	setlocale(LC_ALL, "");
	int actions = 0;
	char invalid_arg = 0;
	if (argc >= 2){
		for (int c = 0; argv[1][c]; c++){
			switch (argv[1][c]){
				case '-':
					break;
				case 'o':
					Options.old = 1;
					break;
				case 'h':
					Options.help = 1;
				case 'v':
					Options.verbose = 1;
					break;
				case 'u':
					Options.update = 1;
					break;
				case 'l':
					Options.list = 1;
					break;
				case 'a': actions++; Options.action = 'a';
					break;
				case 'd': actions++; Options.action = 'd';
					break;
				case 'w': actions++; Options.action = 'w';
					break;
				case 'k': actions++; Options.action = 'k';
					break;
				case 'm': actions++; Options.action = 'm';
					break;
				case 'p': actions++; Options.action = 'p';
					break;
				case 'f': actions++; Options.action = 'f';
					break;
				default:
					Options.help = 1;
					invalid_arg = argv[1][c];
					break;
			}
		}
	}
	const char* xdg_config_dir_path = getenv("XDG_CONFIG_HOME");
	if (!xdg_config_dir_path){
		printf("XDG_CONFIG_HOME not defined!");
		xdg_config_dir_path = "~";
	}
	Paths.config_dir = Malloc(strlen(xdg_config_dir_path)+4+strlen(PROGRAM_NAME));
	sprintf(Paths.config_dir, "%s/%s", xdg_config_dir_path, PROGRAM_NAME);
	if (Options.verbose) printf("config dir: %s\n", Paths.config_dir);
	if (mkdir(Paths.config_dir, 0777)){
		if (errno != EEXIST){
			perror("mkdir");
			die();
		}else{
			DIR* d = opendir(Paths.config_dir);
			if (!d){
				perror("opendir");
				die();
			}
			closedir(d);
		}
	}
	if (access(Paths.config_dir, R_OK|W_OK|X_OK)){
		perror("access");
		die();
	}
	size_t path_config_dir_len = strlen(Paths.config_dir);
	Paths.config_file = Malloc(path_config_dir_len+16);
	sprintf(Paths.config_file, "%s/config.txt", Paths.config_dir);
	// write default if it doesn't exist
	if (access(Paths.config_file, R_OK|W_OK)){
		if (access(Paths.config_file, F_OK)){
			write_default_config();
		}else{
			perror("access");
			die();
		}
	}
	
	Paths.channels_file = Catpath(Paths.config_dir, "channels.tsv");
	Paths.database_dir = Catpath(Paths.config_dir, "database");	
	
	if (mkdir(Paths.database_dir, 0777)){
		if (errno != EEXIST){
			perror("mkdir");
			die();
		}else{
			DIR* d = opendir(Paths.database_dir);
			if (!d){
				perror("opendir");
				die();
			}
			closedir(d);
		}
	}
	
	B error = 0;
	if (Options.help || argc < 2 || argv[1][0] != '-'){
		printf(help_str, Paths.channels_file);
		if (argc < 2 || argv[1][0] != '-'){
			goto _the_end;
		}
		error = 1;
	}
	if (invalid_arg){
		fprintf(stderr, "Error: Invalid argument: \"%c\"\n", invalid_arg);
		error = 1;
	}
	if (actions > 1){
		fprintf(stderr, "Error: Too many actions in arguments\n");
		error = 1;
	}
	if (error){
		goto _the_end;
	}
	for (int c = 1; argv[1][c]; c++){
		switch (argv[1][c]){
			case 'a':
				if (argc < 3){
					fprintf(stderr, "Error: Missing argument after -a\n");
					goto _the_end;
				}
				cmd_channel_add(&argv[2], argc-2);
				break;
		}
	}
	
	config_set_defaults();
	config_load();
	channels_load();

	if (Options.update){
		set_truncated_channel_names(1);
		Update();
	}
	switch (Options.action){
		case 'd':
		case 'w':
		case 'k':
			set_truncated_channel_names(0);
			marking_action(Options.action, &argv[2], argc-2);
		break;
		case 'm':
			set_truncated_channel_names(0);
			cmd_mark_videos(&argv[2], argc-2);
		break;
		case 'p':
			cmd_print(&argv[2], argc-2);
		break;
		case 'f':
			set_truncated_channel_names(1);
			cmd_find(&argv[2], argc-2);
			Options.list = 0;
		break;
		default:
			if (Options.list){
				set_truncated_channel_names(0);
			}
		break;
	}
	for (int c = 0; c < channels_count; c++){
		if (channels[c].video_info_dict_modified){
			if (!Dictionary_save_compressed(channels[c].video_info_dict, channels[c].path_vid_info_file)){
				printf("Dictionary_save_compressed failed\n");
			}
		}
	}
	if (Options.list){
		cmd_list_videos();
	}
	_the_end:
	free(Paths.channels_file);
	free(Paths.config_file);
	free(Paths.config_dir);
	Curl_quit();
}

