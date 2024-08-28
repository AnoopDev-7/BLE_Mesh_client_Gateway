#ifndef _SDCARD_H_
#define _SDCARD_H_

#include <sys/unistd.h>

#define SD_MAX_LINE_LENGTH 255

void sd_init(void);
void sd_delete_file(const char *filename);
void sd_append_to_file(const char *filename, const char *buffer);
FILE *sd_open_file_for_read(const char *filename);
void sd_close_file(FILE *f);
esp_err_t sd_read_line_from_file(FILE *f, char *buffer, size_t size);
void sd_clear_file(const char *filename);
long sd_get_file_size(const char *filename);

#endif // _SDCARD_H_