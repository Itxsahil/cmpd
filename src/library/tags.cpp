#include "library/tags.h"

#include <taglib.h>
#include <fileref.h>
#include <tag.h>
#include <tpropertymap.h>
#include <mpegfile.h>
#include <id3v2tag.h>
#include <attachedpictureframe.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <xiphcomment.h>
#include <vorbisfile.h>
#include <opusfile.h>
#include <mp4file.h>
#include <mp4tag.h>
#include <mp4coverart.h>

#include <cstdlib>
#include <cstring>

#define TAGLIB_AT_LEAST(maj, min) \
    (TAGLIB_MAJOR_VERSION > (maj) || \
     (TAGLIB_MAJOR_VERSION == (maj) && TAGLIB_MINOR_VERSION >= (min)))

namespace {

/* strdup() that never returns NULL for a valid input, so callers can rely on
 * the SongInfo string fields always being printable. */
char *dup_str(const char *s)
{
    if (!s) s = "";
    size_t n = std::strlen(s);
    char *p = static_cast<char *>(std::malloc(n + 1));
    if (!p) return NULL;
    std::memcpy(p, s, n + 1);
    return p;
}

char *dup_tstring(const TagLib::String &s)
{
    if (s.isEmpty()) return dup_str("");
    return dup_str(s.toCString(true));
}

/* Copy a TagLib ByteVector into the SongInfo cover slot. Returns true when a
 * non-empty picture was stored. */
bool store_cover(SongInfo *info, const TagLib::ByteVector &data)
{
    if (data.isEmpty()) return false;
    void *p = std::malloc(data.size());
    if (!p) return false;
    std::memcpy(p, data.data(), data.size());
    info->cover_data = p;
    info->cover_size = data.size();
    return true;
}

/* XiphComment carries pictures for Vorbis, Opus and FLAC-in-Ogg. */
bool cover_from_xiph(SongInfo *info, TagLib::Ogg::XiphComment *xiph)
{
    if (!xiph) return false;
    const TagLib::List<TagLib::FLAC::Picture *> pics = xiph->pictureList();
    if (pics.isEmpty()) return false;
    return store_cover(info, pics.front()->data());
}

bool cover_from_file(SongInfo *info, TagLib::File *file)
{
    if (!file) return false;

    /* MP3: ID3v2 APIC frame */
    if (auto *mp3 = dynamic_cast<TagLib::MPEG::File *>(file)) {
        if (TagLib::ID3v2::Tag *id3 = mp3->ID3v2Tag()) {
            const TagLib::ID3v2::FrameList &frames = id3->frameList("APIC");
            if (!frames.isEmpty()) {
                if (auto *pic = dynamic_cast<
                        TagLib::ID3v2::AttachedPictureFrame *>(frames.front()))
                    return store_cover(info, pic->picture());
            }
        }
        return false;
    }

    /* Native FLAC: METADATA_BLOCK_PICTURE, falling back to its Xiph comment */
    if (auto *flac = dynamic_cast<TagLib::FLAC::File *>(file)) {
        const TagLib::List<TagLib::FLAC::Picture *> pics = flac->pictureList();
        if (!pics.isEmpty() && store_cover(info, pics.front()->data()))
            return true;
        return cover_from_xiph(info, flac->xiphComment());
    }

    /* Ogg Vorbis and Opus expose the Xiph comment through tag() */
    if (auto *vorbis = dynamic_cast<TagLib::Ogg::Vorbis::File *>(file))
        return cover_from_xiph(info, vorbis->tag());
    if (auto *opus = dynamic_cast<TagLib::Ogg::Opus::File *>(file))
        return cover_from_xiph(info, opus->tag());

    /* MP4/M4A: "covr" atom */
    if (auto *mp4 = dynamic_cast<TagLib::MP4::File *>(file)) {
        TagLib::MP4::Tag *tag = mp4->tag();
        if (!tag) return false;
#if TAGLIB_AT_LEAST(1, 13)
        const TagLib::MP4::ItemMap &items = tag->itemMap();
#else
        const TagLib::MP4::ItemListMap &items = tag->itemListMap();
#endif
        if (!items.contains("covr")) return false;
        TagLib::MP4::CoverArtList covers = items["covr"].toCoverArtList();
        if (covers.isEmpty()) return false;
        return store_cover(info, covers.front().data());
    }

    return false;
}

} /* namespace */

int tags_read(const char *path, SongInfo *info)
{
    if (!path || !info) return -1;

    std::memset(info, 0, sizeof(*info));

    TagLib::FileRef f(path);
    if (f.isNull() || !f.file() || !f.file()->isValid())
        return -1;

    if (TagLib::Tag *tag = f.tag()) {
        info->title  = dup_tstring(tag->title());
        info->artist = dup_tstring(tag->artist());
        info->album  = dup_tstring(tag->album());
        info->genre  = dup_tstring(tag->genre());
        info->year   = static_cast<int>(tag->year());
        info->track  = static_cast<int>(tag->track());
    } else {
        info->title  = dup_str("");
        info->artist = dup_str("");
        info->album  = dup_str("");
        info->genre  = dup_str("");
    }

    if (!info->title || !info->artist || !info->album || !info->genre) {
        tags_free(info);
        std::memset(info, 0, sizeof(*info));
        return -1;
    }

    if (TagLib::AudioProperties *props = f.audioProperties())
#if TAGLIB_AT_LEAST(1, 9)
        info->duration = props->lengthInSeconds();
#else
        info->duration = props->length();
#endif

    cover_from_file(info, f.file());

    return 0;
}

void tags_free(SongInfo *info)
{
    if (!info) return;
    std::free(info->title);
    std::free(info->artist);
    std::free(info->album);
    std::free(info->genre);
    std::free(info->cover_data);
    std::memset(info, 0, sizeof(*info));
}
