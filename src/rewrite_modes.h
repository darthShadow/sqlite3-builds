#ifndef SQLITE3_BUILDS_REWRITE_MODES_H
#define SQLITE3_BUILDS_REWRITE_MODES_H

#include <stdint.h>

#define OBS_REWRITE_MODE_CATALOG(X) \
    X(PLEX_FTS, "plex", "fts+tag_type", "plex_fts_rewrite", 0) \
    X(PLEX_GUID_LIKE, "plex", "guid+like-null", "plex_fts_rewrite", 0) \
    X(PLEX_TAGGINGS, "plex", "taggings+membership", "plex_fts_rewrite", 1) \
    X(PLEX_ONDECK, "plex", "ondeck", "plex_fts_rewrite", 1) \
    X(EMBY_FTS, "emby", "fts+membership", "emby_fts_rewrite", 0) \
    X(EMBY_BROWSE, "emby", "fanout+browse", "emby_fts_rewrite", 0) \
    X(EMBY_FAVORITES, "emby", "fanout+favorites", "emby_fts_rewrite", 0) \
    X(EMBY_LINKS_SEARCH, "emby", "fanout+links_search", "emby_fts_rewrite", 0) \
    X(EMBY_PEOPLE, "emby", "fanout+people", "emby_fts_rewrite", 0) \
    X(EMBY_RESUME, "emby", "fanout+resume", "emby_fts_rewrite", 0) \
    X(EMBY_RESUME_SIMPLE, "emby", "fanout+resume_simple", "emby_fts_rewrite", 0) \
    X(EMBY_SIMILAR, "emby", "fanout+similar", "emby_fts_rewrite", 0) \
    X(EMBY_EPISODES_LATEST, "emby", "dashboard+episodes_latest", "emby_fts_rewrite", 1) \
    X(EMBY_MOVIES_LATEST, "emby", "dashboard+movies_latest", "emby_fts_rewrite", 1)

typedef int32_t obs_rewrite_mode;

enum {
    OBS_MODE_NONE = 0,
#define OBS_REWRITE_MODE_ENUM(suffix, target, mode, fn, eligible) \
    OBS_MODE_##suffix,
    OBS_REWRITE_MODE_CATALOG(OBS_REWRITE_MODE_ENUM)
#undef OBS_REWRITE_MODE_ENUM
    OBS_MODE_COUNT
};

_Static_assert(OBS_MODE_COUNT <= INT32_MAX, "rewrite mode ids fit int32_t");

#endif
