#ifndef TAGS_H
#define TAGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *title;
    char   *artist;
    char   *album;
    char   *genre;
    int     year;
    int     track;
    int     duration;

    /* cover art — raw encoded bytes (JPEG/PNG) */
    void   *cover_data;
    size_t  cover_size;
} SongInfo;

/* returns 0 on success */
int  tags_read(const char *path, SongInfo *info);
void tags_free(SongInfo *info);

#ifdef __cplusplus
}
#endif

#endif
