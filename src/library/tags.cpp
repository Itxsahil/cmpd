#include "library/tags.h"

#include <fileref.h>
#include <tag.h>
#include <mpegfile.h>
#include <id3v2tag.h>
#include <attachedpictureframe.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <xiphcomment.h>

#include <cstring>
#include <cstdio>

int tags_read(const char *path, SongInfo *info)
{
    std::memset(info, 0, sizeof(*info));

    TagLib::FileRef f(path);
    if (f.isNull() || !f.tag())
        return -1;

    TagLib::Tag *tag = f.tag();
    if (tag->title().isEmpty())
        info->title = strdup("");
    else
        info->title = strdup(tag->title().toCString(true));

    if (tag->artist().isEmpty())
        info->artist = strdup("");
    else
        info->artist = strdup(tag->artist().toCString(true));

    if (tag->album().isEmpty())
        info->album = strdup("");
    else
        info->album = strdup(tag->album().toCString(true));

    if (tag->genre().isEmpty())
        info->genre = strdup("");
    else
        info->genre = strdup(tag->genre().toCString(true));

    info->year  = tag->year();
    info->track = tag->track();

    if (f.audioProperties())
        info->duration = f.audioProperties()->lengthInSeconds();

    /* ── cover art ── */

    /* MP3: ID3v2 APIC frame */
    {
        auto *mp3 = dynamic_cast<TagLib::MPEG::File *>(f.file());
        if (mp3 && mp3->ID3v2Tag()) {
            auto &list = mp3->ID3v2Tag()->frameListMap();
            if (list.contains("APIC")) {
                auto *pic =
                    dynamic_cast<TagLib::ID3v2::AttachedPictureFrame *>(
                        list["APIC"].front());
                if (pic && pic->picture().size() > 0) {
                    auto data = pic->picture();
                    info->cover_data = std::malloc(data.size());
                    if (info->cover_data) {
                        std::memcpy(info->cover_data, data.data(), data.size());
                        info->cover_size = data.size();
                    }
                }
            }
        }
    }

    /* FLAC: METADATA_BLOCK_PICTURE */
    if (!info->cover_data) {
        auto *flac = dynamic_cast<TagLib::FLAC::File *>(f.file());
        if (flac) {
            auto pics = flac->pictureList();
            if (!pics.isEmpty()) {
                auto data = pics.front()->data();
                info->cover_data = std::malloc(data.size());
                if (info->cover_data) {
                    std::memcpy(info->cover_data, data.data(), data.size());
                    info->cover_size = data.size();
                }
            }
        }
    }

    /* Ogg Vorbis / Opus: XiphComment METADATA_BLOCK_PICTURE */
    if (!info->cover_data) {
        /* For formats using XiphComment (Vorbis, Opus),
         * we need to get the comment from the file directly.
         * FileRef for Ogg files returns Ogg::Vorbis::File etc.,
         * which have an xiphComment() method via their base type.
         * Try FLAC::File first (already handled above), then try
         * XiphComment extraction via the complexProperties API. */
        auto *xiphFile = dynamic_cast<TagLib::Ogg::XiphComment *>(f.file());
        if (xiphFile) {
            auto pics = xiphFile->pictureList();
            if (!pics.isEmpty()) {
                auto data = pics.front()->data();
                info->cover_data = std::malloc(data.size());
                if (info->cover_data) {
                    std::memcpy(info->cover_data, data.data(), data.size());
                    info->cover_size = data.size();
                }
            }
        } else {
            /* Fallback: try complexProperties "PICTURE" */
            auto props = f.complexProperties("PICTURE");
            if (!props.isEmpty()) {
                auto &picData = props.front();
                auto iter = picData.find("data");
                if (iter != picData.end()) {
                    auto data = iter->second.toByteVector();
                    info->cover_data = std::malloc(data.size());
                    if (info->cover_data) {
                        std::memcpy(info->cover_data, data.data(), data.size());
                        info->cover_size = data.size();
                    }
                }
            }
        }
    }

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
}
