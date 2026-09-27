#ifndef WOS_TEXT_H
#define WOS_TEXT_H
#include <stddef.h>
/* Returned file storage belongs to caller (free); always NUL-terminated. */
char *text_read_file(const char *path, size_t *size);
char *text_trim(char *s);
int text_casecmp(const char *a, const char *b);
/* Destructive iterators; empty fields are preserved. */
char *text_next_line(char **cursor);
char *text_next_field(char **cursor); /* comma or tab separated */
#endif
