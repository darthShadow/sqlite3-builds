#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

static const char *MATCH_SQL_INT =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_INT_REWRITTEN =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_LEAN =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6";
static const char *MATCH_SQL_LEAN_REWRITTEN =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)";
static const char *MATCH_SQL_QUOTED =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type='6' and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_PARAM =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=? and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_NAMED_MATCH_PARAM =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match @SearchTerm and tag_type=6";
static const char *MATCH_SQL_NUMBERED_MATCH_PARAM =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ?1 and tag_type=6";
static const char *PROJECTED_TAG_TYPE_SQL =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 order by tag_type, tags.id";
static const char *PROJECTED_TAG_TYPE_SQL_REWRITTEN =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6) order by tag_type, tags.id";
static const char *BOUNDARY_PLUS_INT_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6+1 order by tags.id";
static const char *BOUNDARY_PLUS_PARAM_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=?+1 order by tags.id";
static const char *BOUNDARY_HEX_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=0x1f order by tags.id";
static const char *NONMATCH_SQL = "select 1";
static const char *NO_FTS_SQL =
    "select tags.id from tags where tag_type=6";
static const char *NO_TARGET_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tags.id=10";
static const char *DUPLICATE_TARGET_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 and tag_type=1";
static const char *CROSS_SCOPE_CTE_SQL =
    "with matched as (select rowid from fts4_tag_titles_icu where fts4_tag_titles_icu.tag match 'Django*') select tags.id from tags where tag_type=6 and tags.id in (select rowid from matched)";
static const char *CROSS_SCOPE_SUBQUERY_SQL =
    "select tags.id from tags where tag_type=6 and exists (select 1 from fts4_tag_titles_icu where fts4_tag_titles_icu.rowid=tags.id and fts4_tag_titles_icu.tag match 'Django*')";
static const char *PROJECTION_ONLY_TAG_TYPE_EQ_SQL =
    "select (tag_type=6) from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*'";
static const char *ORDER_ONLY_TAG_TYPE_EQ_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' order by tag_type=6";
static const char *LEFT_BOUND_TAG_TYPE_EQ_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and 1 + tag_type=4";

static const char *GUID_LIKE_SQL =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE :1) LIMIT :2";
static const char *GUID_LIKE_SQL_REWRITTEN =
    "SELECT mt.`id` FROM metadata_items mt WHERE :1 IS NOT NULL AND (mt.`guid` LIKE :1) LIMIT :2";
static const char *GUID_LIKE_NEGATIVE_SQL =
    "SELECT mt.`id` FROM metadata_items mt WHERE mt.`guid` LIKE :1 LIMIT :2";

#define TAG_MEMBERSHIP_10 " AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=10)"

static const char *TAG_BROWSE_SQL =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10)  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 2 offset 0";
static const char *TAG_BROWSE_SQL_REWRITTEN =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ")  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 2 offset 0";
static const char *TAG_COUNT_SQL =
    "select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10) )";
static const char *TAG_COUNT_SQL_REWRITTEN =
    "select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ") )";
static const char *TAG_LIMIT_SQL =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10)  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 27 offset 0";
static const char *TAG_LIMIT_SQL_REWRITTEN =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ")  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 27 offset 0";
static const char *TAG_MISSING_JOIN_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.tag_id=10 left join tags on taggings.tag_id=tags.id where tags.id=10";
static const char *TAG_NESTED_JOIN_SQL =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 where tags.id=10 and exists (select 1 from taggings where taggings.metadata_item_id=metadata_items.id and taggings.tag_id=tags.id)";
static const char *TAG_JOIN_IN_STRING_SQL =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 where tags.id=10 and 'taggings.metadata_item_id=metadata_items.id taggings.tag_id=tags.id' != ''";
static const char *TAG_JOIN_IN_COMMENT_SQL =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 /* taggings.metadata_item_id=metadata_items.id and taggings.tag_id=tags.id */ where tags.id=10";
static const char *TAG_DUPLICATE_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=10 and tags.id=11";
static const char *TAG_BOUND_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=?";
static const char *TAG_NAMED_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=:tag_id";
static const char *TAG_STRING_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id='10'";
static const char *TAG_METADATA_FTS_SQL =
    "select metadata_items.id from metadata_items join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where fts4_metadata_titles_icu.title match 'Django*' and tags.id=10";
static const char *TAG_FLIPPED_JOIN_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 and tags.id=10 order by metadata_items.id";
static const char *TAG_FLIPPED_JOIN_SQL_REWRITTEN =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 " order by metadata_items.id";
static const char *TAG_OR_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 or tags.id=10)";
static const char *TAG_NOT_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and not (tags.id=10)";
static const char *TAG_VALUE_COMPARED_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and (tags.id=10)=1";
static const char *TAG_COLUMN_RHS_ONLY_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1";

#define ONDECK_HEAD \
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id="
#define ONDECK_AFTER_SECTION " and grandparents.id in ("
#define ONDECK_AFTER_THRESHOLD " and viewed_at > "
#define ONDECK_AFTER_IDS " and metadata_item_settings.view_count>0  and metadata_item_views.account_id="
#define ONDECK_AFTER_ACCOUNT " group by grandparents.id order by viewed_at desc"
#define ONDECK_IDS "101,101,102"
#define ONDECK_THRESHOLD "1500"
#define ONDECK_SQL_BODY ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT
#define ONDECK_THRESHOLD_SQL_BODY ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT

static const char *ONDECK_SQL = ONDECK_SQL_BODY;
static const char *ONDECK_PER_GUID_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=? and true and metadata_item_settings.view_count>0  and metadata_item_views.account_id=? and grandparents.guid='plex://show/648dd4c8004a8e8652751de2'  group by grandparents.id order by viewed_at desc";
static const char *ONDECK_SQL_REWRITTEN =
    "SELECT grandparents_id AS id,\n"
    "       originally_available_at AS originally_available_at,\n"
    "       parent_index AS parent_index,\n"
    "       metadata_item_views_index AS \"index\",\n"
    "       +viewed_at AS \"max(viewed_at)\",\n"
    "       library_section_id AS library_section_id,\n"
    "       grandparents_extra_data AS extra_data\n"
    "FROM (\n"
    "  SELECT grandparents.id AS grandparents_id,\n"
    "         metadata_item_views.originally_available_at AS originally_available_at,\n"
    "         metadata_item_views.parent_index AS parent_index,\n"
    "         metadata_item_views.`index` AS metadata_item_views_index,\n"
    "         metadata_item_views.viewed_at AS viewed_at,\n"
    "         grandparents.library_section_id AS library_section_id,\n"
    "         grandparentsSettings.extra_data AS grandparents_extra_data,\n"
    "         row_number() OVER (PARTITION BY grandparents.id ORDER BY metadata_item_views.viewed_at DESC, metadata_item_views.id DESC, grandparentsSettings.id DESC, metadata_item_settings.id DESC) AS dshadow_on_deck_rank\n"
    "  FROM metadata_items AS grandparents\n"
    "  JOIN metadata_item_views\n"
    "  JOIN metadata_item_settings\n"
    "  JOIN metadata_item_settings AS grandparentsSettings\n"
    "  WHERE grandparents.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_settings.guid=metadata_item_views.guid\n"
    "    AND metadata_item_views.account_id=metadata_item_settings.account_id\n"
    "    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_views.account_id=grandparentsSettings.account_id\n"
    "    AND metadata_item_views.library_section_id=2\n"
    "    AND grandparents.id IN (" ONDECK_IDS ")\n"
    "    AND metadata_item_settings.view_count>0\n"
    "    AND metadata_item_views.account_id=42\n"
    ") AS dshadow_on_deck_ranked\n"
    "WHERE dshadow_on_deck_rank=1\n"
    "ORDER BY viewed_at DESC, grandparents_id DESC;";
static const char *ONDECK_THRESHOLD_SQL = ONDECK_THRESHOLD_SQL_BODY;
static const char *ONDECK_THRESHOLD_SQL_REWRITTEN =
    "SELECT grandparents_id AS id,\n"
    "       originally_available_at AS originally_available_at,\n"
    "       parent_index AS parent_index,\n"
    "       metadata_item_views_index AS \"index\",\n"
    "       +viewed_at AS \"max(viewed_at)\",\n"
    "       library_section_id AS library_section_id,\n"
    "       grandparents_extra_data AS extra_data\n"
    "FROM (\n"
    "  SELECT grandparents.id AS grandparents_id,\n"
    "         metadata_item_views.originally_available_at AS originally_available_at,\n"
    "         metadata_item_views.parent_index AS parent_index,\n"
    "         metadata_item_views.`index` AS metadata_item_views_index,\n"
    "         metadata_item_views.viewed_at AS viewed_at,\n"
    "         grandparents.library_section_id AS library_section_id,\n"
    "         grandparentsSettings.extra_data AS grandparents_extra_data,\n"
    "         row_number() OVER (PARTITION BY grandparents.id ORDER BY metadata_item_views.viewed_at DESC, metadata_item_views.id DESC, grandparentsSettings.id DESC, metadata_item_settings.id DESC) AS dshadow_on_deck_rank\n"
    "  FROM metadata_items AS grandparents\n"
    "  JOIN metadata_item_views\n"
    "  JOIN metadata_item_settings\n"
    "  JOIN metadata_item_settings AS grandparentsSettings\n"
    "  WHERE grandparents.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_settings.guid=metadata_item_views.guid\n"
    "    AND metadata_item_views.account_id=metadata_item_settings.account_id\n"
    "    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_views.account_id=grandparentsSettings.account_id\n"
    "    AND metadata_item_views.library_section_id=2\n"
    "    AND metadata_item_views.viewed_at > " ONDECK_THRESHOLD "\n"
    "    AND metadata_item_settings.view_count>0\n"
    "    AND metadata_item_views.account_id=42\n"
    ") AS dshadow_on_deck_ranked\n"
    "WHERE dshadow_on_deck_rank=1\n"
    "ORDER BY viewed_at DESC, grandparents_id DESC;";
static const char *ONDECK_THRESHOLD_PARAM_SECTION_SQL =
    ONDECK_HEAD "?" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_PARAM_ACCOUNT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_PARAM_BOTH_SQL =
    ONDECK_HEAD "?" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_CROSS_PRODUCT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_BIND_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "?" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_NAMED_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ":threshold" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_POSITIVE_SIGN_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "+1500" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_NEGATIVE_SIGN_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "-1500" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_DECIMAL_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "1500.0" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_EXPRESSION_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "1500+0" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_OVERFLOW_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "9223372036854775808" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_NO_SELECTOR_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_THRESHOLD_WRONG_TAIL_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42 group by grandparents.id order by grandparents.id";
static const char *ONDECK_LEFT_JOIN_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid left join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_LIST_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION "101,?" ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_ACCOUNT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_BOTH_SQL =
    ONDECK_HEAD "?" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_REVERSED_SQL =
    ONDECK_HEAD "?2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?1" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_HOLE_SQL =
    ONDECK_HEAD "?2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_REUSE_FORWARD_SQL =
    ONDECK_HEAD "?1" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?1" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_REUSE_REVERSE_SQL =
    ONDECK_HEAD "?" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?1" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_LEADING_ZERO_SQL =
    ONDECK_HEAD "?01" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_NAMED_SQL =
    ONDECK_HEAD ":section" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_DUP_ACCOUNT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42 and metadata_item_views.account_id=43" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_MISSING_VIEWCOUNT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" "  and metadata_item_views.account_id=42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_MISSING_SETTINGS_GUID_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_MISSING_SETTINGS_ACCOUNT_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PROJECTION_DRIFT_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(metadata_item_views.viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;

typedef struct auth_probe {
    int unlikely_calls;
    int deny_unlikely;
} auth_probe;

typedef struct ondeck_failure_probe {
    int deny_row_number;
    int deny_sqlite_master;
    int denied_calls;
} ondeck_failure_probe;

typedef struct digest_result {
    int rows;
    uint64_t hash;
} digest_result;

static const char *g_program_path;

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#include "contract_parity.h"

static void require_int(const char *label, int got, int want) {
    if (got != want) failf("FAIL [%s]: got=%d want=%d", label, got, want);
}

static void require_str_eq(const char *label, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        failf("FAIL [%s]: got=\"%s\" want=\"%s\"", label, got ? got : "(null)", want);
    }
}

static void require_contains(const char *label, const char *got, const char *needle) {
    if (!got || !strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" missing=\"%s\"", label, got ? got : "(null)", needle);
    }
}

static void require_anonymous_parameter(const char *label, const char *got, const char *needle) {
    size_t needle_len = strlen(needle);
    const char *match = got ? strstr(got, needle) : NULL;

    if (!match) {
        failf("FAIL [%s]: got=\"%s\" missing=\"%s\"", label, got ? got : "(null)", needle);
    }
    if (needle_len == 0 || needle[needle_len - 1] != '?') {
        failf("FATAL [%s]: anonymous-parameter needle must end with '?': \"%s\"", label, needle);
    }
    if (match[needle_len] >= '0' && match[needle_len] <= '9') {
        failf("FAIL [%s]: got=\"%s\" want anonymous parameter after \"%s\"", label, got, needle);
    }
}

static void require_absent(const char *label, const char *got, const char *needle) {
    if (got && strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" unexpected=\"%s\"", label, got, needle);
    }
}

static void require_same_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) {
        failf("FAIL [%s]: text=(null)", label);
    }
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *first_match = strstr(line, first);
        const char *second_match = strstr(line, second);
        if (first_match && (!end || first_match < end) &&
            second_match && (!end || second_match < end)) {
            return;
        }
        if (!end) break;
        line = end + 1;
    }
    failf("FAIL [%s]: no line contains \"%s\" and \"%s\" in \"%s\"",
          label, first, second, text ? text : "(null)");
}

static void require_no_same_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) {
        failf("FAIL [%s]: text=(null)", label);
    }
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *first_match = strstr(line, first);
        const char *second_match = strstr(line, second);
        if (first_match && (!end || first_match < end) &&
            second_match && (!end || second_match < end)) {
            failf("FAIL [%s]: line contains \"%s\" and unexpected \"%s\" in \"%s\"",
                  label, first, second, text);
        }
        if (!end) break;
        line = end + 1;
    }
}

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    const char *p = haystack;

    if (needle_len == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static uint64_t sql_corr_key(const char *sql, size_t len) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)sql[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void require_occurrences(const char *label, const char *got, const char *needle, int want) {
    int got_count = got ? count_occurrences(got, needle) : 0;
    if (got_count != want) {
        failf("FAIL [%s]: occurrences=%d want=%d needle=\"%s\" text=\"%s\"",
              label, got_count, want, needle, got ? got : "(null)");
    }
}

static uint64_t require_shape_field(const char *label, const char *text) {
    const char *field = text ? strstr(text, "shape=") : NULL;
    const char *start;
    char *end = NULL;
    unsigned long long value;

    if (!field) failf("FAIL [%s]: missing shape field in \"%s\"", label,
                      text ? text : "(null)");
    start = field + strlen("shape=");
    errno = 0;
    value = strtoull(start, &end, 16);
    if (errno != 0 || end != start + 16 || *end != ' ' || value == 0u) {
        failf("FAIL [%s]: invalid shape field in \"%s\"", label, text);
    }
    return (uint64_t)value;
}

static char *read_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;

    if (!fp) failf("FATAL: open %s failed: %s", path, strerror(errno));
    if (fseek(fp, 0, SEEK_END) != 0) failf("FATAL: seek end %s failed", path);
    size = ftell(fp);
    if (size < 0) failf("FATAL: tell %s failed", path);
    if (fseek(fp, 0, SEEK_SET) != 0) failf("FATAL: seek start %s failed", path);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) failf("FATAL: malloc file buffer failed");
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        failf("FATAL: read %s failed", path);
    }
    if (fclose(fp) != 0) failf("FATAL: close %s failed", path);
    buf[size] = 0;
    return buf;
}

static char *read_sql_fixture_payload(const char *path) {
    char *buf = read_text_file(path);
    size_t len = strlen(buf);

    if (len == 0 || buf[len - 1] != '\n') {
        failf("FATAL: SQL fixture %s must end with one repository framing LF", path);
    }
    buf[len - 1] = 0;
    return buf;
}

static int safe_setenv(const char *name, const char *value) {
    if (setenv(name, value, 1) != 0) {
        fprintf(stderr, "setenv(%s=%s) failed: %s\n", name, value, strerror(errno));
        return 0;
    }
    return 1;
}

static int safe_unsetenv(const char *name) {
    if (unsetenv(name) != 0) {
        fprintf(stderr, "unsetenv(%s) failed: %s\n", name, strerror(errno));
        return 0;
    }
    return 1;
}

static void configure_env_all(const char *fts_value, const char *tag_value, const char *ondeck_value) {
    if (!safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1")) exit(1);
    if (!safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1")) exit(1);
    if (!safe_setenv("SQLITE3_DISABLE_OBSERVABILITY", "1")) exit(1);
    if (fts_value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", fts_value)) exit(1);
    } else {
        if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE")) exit(1);
    }
    if (tag_value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", tag_value)) exit(1);
    } else {
        if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE")) exit(1);
    }
    if (ondeck_value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", ondeck_value)) exit(1);
    } else {
        if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE")) exit(1);
    }
    if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE")) exit(1);
}

static void configure_guid_like_env(const char *value) {
    configure_env_all("1", "1", "1");
    if (value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE", value)) exit(1);
    }
}

static void configure_env(const char *rewrite_value) {
    configure_env_all(rewrite_value, "1", NULL);
}

static int configure_obs_enabled_env(void) {
    return safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1") &&
           safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "0") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE");
}

static int configure_obs_ondeck_enabled_env(void) {
    return safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1") &&
           safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE", "0") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE_SAMPLING", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "0");
}

static int configure_obs_tag_enabled_env(void) {
    return safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1") &&
           safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "0") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1");
}

static int configure_obs_guid_like_enabled_env(void) {
    return safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1") &&
           safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY") &&
           safe_unsetenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE", "0") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1");
}

static void temp_path(char *buf, size_t n, const char *basename) {
    int rc = snprintf(buf, n, "/tmp/plex-fts-rewrite-smoke-%ld/%s", (long)getpid(), basename);
    if (rc < 0 || (size_t)rc >= n) failf("FATAL: temp path too long for %s", basename);
}

static void make_temp_dir(void) {
    char dir[256];
    int rc = snprintf(dir, sizeof(dir), "/tmp/plex-fts-rewrite-smoke-%ld", (long)getpid());
    if (rc < 0 || (size_t)rc >= sizeof(dir)) failf("FATAL: temp dir too long");
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        failf("FATAL: mkdir(%s) failed: %s", dir, strerror(errno));
    }
}

static void cleanup_temp_dir(void) {
    char dir[256];
    char path[512];
    const char *names[] = {
        "com.plexapp.plugins.library.db", "library.db", "jellyfin.db", "not-target.db"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        temp_path(path, sizeof(path), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-wal", (long)getpid(), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-shm", (long)getpid(), names[i]);
        unlink(path);
    }
    snprintf(dir, sizeof(dir), "/tmp/plex-fts-rewrite-smoke-%ld", (long)getpid());
    rmdir(dir);
}

static void exec_sql(sqlite3 *db, const char *label, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: sqlite3_exec rc=%d err=%s", label, rc, err ? err : sqlite3_errmsg(db));
    }
    sqlite3_free(err);
}

static sqlite3 *open_db(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        failf("FAIL [open]: path=%s rc=%d err=%s", path, rc, db ? sqlite3_errmsg(db) : "(null)");
    }
    return db;
}

static void setup_schema(sqlite3 *db) {
    exec_sql(db, "schema",
        "CREATE TABLE metadata_items("
        "id INTEGER PRIMARY KEY,"
        "library_section_id INTEGER NOT NULL,"
        "metadata_type INTEGER NOT NULL,"
        "guid TEXT,"
        "parent_id INTEGER,"
        "title_sort TEXT,"
        "originally_available_at INTEGER"
        ");"
        "CREATE INDEX index_metadata_items_on_guid ON metadata_items(guid);"
        "CREATE TABLE tags(id INTEGER PRIMARY KEY, tag_type INTEGER NOT NULL);"
        "CREATE TABLE taggings("
        "id INTEGER PRIMARY KEY,"
        "metadata_item_id INTEGER NOT NULL,"
        "tag_id INTEGER NOT NULL,"
        "`index` INTEGER"
        ");"
        "CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag);"
        "CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(title);"
        "CREATE TABLE metadata_item_views("
        "id INTEGER PRIMARY KEY,"
        "guid TEXT NOT NULL,"
        "grandparent_guid TEXT NOT NULL,"
        "account_id INTEGER NOT NULL,"
        "library_section_id INTEGER NOT NULL,"
        "originally_available_at INTEGER,"
        "parent_index INTEGER,"
        "`index` INTEGER,"
        "viewed_at INTEGER"
        ");"
        "CREATE INDEX index_metadata_item_views_on_guid ON metadata_item_views(guid);"
        "CREATE TABLE metadata_item_settings("
        "id INTEGER PRIMARY KEY,"
        "account_id INTEGER NOT NULL,"
        "guid TEXT NOT NULL,"
        "view_count INTEGER NOT NULL,"
        "extra_data TEXT"
        ");"
        "CREATE INDEX index_metadata_item_settings_on_account_id ON metadata_item_settings(account_id);"
        "CREATE INDEX index_metadata_item_settings_on_guid ON metadata_item_settings(guid);"
        "INSERT INTO metadata_items(id,library_section_id,metadata_type,guid,parent_id,title_sort,originally_available_at) VALUES"
        "(1,1,1,'plex://item/1',NULL,'Item 1',100),"
        "(2,1,1,'plex://item/2',NULL,'Item 2',200),"
        "(3,1,1,'plex://item/3',NULL,'Item 3',300),"
        "(4,2,1,'plex://item/4',NULL,'Item 4',400),"
        "(5,1,1,'plex://item/5',NULL,'Item 5',500),"
        "(6,1,1,'plex://item/6',NULL,'Item 6',600),"
        "(101,2,2,'plex://grandparent/101',NULL,'Grandparent 101',1000),"
        "(102,2,2,'plex://grandparent/102',NULL,'Grandparent 102',1001);"
        "INSERT INTO tags VALUES(10,6),(11,6),(12,1),(13,6),(14,7),(15,31);"
        "INSERT INTO taggings(id,metadata_item_id,tag_id,`index`) VALUES"
        "(1,1,10,1),(2,2,11,2),(3,3,12,3),(4,4,13,4),(5,5,14,5),(6,6,15,6);"
        "INSERT INTO fts4_tag_titles_icu(rowid, tag) VALUES"
        "(10,'Django Alpha'),(11,'Django Beta'),(12,'Django Other'),(13,'Django Section Two'),"
        "(14,'Django Seven'),(15,'Django Hex');"
        "INSERT INTO fts4_metadata_titles_icu(rowid, title) VALUES(1,'Django Metadata');"
        "INSERT INTO metadata_item_views"
        "(id,guid,grandparent_guid,account_id,library_section_id,originally_available_at,parent_index,`index`,viewed_at) VALUES"
        "(1001,'plex://episode/1','plex://grandparent/101',42,2,101,1,1,1000),"
        "(1002,'plex://episode/2','plex://grandparent/101',42,2,102,2,2,2000),"
        "(1003,'plex://episode/3','plex://grandparent/101',42,2,103,3,3,2000),"
        "(1004,'plex://episode/4','plex://grandparent/102',42,2,104,4,4,1500),"
        "(1005,'plex://episode/5','plex://grandparent/102',2,2,105,5,5,1400);"
        "INSERT INTO metadata_item_settings(id,account_id,guid,view_count,extra_data) VALUES"
        "(201,42,'plex://episode/1',1,'episode-1'),"
        "(202,42,'plex://episode/2',1,'episode-2'),"
        "(203,42,'plex://episode/3',1,'episode-3'),"
        "(204,42,'plex://episode/4',1,'episode-4'),"
        "(205,2,'plex://episode/5',1,'episode-5'),"
        "(301,42,'plex://grandparent/101',1,'gp101-a'),"
        "(302,42,'plex://grandparent/101',1,'gp101-b'),"
        "(303,42,'plex://grandparent/102',1,'gp102'),"
        "(304,2,'plex://grandparent/102',1,'gp102-account-2');"
    );
}

static void create_tag_membership_index(sqlite3 *db) {
    exec_sql(db, "tag-membership-index",
             "CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id "
             "ON taggings(tag_id, metadata_item_id);");
}

static void create_ondeck_index(sqlite3 *db) {
    exec_sql(db, "ondeck-index",
             "CREATE INDEX idx_dshadow_metadata_item_views_account_grandparent_guid "
             "ON metadata_item_views(account_id, grandparent_guid);");
}

static sqlite3_stmt *prepare_entry(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char **tail
);
static sqlite3 *open_seeded_temp(const char *basename);

static sqlite3_stmt *contract_prepare_v2(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char **tail
) {
    return prepare_entry(db, label, sql, -1, 2, tail);
}

typedef struct ondeck_bind_values {
    int count;
    int values[3];
} ondeck_bind_values;

typedef struct guid_like_bind_values {
    const char *pattern;
    int limit;
} guid_like_bind_values;

typedef struct ondeck_contract {
    int expected_rows;
    unsigned int expected_mask;
    int account_2;
} ondeck_contract;

typedef struct plex_fts_tie_exception {
    unsigned int accepted_rows;
} plex_fts_tie_exception;

/* MATCH_SQL_INT leaves equal count(*) rows unordered. Accept only the complete
 * two-row permutation produced by the unchanged {10,11} result set. */
static int accept_plex_fts_count_tie(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    plex_fts_tie_exception *tie = (plex_fts_tie_exception *)ctx;
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;
    unsigned int bit;

    (void)label;
    if (row < 0 || row > 1 || sqlite3_column_type(vendor, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 0) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 0);
    candidate_id = sqlite3_column_int64(candidate, 0);
    if (!((vendor_id == 10 && candidate_id == 11) ||
          (vendor_id == 11 && candidate_id == 10))) {
        return 0;
    }
    bit = 1u << row;
    if ((tie->accepted_rows & bit) != 0) return 0;
    tie->accepted_rows |= bit;
    return 1;
}

static void bind_guid_like_values(
    sqlite3_stmt *stmt,
    const char *side,
    const char *label,
    void *ctx
) {
    guid_like_bind_values *binds = (guid_like_bind_values *)ctx;
    int rc;

    if (binds->pattern) {
        rc = sqlite3_bind_text(stmt, 1, binds->pattern, -1, SQLITE_STATIC);
    } else {
        rc = sqlite3_bind_null(stmt, 1);
    }
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-bind-pattern]: rc=%d", label, side, rc);
    }
    rc = sqlite3_bind_int(stmt, 2, binds->limit);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-bind-limit]: rc=%d", label, side, rc);
    }
}

static int accept_guid_like_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;

    (void)label;
    (void)row;
    (void)ctx;
    if (sqlite3_column_type(vendor, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 0) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 0);
    candidate_id = sqlite3_column_int64(candidate, 0);
    return vendor_id >= 1 && vendor_id <= 6 &&
        candidate_id >= 1 && candidate_id <= 6;
}

static void require_guid_like_legal_rows(
    sqlite3 *db,
    const char *label,
    const char *side,
    const char *sql,
    const char *want_sql,
    guid_like_bind_values *binds
) {
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, NULL);
    unsigned int mask = 0;
    int rows = 0;
    int rc;

    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    bind_guid_like_values(stmt, side, label, binds);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        unsigned int bit;
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || id < 1 || id > 6) {
            failf("FAIL [%s/%s-id]: type=%d id=%lld want=1..6", label, side,
                  sqlite3_column_type(stmt, 0), (long long)id);
        }
        bit = 1u << (unsigned int)(id - 1);
        if ((mask & bit) != 0) {
            failf("FAIL [%s/%s-duplicate]: id=%lld mask=0x%x", label, side,
                  (long long)id, mask);
        }
        mask |= bit;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/%s-step]: rc=%d err=%s", label, side, rc,
              sqlite3_errmsg(db));
    }
    require_int(label, rows, 3);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void bind_ondeck_values(
    sqlite3_stmt *stmt,
    const char *side,
    const char *label,
    void *ctx
) {
    ondeck_bind_values *binds = (ondeck_bind_values *)ctx;
    int i;
    for (i = 0; i < binds->count; i++) {
        int rc = sqlite3_bind_int(stmt, i + 1, binds->values[i]);
        if (rc != SQLITE_OK) {
            failf("FAIL [%s/%s-bind-%d]: rc=%d", label, side, i + 1, rc);
        }
    }
}

static void require_ondeck_integer_cell(
    const char *label,
    const char *side,
    sqlite3_stmt *stmt,
    int column,
    sqlite3_int64 want
) {
    int type = sqlite3_column_type(stmt, column);
    sqlite3_int64 got = sqlite3_column_int64(stmt, column);
    if (type != SQLITE_INTEGER || got != want) {
        failf("FAIL [%s/tied-%s-column-%d]: type=%d value=%lld want_type=%d want=%lld",
              label, side, column, type, (long long)got, SQLITE_INTEGER,
              (long long)want);
    }
}

static void require_ondeck_text_cell(
    const char *label,
    const char *side,
    sqlite3_stmt *stmt,
    int column,
    const char *want
) {
    int type = sqlite3_column_type(stmt, column);
    const unsigned char *got = sqlite3_column_text(stmt, column);
    if (type != SQLITE_TEXT || !got || strcmp((const char *)got, want) != 0) {
        failf("FAIL [%s/tied-%s-column-%d]: type=%d value=%s want_type=%d want=%s",
              label, side, column, type, got ? (const char *)got : "(null)",
              SQLITE_TEXT, want);
    }
}

static void require_ondeck_candidate_row(
    const char *label,
    int row,
    sqlite3_stmt *stmt,
    const ondeck_contract *contract
) {
    if (contract->account_2) {
        if (row != 0) {
            failf("FAIL [%s/candidate-row]: row=%d want=0", label, row);
        }
        require_ondeck_integer_cell(label, "candidate", stmt, 0, 102);
        require_ondeck_integer_cell(label, "candidate", stmt, 1, 105);
        require_ondeck_integer_cell(label, "candidate", stmt, 2, 5);
        require_ondeck_integer_cell(label, "candidate", stmt, 3, 5);
        require_ondeck_integer_cell(label, "candidate", stmt, 4, 1400);
        require_ondeck_integer_cell(label, "candidate", stmt, 5, 2);
        require_ondeck_text_cell(
            label, "candidate", stmt, 6, "gp102-account-2"
        );
        return;
    }
    if (row == 0) {
        require_ondeck_integer_cell(label, "candidate", stmt, 0, 101);
        require_ondeck_integer_cell(label, "candidate", stmt, 1, 103);
        require_ondeck_integer_cell(label, "candidate", stmt, 2, 3);
        require_ondeck_integer_cell(label, "candidate", stmt, 3, 3);
        require_ondeck_integer_cell(label, "candidate", stmt, 4, 2000);
        require_ondeck_integer_cell(label, "candidate", stmt, 5, 2);
        require_ondeck_text_cell(label, "candidate", stmt, 6, "gp101-b");
        return;
    }
    if (row == 1) {
        require_ondeck_integer_cell(label, "candidate", stmt, 0, 102);
        require_ondeck_integer_cell(label, "candidate", stmt, 1, 104);
        require_ondeck_integer_cell(label, "candidate", stmt, 2, 4);
        require_ondeck_integer_cell(label, "candidate", stmt, 3, 4);
        require_ondeck_integer_cell(label, "candidate", stmt, 4, 1500);
        require_ondeck_integer_cell(label, "candidate", stmt, 5, 2);
        require_ondeck_text_cell(label, "candidate", stmt, 6, "gp102");
        return;
    }
    failf("FAIL [%s/candidate-row]: row=%d want=0|1", label, row);
}

static unsigned int require_ondeck_vendor_row(
    const char *label,
    int row,
    sqlite3_stmt *stmt,
    const ondeck_contract *contract
) {
    sqlite3_int64 id = sqlite3_column_int64(stmt, 0);

    if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER) {
        failf("FAIL [%s/vendor-id]: type=%d want=%d", label,
              sqlite3_column_type(stmt, 0), SQLITE_INTEGER);
    }
    if (!contract->account_2 && contract->expected_rows == 2 &&
        id != (row == 0 ? 101 : 102)) {
        failf("FAIL [%s/vendor-order]: row=%d got=%lld want=%d", label, row,
              (long long)id, row == 0 ? 101 : 102);
    }
    if (id == 101) {
        sqlite3_int64 originally_available_at = sqlite3_column_int64(stmt, 1);
        sqlite3_int64 parent_index = sqlite3_column_int64(stmt, 2);
        sqlite3_int64 index = sqlite3_column_int64(stmt, 3);
        const unsigned char *extra_data = sqlite3_column_text(stmt, 6);
        int episode_is_2 = originally_available_at == 102 &&
            parent_index == 2 && index == 2;
        int episode_is_3 = originally_available_at == 103 &&
            parent_index == 3 && index == 3;
        int extra_is_legal = sqlite3_column_type(stmt, 6) == SQLITE_TEXT &&
            extra_data &&
            (strcmp((const char *)extra_data, "gp101-a") == 0 ||
             strcmp((const char *)extra_data, "gp101-b") == 0);

        if (sqlite3_column_type(stmt, 1) != SQLITE_INTEGER ||
            sqlite3_column_type(stmt, 2) != SQLITE_INTEGER ||
            sqlite3_column_type(stmt, 3) != SQLITE_INTEGER ||
            (!episode_is_2 && !episode_is_3) || !extra_is_legal) {
            failf("FAIL [%s/vendor-101-representative]: oa=%lld parent=%lld "
                  "index=%lld extra=%s", label,
                  (long long)originally_available_at, (long long)parent_index,
                  (long long)index,
                  extra_data ? (const char *)extra_data : "(null)");
        }
        require_ondeck_integer_cell(label, "vendor", stmt, 4, 2000);
        require_ondeck_integer_cell(label, "vendor", stmt, 5, 2);
        return 1u;
    }
    if (id == 102) {
        require_ondeck_integer_cell(
            label, "vendor", stmt, 1, contract->account_2 ? 105 : 104
        );
        require_ondeck_integer_cell(
            label, "vendor", stmt, 2, contract->account_2 ? 5 : 4
        );
        require_ondeck_integer_cell(
            label, "vendor", stmt, 3, contract->account_2 ? 5 : 4
        );
        require_ondeck_integer_cell(
            label, "vendor", stmt, 4, contract->account_2 ? 1400 : 1500
        );
        require_ondeck_integer_cell(label, "vendor", stmt, 5, 2);
        require_ondeck_text_cell(
            label, "vendor", stmt, 6,
            contract->account_2 ? "gp102-account-2" : "gp102"
        );
        return 2u;
    }
    failf("FAIL [%s/vendor-id]: got=%lld want=101|102", label, (long long)id);
    return 0;
}

static int accept_ondeck_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    ondeck_contract *contract = (ondeck_contract *)ctx;

    if (row < 0 || row >= contract->expected_rows) return 0;
    (void)require_ondeck_vendor_row(label, row, vendor, contract);
    require_ondeck_candidate_row(label, row, candidate, contract);
    return 1;
}

static void require_ondeck_result_contract(
    sqlite3 *db,
    const char *label,
    const char *side,
    const char *sql,
    const char *want_sql,
    const ondeck_bind_values *binds,
    const ondeck_contract *contract,
    int candidate
) {
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, NULL);
    unsigned int vendor_mask = 0;
    int row = 0;
    int rc;

    if (want_sql) {
        require_str_eq(label, sqlite3_sql(stmt), want_sql);
    } else if (!sqlite3_sql(stmt) || strcmp(sqlite3_sql(stmt), sql) == 0) {
        failf("FAIL [%s/%s-rewrite-fired]: candidate SQL did not change", label, side);
    }
    bind_ondeck_values(stmt, side, label, (void *)binds);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (row >= contract->expected_rows) {
            failf("FAIL [%s/%s-rows]: got>%d", label, side,
                  contract->expected_rows);
        }
        if (candidate) {
            require_ondeck_candidate_row(label, row, stmt, contract);
        } else {
            unsigned int bit = require_ondeck_vendor_row(
                label, row, stmt, contract
            );
            if ((vendor_mask & bit) != 0) {
                failf("FAIL [%s/vendor-duplicate]: mask=0x%x bit=0x%x",
                      label, vendor_mask, bit);
            }
            vendor_mask |= bit;
        }
        row++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/%s-step]: rc=%d err=%s", label, side, rc,
              sqlite3_errmsg(db));
    }
    require_int(label, row, contract->expected_rows);
    if (!candidate) {
        require_int(label, (int)vendor_mask, (int)contract->expected_mask);
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_ondeck_bound_parity(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *rewrite_needle,
    const char *expected_sql,
    int check_anonymous_parameter,
    int expected_bind_count,
    int expected_rows,
    unsigned int expected_mask,
    int account_2,
    int bind1,
    int bind2,
    int bind3
) {
    sqlite3 *original_db = open_seeded_temp("not-target.db");
    ondeck_bind_values binds = {expected_bind_count, {bind1, bind2, bind3}};
    ondeck_contract contract = {expected_rows, expected_mask, account_2};
    sqlite3_stmt *probe = NULL;

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        require_int(label, sqlite3_close(original_db), SQLITE_OK);
        return;
    }
    probe = prepare_entry(db, label, sql, -1, 2, NULL);
    if (check_anonymous_parameter) {
        require_anonymous_parameter(label, sqlite3_sql(probe), rewrite_needle);
    } else {
        require_contains(label, sqlite3_sql(probe), rewrite_needle);
    }
    require_contains(label, sqlite3_sql(probe), "dshadow_on_deck_rank");
    require_int(label, sqlite3_finalize(probe), SQLITE_OK);
    contract_parity_require(
        original_db, db, contract_prepare_v2, label, sql, sql, expected_sql,
        bind_ondeck_values, &binds,
        accept_ondeck_legal_difference, &contract
    );
    require_ondeck_result_contract(
        original_db, label, "vendor", sql, sql, &binds, &contract, 0
    );
    require_ondeck_result_contract(
        db, label, "candidate", sql, expected_sql, &binds, &contract, 1
    );
    require_int(label, sqlite3_close(original_db), SQLITE_OK);
}

static void expect_ondeck_contract(
    sqlite3 *db,
    const char *label,
    const char *source_sql,
    const char *expected_sql,
    int expected_rows,
    unsigned int expected_mask
) {
    sqlite3 *vendor_db = open_seeded_temp("not-target.db");
    ondeck_bind_values binds = {0, {0, 0, 0}};
    ondeck_contract contract = {expected_rows, expected_mask, 0};

    create_ondeck_index(vendor_db);
    contract_parity_require(
        vendor_db, db, contract_prepare_v2, label, source_sql, source_sql,
        expected_sql, NULL, NULL,
        accept_ondeck_legal_difference, &contract
    );
    require_ondeck_result_contract(
        vendor_db, label, "vendor", source_sql, source_sql, &binds,
        &contract, 0
    );
    require_ondeck_result_contract(
        db, label, "candidate", source_sql, expected_sql, &binds,
        &contract, 1
    );
    require_int(label, sqlite3_close(vendor_db), SQLITE_OK);
}

static int authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    auth_probe *probe = (auth_probe *)ctx;
    (void)db;
    (void)trigger;
    if (action == SQLITE_FUNCTION &&
        ((p1 && strcmp(p1, "unlikely") == 0) || (p2 && strcmp(p2, "unlikely") == 0))) {
        probe->unlikely_calls++;
        if (probe->deny_unlikely) return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static int ondeck_failure_authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    ondeck_failure_probe *probe = (ondeck_failure_probe *)ctx;
    (void)db;
    (void)trigger;
    if (probe->deny_row_number && action == SQLITE_FUNCTION &&
        ((p1 && strcmp(p1, "row_number") == 0) ||
         (p2 && strcmp(p2, "row_number") == 0))) {
        probe->denied_calls++;
        return SQLITE_DENY;
    }
    if (probe->deny_sqlite_master && action == SQLITE_READ && p1 &&
        (strcmp(p1, "sqlite_master") == 0 || strcmp(p1, "sqlite_schema") == 0)) {
        probe->denied_calls++;
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static sqlite3_stmt *prepare_entry(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char **tail
) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (entry == 3) {
        rc = sqlite3_prepare_v3(db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, &stmt, tail);
    } else if (entry == 2) {
        rc = sqlite3_prepare_v2(db, sql, nbyte, &stmt, tail);
    } else {
        rc = sqlite3_prepare(db, sql, nbyte, &stmt, tail);
    }
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: prepare entry=%d rc=%d err=%s", label, entry, rc, sqlite3_errmsg(db));
    }
    return stmt;
}

static void expect_saved_sql(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char *want_sql
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, entry, &tail);
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_saved_sql_contains(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *needle
) {
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, NULL);
    require_contains(label, sqlite3_sql(stmt), needle);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_ondeck_static_negative(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *unique_needle
) {
    require_occurrences(label, sql, unique_needle, 1);
    expect_saved_sql(db, label, sql, -1, 2, sql);
}

static void expect_ondeck_authorizer_fallback(
    sqlite3 *db,
    const char *label,
    int deny_row_number,
    int deny_sqlite_master
) {
    ondeck_failure_probe probe = {deny_row_number, deny_sqlite_master, 0};

    require_int(label,
                sqlite3_set_authorizer(db, ondeck_failure_authorizer_cb, &probe),
                SQLITE_OK);
    expect_saved_sql(db, label, ONDECK_THRESHOLD_SQL, -1, 2, ONDECK_THRESHOLD_SQL);
    require_int(label, sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (probe.denied_calls == 0) {
        failf("FAIL [%s]: denied_calls=0 want=>0", label);
    }
}

static void expect_rewritten_prepare_failed_skip_log(sqlite3 *db, const char *log_path) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int saved_stderr;
    int log_fd;
    int old_limit;
    int limit;
    int rc;

    if (strlen(MATCH_SQL_LEAN) + 5 > (size_t)INT_MAX) {
        failf("FATAL: skip-log SQL length exceeds int range");
    }
    limit = (int)strlen(MATCH_SQL_LEAN) + 5;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    /* Emby's over-limit lever maps to build_failed; Plex maps it to
       rewritten_prepare_failed, so this Q7 assertion depends on DIV-E keeping
       Plex wrapper SQL-length handling unchanged. */
    old_limit = sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, limit);
    rc = sqlite3_prepare_v2(db, MATCH_SQL_LEAN, -1, &stmt, &tail);
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, old_limit);
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);

    if (rc != SQLITE_OK) {
        failf("FAIL [skip-log/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    }
    require_str_eq("skip-log/sql", sqlite3_sql(stmt), MATCH_SQL_LEAN);
    if (tail != MATCH_SQL_LEAN + strlen(MATCH_SQL_LEAN)) {
        failf("FAIL [skip-log/tail]: tail_offset=%ld want=%ld",
              (long)(tail - MATCH_SQL_LEAN), (long)strlen(MATCH_SQL_LEAN));
    }
    require_int("skip-log/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_ondeck_skip_log(
    sqlite3 *db,
    const char *log_path,
    const char *label,
    const char *sql
) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int saved_stderr;
    int log_fd;
    int rc;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, &tail);
    if (rc == SQLITE_OK) {
        int step_rc;
        while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        }
        if (step_rc != SQLITE_DONE) rc = step_rc;
    }
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);

    if (rc != SQLITE_OK) {
        failf("FAIL [%s/prepare]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static char *copy_ondeck_per_guid_sql(const char *guid) {
    static const char guid_prefix[] = "plex://show/";
    size_t sql_len = strlen(ONDECK_PER_GUID_SQL);
    char *sql;
    char *slot;

    if (!guid || strlen(guid) != 24u) {
        failf("FATAL: per-GUID value must contain exactly 24 bytes");
    }
    sql = (char *)malloc(sql_len + 1u);
    if (!sql) failf("FATAL: per-GUID SQL allocation failed");
    memcpy(sql, ONDECK_PER_GUID_SQL, sql_len + 1u);
    slot = strstr(sql, guid_prefix);
    if (!slot) failf("FATAL: per-GUID prefix missing");
    slot += sizeof(guid_prefix) - 1u;
    if (slot[24] != '\'') failf("FATAL: per-GUID slot width drifted");
    memcpy(slot, guid, 24u);
    return sql;
}

static char *make_ondeck_per_guid_near_miss(void) {
    static const char tail[] = "order by viewed_at desc";
    char *sql = copy_ondeck_per_guid_sql("648dd4c8004a8e8652751de2");
    char *found = strstr(sql, tail);

    if (!found) failf("FATAL: per-GUID tail missing");
    memcpy(found + strlen("order by viewed_at "), "asc ", 4u);
    return sql;
}

static void expect_ondeck_skip_logs(
    sqlite3 *db,
    const char *log_path,
    const char *label,
    const char *const *sqls,
    size_t sql_count
) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int saved_stderr;
    int log_fd;
    int rc = SQLITE_OK;
    size_t i;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    for (i = 0; i < sql_count; i++) {
        rc = sqlite3_prepare_v2(db, sqls[i], -1, &stmt, &tail);
        if (rc != SQLITE_OK) break;
        require_str_eq(label, sqlite3_sql(stmt), sqls[i]);
        if (tail != sqls[i] + strlen(sqls[i])) {
            failf("FAIL [%s/tail]: index=%lu tail_offset=%ld want=%ld",
                  label, (unsigned long)i, (long)(tail - sqls[i]),
                  (long)strlen(sqls[i]));
        }
        require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
        stmt = NULL;
    }
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/prepare]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
}

static void expect_tag_applied_log(sqlite3 *db, const char *log_path, int count) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int saved_stderr;
    int log_fd;
    int rc;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    for (int i = 0; i < count; i++) {
        const char *input_sql =
            (i == 1 || i == 2) ? TAG_COUNT_SQL : TAG_BROWSE_SQL;
        rc = sqlite3_prepare_v2(db, input_sql, -1, &stmt, &tail);
        if (rc != SQLITE_OK) break;
        require_contains("tag-applied-log/sql", sqlite3_sql(stmt), TAG_MEMBERSHIP_10);
        if (tail != input_sql + strlen(input_sql)) {
            failf("FAIL [tag-applied-log/tail]: tail_offset=%ld want=%ld",
                  (long)(tail - input_sql), (long)strlen(input_sql));
        }
        require_int("tag-applied-log/finalize", sqlite3_finalize(stmt), SQLITE_OK);
        stmt = NULL;
    }
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);

    if (rc != SQLITE_OK) {
        failf("FAIL [tag-applied-log/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    }
}

static void expect_guid_like_applied_log(
    sqlite3 *db_a,
    sqlite3 *db_b,
    const char *log_path
) {
    sqlite3 *dbs[] = {db_a, db_b};
    int saved_stderr;
    int log_fd;
    size_t db_index;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    for (db_index = 0; db_index < sizeof(dbs) / sizeof(dbs[0]); db_index++) {
        int i;
        for (i = 0; i < 1025; i++) {
            sqlite3_stmt *stmt = NULL;
            const char *tail = NULL;
            int rc = sqlite3_prepare_v2(dbs[db_index], GUID_LIKE_SQL, -1, &stmt, &tail);
            if (rc != SQLITE_OK) {
                failf(
                    "FAIL [guid-like-applied/prepare]: db=%lu i=%d rc=%d err=%s",
                    (unsigned long)db_index, i, rc, sqlite3_errmsg(dbs[db_index])
                );
            }
            require_str_eq(
                "guid-like-applied/sql", sqlite3_sql(stmt), GUID_LIKE_SQL_REWRITTEN
            );
            if (tail != GUID_LIKE_SQL + strlen(GUID_LIKE_SQL)) {
                failf(
                    "FAIL [guid-like-applied/tail]: db=%lu i=%d got=%ld want=%ld",
                    (unsigned long)db_index, i, (long)(tail - GUID_LIKE_SQL),
                    (long)strlen(GUID_LIKE_SQL)
                );
            }
            require_int(
                "guid-like-applied/finalize", sqlite3_finalize(stmt), SQLITE_OK
            );
        }
    }
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);
}

static void expect_legacy_authorizer(sqlite3 *db, int expect_rewrite) {
    auth_probe probe = {0, 0};
    sqlite3_stmt *stmt = NULL;

    require_int("legacy-authorizer/set", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    stmt = prepare_entry(db, "legacy-authorizer", MATCH_SQL_INT, -1, 1, NULL);
    require_int("legacy-authorizer/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    require_int("legacy-authorizer/clear", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (expect_rewrite && probe.unlikely_calls < 1) {
        failf("FAIL [legacy-authorizer]: expected unlikely authorizer call, got=%d", probe.unlikely_calls);
    }
    if (!expect_rewrite && probe.unlikely_calls != 0) {
        failf("FAIL [legacy-authorizer]: unexpected unlikely authorizer calls=%d", probe.unlikely_calls);
    }
}

static void expect_tail_null_ok(sqlite3 *db, const char *label, const char *want_sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, MATCH_SQL_INT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [%s]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_clean_original_single_id(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int bind_param,
    int bind_value,
    int want_id
) {
    auth_probe probe = {0, 1};
    sqlite3_stmt *stmt = NULL;
    int rc;
    int got_id;

    require_int("clean-original/set-authorizer", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    require_int("clean-original/clear-authorizer", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (rc != SQLITE_OK) failf("FAIL [%s]: prepare rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (probe.unlikely_calls != 0) {
        failf("FAIL [%s]: rewrite attempt count=%d want=0", label, probe.unlikely_calls);
    }
    if (bind_param) {
        require_int("clean-original/bind", sqlite3_bind_int(stmt, 1, bind_value), SQLITE_OK);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        failf("FAIL [%s]: first step rc=%d want=SQLITE_ROW id=%d err=%s",
              label, rc, want_id, sqlite3_errmsg(db));
    }
    got_id = sqlite3_column_int(stmt, 0);
    if (got_id != want_id) failf("FAIL [%s]: got_id=%d want_id=%d", label, got_id, want_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) failf("FAIL [%s]: second step rc=%d want=SQLITE_DONE", label, rc);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void collect_first_int_ids(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int expect_membership,
    char *out,
    size_t out_n
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, 2, &tail);
    int rc;
    int rows = 0;
    size_t used = 0;

    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (expect_membership) {
        require_contains(label, sqlite3_sql(stmt), TAG_MEMBERSHIP_10);
    } else {
        require_absent(label, sqlite3_sql(stmt), TAG_MEMBERSHIP_10);
    }
    if (out_n == 0) failf("FAIL [%s]: output buffer empty", label);
    out[0] = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int n = snprintf(out + used, out_n - used, "%d,", sqlite3_column_int(stmt, 0));
        if (n < 0 || (size_t)n >= out_n - used) {
            failf("FAIL [%s]: row-id buffer too small", label);
        }
        used += (size_t)n;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    if (rows == 0) failf("FAIL [%s]: expected rows", label);
    require_int("first-int-ids/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static uint64_t fnv1a_u64(uint64_t h, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (unsigned char)(v & 0xffu);
        h *= 1099511628211ULL;
        v >>= 8;
    }
    return h;
}

static digest_result digest_grouped(sqlite3 *db, const char *sql, int bind_mode) {
    sqlite3_stmt *stmt = NULL;
    digest_result result = {0, 1469598103934665603ULL};
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [digest/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    sqlite3_bind_text(stmt, 1, "Django*", -1, SQLITE_STATIC);
    if (bind_mode == 0) sqlite3_bind_int(stmt, 2, 6);
    else if (bind_mode == 1) sqlite3_bind_text(stmt, 2, "6", -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, 1);
    sqlite3_bind_int(stmt, 4, 1);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.rows++;
        result.hash = fnv1a_u64(result.hash, (uint64_t)sqlite3_column_int64(stmt, 0));
        result.hash = fnv1a_u64(result.hash, (uint64_t)sqlite3_column_int64(stmt, 1));
    }
    if (rc != SQLITE_DONE) failf("FAIL [digest/step]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    require_int("digest/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return result;
}

static void expect_grouped_digest_identity(sqlite3 *db) {
    static const char *original =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and tag_type=? and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    static const char *rewritten =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and unlikely(tag_type=?) and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    int mode;
    for (mode = 0; mode < 3; mode++) {
        digest_result a = digest_grouped(db, original, mode);
        digest_result b = digest_grouped(db, rewritten, mode);
        if (a.rows != b.rows || a.hash != b.hash) {
            failf("FAIL [grouped-digest mode=%d]: original rows=%d hash=%llu rewritten rows=%d hash=%llu",
                  mode, a.rows, (unsigned long long)a.hash, b.rows, (unsigned long long)b.hash);
        }
    }
}

static void expect_fail_open(sqlite3 *db, int expect_rewrite_attempt) {
    auth_probe probe = {0, 1};
    sqlite3_stmt *stmt = NULL;
    int rc;

    require_int("fail-open/set-authorizer", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    rc = sqlite3_prepare_v2(db, MATCH_SQL_INT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [fail-open]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    require_str_eq("fail-open/sql", sqlite3_sql(stmt), MATCH_SQL_INT);
    require_int("fail-open/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    require_int("fail-open/clear-authorizer", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (expect_rewrite_attempt && probe.unlikely_calls < 1) {
        failf("FAIL [fail-open]: expected denied unlikely attempt, got=%d", probe.unlikely_calls);
    }
    if (!expect_rewrite_attempt && probe.unlikely_calls != 0) {
        failf("FAIL [fail-open]: unexpected denied unlikely attempts=%d", probe.unlikely_calls);
    }
}

static void expect_original_without_unlikely(sqlite3 *db, const char *label, const char *sql) {
    auth_probe probe = {0, 1};

    require_int("original-no-unlikely/set-authorizer",
                sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    expect_saved_sql(db, label, sql, -1, 2, sql);
    require_int("original-no-unlikely/clear-authorizer",
                sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (probe.unlikely_calls != 0) {
        failf("FAIL [%s]: rewrite attempt count=%d want=0", label, probe.unlikely_calls);
    }
}

static sqlite3 *open_seeded_temp(const char *basename) {
    char path[512];
    temp_path(path, sizeof(path), basename);
    unlink(path);
    snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-wal", (long)getpid(), basename);
    unlink(path);
    snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-shm", (long)getpid(), basename);
    unlink(path);
    temp_path(path, sizeof(path), basename);
    sqlite3 *db = open_db(path);
    setup_schema(db);
    return db;
}

static sqlite3 *open_seeded_target_in_subdir(const char *subdir) {
    char dir[512];
    char path[768];
    sqlite3 *db;
    int rc;

    temp_path(dir, sizeof(dir), subdir);
    if (mkdir(dir, 0700) != 0) {
        failf("FATAL: mkdir %s failed: %s", dir, strerror(errno));
    }
    rc = snprintf(
        path, sizeof(path), "%s/com.plexapp.plugins.library.db", dir
    );
    if (rc < 0 || (size_t)rc >= sizeof(path)) {
        failf("FATAL: target subdirectory path overflow");
    }
    db = open_db(path);
    setup_schema(db);
    return db;
}

static void cleanup_seeded_target_subdir(const char *subdir) {
    static const char *suffixes[] = {"", "-wal", "-shm"};
    char dir[512];
    char path[768];
    size_t i;

    temp_path(dir, sizeof(dir), subdir);
    for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        int rc = snprintf(
            path, sizeof(path), "%s/com.plexapp.plugins.library.db%s",
            dir, suffixes[i]
        );
        if (rc < 0 || (size_t)rc >= sizeof(path)) {
            failf("FATAL: target subdirectory cleanup path overflow");
        }
        unlink(path);
    }
    if (rmdir(dir) != 0) {
        failf("FATAL: rmdir %s failed: %s", dir, strerror(errno));
    }
}

static int child_positive(void) {
    int expect_rewrite;
    plex_fts_tie_exception fts_tie = {0};
    sqlite3 *db;
    sqlite3 *vendor_db;

    configure_env("0");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    vendor_db = open_seeded_temp("not-target.db");
    expect_saved_sql(db, "v2-int", MATCH_SQL_INT, -1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "v3-int", MATCH_SQL_INT, -1, 3,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "lean-no-metadata-group-limit", MATCH_SQL_LEAN, -1, 2,
                     expect_rewrite ? MATCH_SQL_LEAN_REWRITTEN : MATCH_SQL_LEAN);
    expect_saved_sql(db, "projected-tag-type", PROJECTED_TAG_TYPE_SQL, -1, 2,
                     expect_rewrite ? PROJECTED_TAG_TYPE_SQL_REWRITTEN : PROJECTED_TAG_TYPE_SQL);
    expect_saved_sql(db, "nbyte-positive-nul", MATCH_SQL_INT, (int)strlen(MATCH_SQL_INT) + 1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "nbyte-positive-no-nul", MATCH_SQL_INT, (int)strlen(MATCH_SQL_INT), 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_tail_null_ok(db, "pztail-null", expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    if (expect_rewrite) {
        expect_saved_sql_contains(db, "quoted", MATCH_SQL_QUOTED, "unlikely(tag_type='6')");
        expect_saved_sql_contains(db, "param", MATCH_SQL_PARAM, "unlikely(tag_type=?)");
    } else {
        expect_saved_sql(db, "quoted", MATCH_SQL_QUOTED, -1, 2, MATCH_SQL_QUOTED);
        expect_saved_sql(db, "param", MATCH_SQL_PARAM, -1, 2, MATCH_SQL_PARAM);
    }
    expect_legacy_authorizer(db, expect_rewrite);
    expect_grouped_digest_identity(db);
    if (expect_rewrite) {
        contract_parity_require(
            vendor_db, db, contract_prepare_v2, "plex-fts-contract", MATCH_SQL_INT, MATCH_SQL_INT,
            MATCH_SQL_INT_REWRITTEN, NULL, NULL,
            accept_plex_fts_count_tie, &fts_tie
        );
        if (fts_tie.accepted_rows != 0 && fts_tie.accepted_rows != 3) {
            failf("FAIL [plex-fts-contract/tied-order]: accepted=0x%x want=0x0|0x3",
                  fts_tie.accepted_rows);
        }
    }
    exec_sql(db, "exec-miss", "PRAGMA user_version;");
    require_int("positive/vendor-close", sqlite3_close(vendor_db), SQLITE_OK);
    require_int("positive/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [positive]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_env_case(const char *value, const char *label, int env_enables_rewrite) {
    int expect_rewrite;
    sqlite3 *db;
    configure_env(value);
    make_temp_dir();
    expect_rewrite = env_enables_rewrite && sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, label, MATCH_SQL_INT, -1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_legacy_authorizer(db, expect_rewrite);
    require_int("env-case/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]: expect_rewrite=%d\n", label, expect_rewrite);
    return 0;
}

static int child_path_negative(void) {
    const char *names[] = {"library.db", "jellyfin.db", "not-target.db"};
    char non_ascii_dir[512];
    char non_ascii_path[768];
    size_t i;
    configure_env("0");
    make_temp_dir();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        sqlite3 *db = open_seeded_temp(names[i]);
        expect_saved_sql(db, names[i], MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/close", sqlite3_close(db), SQLITE_OK);
    }
    {
        sqlite3 *db = open_db(":memory:");
        setup_schema(db);
        expect_saved_sql(db, "memory", MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/memory-close", sqlite3_close(db), SQLITE_OK);
    }
    {
        auth_probe probe = {0, 1};
        int rc = snprintf(non_ascii_dir, sizeof(non_ascii_dir),
                          "/tmp/plex-fts-rewrite-smoke-%ld/pl\303\251x",
                          (long)getpid());
        sqlite3 *db;
        if (rc < 0 || (size_t)rc >= sizeof(non_ascii_dir)) {
            failf("FATAL: non-ASCII temp dir too long");
        }
        if (mkdir(non_ascii_dir, 0700) != 0 && errno != EEXIST) {
            failf("FATAL: mkdir(%s) failed: %s", non_ascii_dir, strerror(errno));
        }
        rc = snprintf(non_ascii_path, sizeof(non_ascii_path),
                      "%s/com.plexapp.plugins.library.db", non_ascii_dir);
        if (rc < 0 || (size_t)rc >= sizeof(non_ascii_path)) {
            failf("FATAL: non-ASCII temp path too long");
        }
        unlink(non_ascii_path);
        db = open_db(non_ascii_path);
        setup_schema(db);
        require_int("path-negative/non-ascii-set-authorizer",
                    sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
        expect_saved_sql(db, "non-ascii-path", MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/non-ascii-clear-authorizer",
                    sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
        if (probe.unlikely_calls != 0) {
            failf("FAIL [non-ascii-path]: rewrite attempt count=%d want=0",
                  probe.unlikely_calls);
        }
        require_int("path-negative/non-ascii-close", sqlite3_close(db), SQLITE_OK);
        unlink(non_ascii_path);
        rc = snprintf(non_ascii_path, sizeof(non_ascii_path),
                      "%s/com.plexapp.plugins.library.db-wal", non_ascii_dir);
        if (rc >= 0 && (size_t)rc < sizeof(non_ascii_path)) unlink(non_ascii_path);
        rc = snprintf(non_ascii_path, sizeof(non_ascii_path),
                      "%s/com.plexapp.plugins.library.db-shm", non_ascii_dir);
        if (rc >= 0 && (size_t)rc < sizeof(non_ascii_path)) unlink(non_ascii_path);
        rmdir(non_ascii_dir);
    }
    cleanup_temp_dir();
    printf("PASS [path-negative]\n");
    return 0;
}

static int child_nonmatch(void) {
    sqlite3 *db;
    configure_env("0");
    make_temp_dir();
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, "nonmatch-select", NONMATCH_SQL, -1, 2, NONMATCH_SQL);
    expect_saved_sql(db, "no-fts-table", NO_FTS_SQL, -1, 2, NO_FTS_SQL);
    expect_saved_sql(db, "no-target-column", NO_TARGET_SQL, -1, 2, NO_TARGET_SQL);
    expect_original_without_unlikely(db, "match-named-param", MATCH_SQL_NAMED_MATCH_PARAM);
    expect_original_without_unlikely(db, "match-numbered-param", MATCH_SQL_NUMBERED_MATCH_PARAM);
    expect_saved_sql(db, "duplicate-target-column", DUPLICATE_TARGET_SQL, -1, 2,
                     DUPLICATE_TARGET_SQL);
    expect_saved_sql(db, "cross-scope-cte", CROSS_SCOPE_CTE_SQL, -1, 2, CROSS_SCOPE_CTE_SQL);
    expect_saved_sql(db, "cross-scope-subquery", CROSS_SCOPE_SUBQUERY_SQL, -1, 2,
                     CROSS_SCOPE_SUBQUERY_SQL);
    expect_saved_sql(db, "projection-only-target-column", PROJECTION_ONLY_TAG_TYPE_EQ_SQL, -1,
                     2, PROJECTION_ONLY_TAG_TYPE_EQ_SQL);
    expect_saved_sql(db, "order-only-target-column", ORDER_ONLY_TAG_TYPE_EQ_SQL, -1, 2,
                     ORDER_ONLY_TAG_TYPE_EQ_SQL);
    expect_original_without_unlikely(db, "left-bound-target-column", LEFT_BOUND_TAG_TYPE_EQ_SQL);
    expect_clean_original_single_id(db, "boundary-plus-int", BOUNDARY_PLUS_INT_SQL, 0, 0, 14);
    expect_clean_original_single_id(db, "boundary-plus-param", BOUNDARY_PLUS_PARAM_SQL, 1, 6, 14);
    expect_clean_original_single_id(db, "boundary-hex", BOUNDARY_HEX_SQL, 0, 0, 15);
    expect_fail_open(db, sqlite3_compileoption_used("ENABLE_ICU") != 0);
    require_int("nonmatch/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [nonmatch]\n");
    return 0;
}

static int child_guid_like(void) {
    guid_like_bind_values null_binds = {NULL, 3};
    guid_like_bind_values prefix_binds = {"plex://item/%", 3};
    sqlite3 *vendor_db;
    sqlite3 *candidate_db;

    configure_guid_like_env("0");
    make_temp_dir();
    vendor_db = open_seeded_temp("not-target.db");
    candidate_db = open_seeded_temp("com.plexapp.plugins.library.db");
    if (sqlite3_compileoption_used("ENABLE_ICU") != 0) {
        contract_parity_require_min_rows(
            vendor_db, candidate_db, contract_prepare_v2, "guid-like-null-contract",
            GUID_LIKE_SQL, GUID_LIKE_SQL, GUID_LIKE_SQL_REWRITTEN,
            bind_guid_like_values, &null_binds, NULL, NULL, 0
        );
        contract_parity_require(
            vendor_db, candidate_db, contract_prepare_v2, "guid-like-prefix-contract",
            GUID_LIKE_SQL, GUID_LIKE_SQL, GUID_LIKE_SQL_REWRITTEN,
            bind_guid_like_values, &prefix_binds,
            accept_guid_like_legal_difference, NULL
        );
        require_guid_like_legal_rows(
            vendor_db, "guid-like-prefix-contract", "vendor", GUID_LIKE_SQL,
            GUID_LIKE_SQL, &prefix_binds
        );
        require_guid_like_legal_rows(
            candidate_db, "guid-like-prefix-contract", "candidate", GUID_LIKE_SQL,
            GUID_LIKE_SQL_REWRITTEN, &prefix_binds
        );
        expect_saved_sql(
            candidate_db, "guid-like-shape-negative", GUID_LIKE_NEGATIVE_SQL,
            -1, 2, GUID_LIKE_NEGATIVE_SQL
        );
    }
    require_int("guid-like/vendor-close", sqlite3_close(vendor_db), SQLITE_OK);
    require_int("guid-like/candidate-close", sqlite3_close(candidate_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [guid-like]\n");
    return 0;
}

static int child_guid_like_env_case(
    const char *value,
    const char *label,
    int env_enables_rewrite
) {
    sqlite3 *db;
    int expect_rewrite;

    configure_guid_like_env(value);
    make_temp_dir();
    expect_rewrite = env_enables_rewrite &&
        sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(
        db, label, GUID_LIKE_SQL, -1, 2,
        expect_rewrite ? GUID_LIKE_SQL_REWRITTEN : GUID_LIKE_SQL
    );
    require_int(label, sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]\n", label);
    return 0;
}

static int child_tag_membership(void) {
    int expect_rewrite;
    sqlite3 *db;
    sqlite3 *missing_index_db;
    sqlite3 *non_target_db;

    configure_env_all("1", "0", "1");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(db);
    expect_saved_sql(db, "tag-browse", TAG_BROWSE_SQL, -1, 2,
                     expect_rewrite ? TAG_BROWSE_SQL_REWRITTEN : TAG_BROWSE_SQL);
    expect_saved_sql(db, "tag-count", TAG_COUNT_SQL, -1, 2,
                     expect_rewrite ? TAG_COUNT_SQL_REWRITTEN : TAG_COUNT_SQL);
    expect_saved_sql(db, "tag-limit", TAG_LIMIT_SQL, -1, 2,
                     expect_rewrite ? TAG_LIMIT_SQL_REWRITTEN : TAG_LIMIT_SQL);
    expect_saved_sql(db, "tag-flipped-join", TAG_FLIPPED_JOIN_SQL, -1, 2,
                     expect_rewrite ? TAG_FLIPPED_JOIN_SQL_REWRITTEN : TAG_FLIPPED_JOIN_SQL);
    expect_saved_sql(db, "tag-already-rewritten", TAG_BROWSE_SQL_REWRITTEN, -1, 2,
                     TAG_BROWSE_SQL_REWRITTEN);
    expect_saved_sql(db, "tag-missing-join", TAG_MISSING_JOIN_SQL, -1, 2,
                     TAG_MISSING_JOIN_SQL);
    expect_saved_sql(db, "tag-nested-join", TAG_NESTED_JOIN_SQL, -1, 2,
                     TAG_NESTED_JOIN_SQL);
    expect_saved_sql(db, "tag-join-in-string", TAG_JOIN_IN_STRING_SQL, -1, 2,
                     TAG_JOIN_IN_STRING_SQL);
    expect_saved_sql(db, "tag-join-in-comment", TAG_JOIN_IN_COMMENT_SQL, -1, 2,
                     TAG_JOIN_IN_COMMENT_SQL);
    expect_saved_sql(db, "tag-duplicate-id", TAG_DUPLICATE_ID_SQL, -1, 2,
                     TAG_DUPLICATE_ID_SQL);
    expect_saved_sql(db, "tag-bound-id", TAG_BOUND_ID_SQL, -1, 2, TAG_BOUND_ID_SQL);
    expect_saved_sql(db, "tag-named-id", TAG_NAMED_ID_SQL, -1, 2, TAG_NAMED_ID_SQL);
    expect_saved_sql(db, "tag-string-id", TAG_STRING_ID_SQL, -1, 2, TAG_STRING_ID_SQL);
    expect_saved_sql(db, "tag-or-id", TAG_OR_ID_SQL, -1, 2, TAG_OR_ID_SQL);
    expect_saved_sql(db, "tag-not-id", TAG_NOT_ID_SQL, -1, 2, TAG_NOT_ID_SQL);
    expect_saved_sql(db, "tag-value-compared-id", TAG_VALUE_COMPARED_ID_SQL, -1, 2,
                     TAG_VALUE_COMPARED_ID_SQL);
    expect_saved_sql(db, "tag-column-rhs-only", TAG_COLUMN_RHS_ONLY_SQL, -1, 2,
                     TAG_COLUMN_RHS_ONLY_SQL);
    expect_saved_sql(db, "tag-metadata-fts", TAG_METADATA_FTS_SQL, -1, 2,
                     TAG_METADATA_FTS_SQL);
    expect_saved_sql(db, "tag-unrelated-fts", MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
    require_int("tag-membership/close", sqlite3_close(db), SQLITE_OK);

    missing_index_db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(missing_index_db, "tag-missing-index", TAG_BROWSE_SQL, -1, 2,
                     TAG_BROWSE_SQL);
    require_int("tag-membership/missing-index-close",
                sqlite3_close(missing_index_db), SQLITE_OK);

    non_target_db = open_seeded_temp("library.db");
    create_tag_membership_index(non_target_db);
    expect_saved_sql(non_target_db, "tag-non-target", TAG_BROWSE_SQL, -1, 2,
                     TAG_BROWSE_SQL);
    require_int("tag-membership/non-target-close", sqlite3_close(non_target_db), SQLITE_OK);

    cleanup_temp_dir();
    printf("PASS [tag-membership]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_tag_row_parity(void) {
    sqlite3 *original_db;
    sqlite3 *rewritten_db;
    char original_ids[128];
    char rewritten_ids[128];
    int expect_rewrite;

    configure_env_all("1", "0", "1");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    original_db = open_seeded_temp("not-target.db");
    rewritten_db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(original_db);
    create_tag_membership_index(rewritten_db);
    exec_sql(original_db, "tag-row-parity-original-extra",
             "INSERT INTO taggings(id,metadata_item_id,tag_id,`index`) VALUES(7,2,10,7);");
    exec_sql(rewritten_db, "tag-row-parity-rewritten-extra",
             "INSERT INTO taggings(id,metadata_item_id,tag_id,`index`) VALUES(7,2,10,7);");

    if (expect_rewrite) {
        contract_parity_require(
            original_db, rewritten_db, contract_prepare_v2, "plex-taggings-contract",
            TAG_LIMIT_SQL, TAG_LIMIT_SQL, TAG_LIMIT_SQL_REWRITTEN,
            NULL, NULL, NULL, NULL
        );
    }

    collect_first_int_ids(original_db, "tag-row-parity-original", TAG_LIMIT_SQL, -1, 0,
                          original_ids, sizeof(original_ids));
    collect_first_int_ids(rewritten_db, "tag-row-parity-rewritten", TAG_LIMIT_SQL,
                          (int)strlen(TAG_LIMIT_SQL) + 1, expect_rewrite,
                          rewritten_ids, sizeof(rewritten_ids));
    require_str_eq("tag-row-parity/ids", rewritten_ids, original_ids);
    require_str_eq("tag-row-parity/expected", rewritten_ids, "1,2,");

    require_int("tag-row-parity/original-close", sqlite3_close(original_db), SQLITE_OK);
    require_int("tag-row-parity/rewrite-close", sqlite3_close(rewritten_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [tag-row-parity]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_tag_env_case(const char *value, const char *label, int env_enables_rewrite) {
    int expect_rewrite;
    sqlite3 *db;

    configure_env_all("1", value, "1");
    make_temp_dir();
    expect_rewrite = env_enables_rewrite && sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(db);
    expect_saved_sql(db, label, TAG_BROWSE_SQL, -1, 2,
                     expect_rewrite ? TAG_BROWSE_SQL_REWRITTEN : TAG_BROWSE_SQL);
    require_int("tag-env/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]: expect_rewrite=%d\n", label, expect_rewrite);
    return 0;
}

static int child_ondeck(void) {
    char *bound_section_sql;
    char *bound_section_expected;
    int expect_rewrite;
    sqlite3 *db;
    sqlite3 *missing_index_db;
    sqlite3 *non_target_db;

    configure_env_all("1", "1", "0");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_ondeck_index(db);
    bound_section_sql = read_sql_fixture_payload(
        "tests/fixtures/plex-fts-rewrite/ondeck-bound-section.sql"
    );
    bound_section_expected = read_sql_fixture_payload(
        "tests/fixtures/plex-fts-rewrite/ondeck-bound-section.expected.sql"
    );
    expect_saved_sql(db, "ondeck-positive", ONDECK_SQL, -1, 2,
                     expect_rewrite ? ONDECK_SQL_REWRITTEN : ONDECK_SQL);
    expect_saved_sql(db, "ondeck-variant-a-byte-identity", ONDECK_SQL, -1, 2,
                     expect_rewrite ? ONDECK_SQL_REWRITTEN : ONDECK_SQL);
    if (expect_rewrite) {
        expect_ondeck_contract(
            db, "ondeck-id-list-contract", ONDECK_SQL, ONDECK_SQL_REWRITTEN,
            2, 3
        );
    }
    expect_saved_sql(db, "ondeck-left-join", ONDECK_LEFT_JOIN_SQL, -1, 2,
                     ONDECK_LEFT_JOIN_SQL);
    expect_saved_sql(db, "ondeck-param-list", ONDECK_PARAM_LIST_SQL, -1, 2,
                     ONDECK_PARAM_LIST_SQL);
    expect_ondeck_bound_parity(db, "ondeck-param-section", bound_section_sql,
                               "library_section_id=?", bound_section_expected,
                               0, 1, 2, 3, 0, 2, 42, 0);
    expect_ondeck_bound_parity(db, "ondeck-param-account", ONDECK_PARAM_ACCOUNT_SQL,
                               "account_id=?", NULL, 1, 1, 2, 3, 0, 42, 2, 0);
    expect_ondeck_bound_parity(db, "ondeck-param-both", ONDECK_PARAM_BOTH_SQL,
                               "library_section_id=?", NULL, 1, 2, 2, 3, 0, 2, 42, 0);
    expect_ondeck_bound_parity(db, "ondeck-param-reversed", ONDECK_PARAM_REVERSED_SQL,
                               "library_section_id=?2", NULL, 0, 2, 2, 3, 0, 42, 2, 0);
    expect_ondeck_bound_parity(db, "ondeck-param-hole", ONDECK_PARAM_HOLE_SQL,
                               "account_id=?", NULL, 1, 3, 2, 3, 0, 0, 2, 42);
    expect_ondeck_bound_parity(db, "ondeck-param-reuse-forward",
                               ONDECK_PARAM_REUSE_FORWARD_SQL,
                               "account_id=?1", NULL, 0, 1, 1, 2, 1, 2, 2, 0);
    expect_ondeck_bound_parity(db, "ondeck-param-reuse-reverse",
                               ONDECK_PARAM_REUSE_REVERSE_SQL,
                               "account_id=?1", NULL, 0, 1, 1, 2, 1, 2, 2, 0);
    expect_saved_sql(db, "ondeck-param-leading-zero", ONDECK_PARAM_LEADING_ZERO_SQL, -1, 2,
                     ONDECK_PARAM_LEADING_ZERO_SQL);
    expect_saved_sql(db, "ondeck-duplicate-account", ONDECK_DUP_ACCOUNT_SQL, -1, 2,
                     ONDECK_DUP_ACCOUNT_SQL);
    expect_saved_sql(db, "ondeck-missing-viewcount", ONDECK_MISSING_VIEWCOUNT_SQL, -1, 2,
                     ONDECK_MISSING_VIEWCOUNT_SQL);
    expect_saved_sql(db, "ondeck-missing-settings-guid", ONDECK_MISSING_SETTINGS_GUID_SQL, -1, 2,
                     ONDECK_MISSING_SETTINGS_GUID_SQL);
    expect_saved_sql(db, "ondeck-missing-settings-account", ONDECK_MISSING_SETTINGS_ACCOUNT_SQL, -1, 2,
                     ONDECK_MISSING_SETTINGS_ACCOUNT_SQL);
    expect_saved_sql(db, "ondeck-projection-drift", ONDECK_PROJECTION_DRIFT_SQL, -1, 2,
                     ONDECK_PROJECTION_DRIFT_SQL);
    free(bound_section_sql);
    free(bound_section_expected);
    require_int("ondeck/close", sqlite3_close(db), SQLITE_OK);

    missing_index_db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(missing_index_db, "ondeck-missing-index", ONDECK_SQL, -1, 2,
                     ONDECK_SQL);
    require_int("ondeck/missing-index-close", sqlite3_close(missing_index_db), SQLITE_OK);

    non_target_db = open_seeded_temp("library.db");
    create_ondeck_index(non_target_db);
    expect_saved_sql(non_target_db, "ondeck-non-target", ONDECK_SQL, -1, 2,
                     ONDECK_SQL);
    require_int("ondeck/non-target-close", sqlite3_close(non_target_db), SQLITE_OK);

    cleanup_temp_dir();
    printf("PASS [ondeck]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_ondeck_threshold(void) {
    const struct {
        const char *label;
        const char *sql;
        const char *needle;
    } negatives[] = {
        {"ondeck-threshold-cross-product", ONDECK_THRESHOLD_CROSS_PRODUCT_SQL,
         ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD},
        {"ondeck-threshold-bind", ONDECK_THRESHOLD_BIND_SQL,
         ONDECK_AFTER_THRESHOLD "?"},
        {"ondeck-threshold-named", ONDECK_THRESHOLD_NAMED_SQL, ":threshold"},
        {"ondeck-threshold-positive-sign", ONDECK_THRESHOLD_POSITIVE_SIGN_SQL, "+1500"},
        {"ondeck-threshold-negative-sign", ONDECK_THRESHOLD_NEGATIVE_SIGN_SQL, "-1500"},
        {"ondeck-threshold-decimal", ONDECK_THRESHOLD_DECIMAL_SQL, "1500.0"},
        {"ondeck-threshold-expression", ONDECK_THRESHOLD_EXPRESSION_SQL, "1500+0"},
        {"ondeck-threshold-overflow", ONDECK_THRESHOLD_OVERFLOW_SQL,
         "9223372036854775808"},
        {"ondeck-threshold-no-selector", ONDECK_NO_SELECTOR_SQL, ONDECK_AFTER_IDS},
        {"ondeck-threshold-wrong-tail", ONDECK_THRESHOLD_WRONG_TAIL_SQL,
         "order by grandparents.id"}
    };
    sqlite3 *db;
    sqlite3 *missing_index_db;
    int expect_rewrite;
    size_t i;

    configure_env_all("1", "1", "0");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_ondeck_index(db);
    expect_saved_sql(db, "ondeck-threshold-exact", ONDECK_THRESHOLD_SQL, -1, 2,
                     expect_rewrite ? ONDECK_THRESHOLD_SQL_REWRITTEN : ONDECK_THRESHOLD_SQL);
    if (expect_rewrite) {
        expect_ondeck_contract(
            db, "ondeck-threshold-contract", ONDECK_THRESHOLD_SQL,
            ONDECK_THRESHOLD_SQL_REWRITTEN, 1, 1
        );
    }
    expect_ondeck_bound_parity(db, "ondeck-threshold-inline", ONDECK_THRESHOLD_SQL,
                               "metadata_item_views.viewed_at > " ONDECK_THRESHOLD,
                               NULL, 0, 0, 1, 1, 0, 0, 0, 0);
    expect_ondeck_bound_parity(db, "ondeck-threshold-param-section",
                               ONDECK_THRESHOLD_PARAM_SECTION_SQL,
                               "library_section_id=?", NULL, 1, 1, 1, 1, 0, 2, 0, 0);
    expect_ondeck_bound_parity(db, "ondeck-threshold-param-account",
                               ONDECK_THRESHOLD_PARAM_ACCOUNT_SQL,
                               "account_id=?", NULL, 1, 1, 1, 1, 0, 42, 0, 0);
    expect_ondeck_bound_parity(db, "ondeck-threshold-param-both",
                               ONDECK_THRESHOLD_PARAM_BOTH_SQL,
                               "library_section_id=?", NULL, 1, 2, 1, 1, 0, 2, 42, 0);
    for (i = 0; i < sizeof(negatives) / sizeof(negatives[0]); i++) {
        expect_ondeck_static_negative(
            db, negatives[i].label, negatives[i].sql, negatives[i].needle
        );
    }
    require_int("ondeck-threshold/close", sqlite3_close(db), SQLITE_OK);

    missing_index_db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(missing_index_db, "ondeck-threshold-missing-index",
                     ONDECK_THRESHOLD_SQL, -1, 2, ONDECK_THRESHOLD_SQL);
    require_int("ondeck-threshold/missing-index-close",
                sqlite3_close(missing_index_db), SQLITE_OK);

    cleanup_temp_dir();
    printf("PASS [ondeck-threshold]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_ondeck_threshold_fail_open(void) {
    sqlite3 *db;
    int old_limit;
    int constrained_limit;

    configure_env_all("1", "1", "0");
    make_temp_dir();
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_ondeck_index(db);

    if (strlen(ONDECK_THRESHOLD_SQL) >= (size_t)INT_MAX) {
        failf("FATAL: On-Deck threshold SQL length exceeds int range");
    }
    constrained_limit = (int)strlen(ONDECK_THRESHOLD_SQL) + 1;
    old_limit = sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, constrained_limit);
    expect_saved_sql(db, "ondeck-threshold-build-fallback", ONDECK_THRESHOLD_SQL,
                     -1, 2, ONDECK_THRESHOLD_SQL);
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, old_limit);

    if (sqlite3_compileoption_used("ENABLE_ICU") != 0) {
        expect_ondeck_authorizer_fallback(
            db, "ondeck-threshold-index-probe-fallback", 0, 1
        );
        expect_ondeck_authorizer_fallback(
            db, "ondeck-threshold-rewritten-prepare-fallback", 1, 0
        );
    }

    require_int("ondeck-threshold-fail-open/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [ondeck-threshold-fail-open]\n");
    return 0;
}

static int child_ondeck_env_case(const char *value, const char *label, int env_enables_rewrite) {
    int expect_rewrite;
    sqlite3 *db;

    configure_env_all("1", "1", value);
    make_temp_dir();
    expect_rewrite = env_enables_rewrite && sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_ondeck_index(db);
    expect_saved_sql(db, label, ONDECK_SQL, -1, 2,
                     expect_rewrite ? ONDECK_SQL_REWRITTEN : ONDECK_SQL);
    require_int("ondeck-env/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]: expect_rewrite=%d\n", label, expect_rewrite);
    return 0;
}

static int child_skip_log(void) {
    sqlite3 *db;
    char log_path[512];
    char *log_text;
    static const char *needle =
        "event=rewrite_skipped target=plex reason=rewritten_prepare_failed mode=fts+tag_type";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [skip-log]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "rewrite-skipped.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_rewritten_prepare_failed_skip_log(db, log_path);
    log_text = read_text_file(log_path);
    require_occurrences("skip-log/rewrite-skipped", log_text, needle, 1);
    free(log_text);
    require_int("skip-log/close", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [skip-log]\n");
    return 0;
}

static int child_tag_applied_log(void) {
    sqlite3 *db;
    char log_path[512];
    char *log_text;
    static const char *needle =
        "event=rewrite_applied target=plex mode=taggings+membership";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [tag-applied-log]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_tag_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "tag-applied.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(db);
    expect_tag_applied_log(db, log_path, 1025);
    log_text = read_text_file(log_path);
    require_occurrences("tag-applied-log/rewrite-applied", log_text, needle, 3);
    require_contains("tag-applied-log/first", log_text, "sample=first count=1");
    require_contains("tag-applied-log/new", log_text, "sample=new count=2");
    require_absent("tag-applied-log/repeated", log_text, "sample=new count=3");
    require_contains("tag-applied-log/periodic", log_text, "sample=periodic count=1024");
    require_contains("tag-applied-log/source", log_text, "source_sql=\"");
    require_contains("tag-applied-log/source-corr", log_text, "source_corr=");
    require_contains("tag-applied-log/rewritten-corr", log_text, " corr=");
    require_occurrences("tag-applied-log/source-full", log_text, "source_sql=\"", 3);
    free(log_text);
    require_int("tag-applied-log/close", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [tag-applied-log]\n");
    return 0;
}

static int child_guid_like_applied_sampling(void) {
    static const char *needle =
        "event=rewrite_applied target=plex mode=guid+like-null db=";
    sqlite3 *db_a;
    sqlite3 *db_b;
    char log_path[512];
    char source_field[512];
    char rewritten_field[512];
    char *log_text;
    int rc;

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [guid-like-applied-sampling]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_guid_like_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "guid-like-applied.stderr");
    unlink(log_path);
    db_a = open_seeded_target_in_subdir("guid-like-a");
    db_b = open_seeded_target_in_subdir("guid-like-b");
    expect_guid_like_applied_log(db_a, db_b, log_path);
    log_text = read_text_file(log_path);
    require_occurrences("guid-like-applied/records", log_text, needle, 4);
    require_occurrences(
        "guid-like-applied/first", log_text, "sample=first count=1", 2
    );
    require_occurrences(
        "guid-like-applied/periodic", log_text,
        "sample=periodic count=1024", 2
    );
    require_no_same_line(
        "guid-like-applied/new", log_text, needle, "sample=new"
    );
    require_no_same_line("guid-like-applied/count-2", log_text, needle, "count=2");
    require_no_same_line("guid-like-applied/count-3", log_text, needle, "count=3");
    require_no_same_line(
        "guid-like-applied/count-1025", log_text, needle, "count=1025"
    );
    rc = snprintf(
        source_field, sizeof(source_field), "source_sql=\"%s\"", GUID_LIKE_SQL
    );
    if (rc < 0 || (size_t)rc >= sizeof(source_field)) {
        failf("FATAL: guid-like source field overflow");
    }
    rc = snprintf(
        rewritten_field, sizeof(rewritten_field),
        "sql=\"%s\"", GUID_LIKE_SQL_REWRITTEN
    );
    if (rc < 0 || (size_t)rc >= sizeof(rewritten_field)) {
        failf("FATAL: guid-like rewritten field overflow");
    }
    require_occurrences(
        "guid-like-applied/source SQL", log_text, source_field, 4
    );
    require_occurrences(
        "guid-like-applied/rewritten SQL", log_text, rewritten_field, 4
    );
    free(log_text);
    require_int("guid-like-applied/close-a", sqlite3_close(db_a), SQLITE_OK);
    require_int("guid-like-applied/close-b", sqlite3_close(db_b), SQLITE_OK);
    cleanup_seeded_target_subdir("guid-like-a");
    cleanup_seeded_target_subdir("guid-like-b");
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [guid-like-applied-sampling]\n");
    return 0;
}

static int child_tag_applied_sql_suppressed(void) {
    sqlite3 *db;
    char log_path[512];
    char *log_text;
    static const char *applied_needle =
        "event=rewrite_applied target=plex mode=taggings+membership";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [tag-applied-sql-suppressed]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_tag_enabled_env() ||
        !safe_setenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL", "1")) {
        return 1;
    }
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "tag-applied-suppressed.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(db);
    expect_tag_applied_log(db, log_path, 1024);
    log_text = read_text_file(log_path);
    require_contains("tag-applied-suppressed/first", log_text, "sample=first count=1");
    require_contains("tag-applied-suppressed/new", log_text, "sample=new count=2");
    require_contains(
        "tag-applied-suppressed/periodic", log_text, "sample=periodic count=1024"
    );
    require_occurrences(
        "tag-applied-suppressed/rewrite-applied", log_text,
        applied_needle, 3
    );
    require_contains("tag-applied-suppressed/source-corr", log_text, "source_corr=");
    require_contains("tag-applied-suppressed/rewritten-corr", log_text, " corr=");
    require_absent("tag-applied-suppressed/source", log_text, " source_sql=\"");
    require_absent("tag-applied-suppressed/rewritten", log_text, " sql=\"");
    require_no_same_line(
        "tag-applied-suppressed/source", log_text, applied_needle, " source_sql=\""
    );
    require_no_same_line(
        "tag-applied-suppressed/rewritten", log_text, applied_needle, " sql=\""
    );
    free(log_text);
    require_int("tag-applied-suppressed/close", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [tag-applied-sql-suppressed]\n");
    return 0;
}

static int child_tag_index_log_dedup(void) {
    sqlite3 *db;
    ondeck_failure_probe probe = {0, 1, 0};
    char log_path[512];
    char *log_text;
    static const char *missing =
        "event=rewrite_skipped target=plex reason=index_missing mode=taggings+membership";
    static const char *probe_error =
        "event=rewrite_skipped target=plex reason=index_probe_error mode=taggings+membership";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [tag-index-log-dedup]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_tag_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "tag-index.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_ondeck_skip_log(db, log_path, "tag-index-missing-first", TAG_BROWSE_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("tag-index-missing-first", log_text, missing, 1);
    require_same_line(
        "tag-index-missing-first/sample", log_text, missing,
        "sample=first count=1"
    );
    free(log_text);
    expect_ondeck_skip_log(db, log_path, "tag-index-missing-repeat", TAG_BROWSE_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("tag-index-missing-repeat", log_text, missing, 0);
    free(log_text);
    require_int("tag-index-missing/close-first", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();

    make_temp_dir();
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_ondeck_skip_log(db, log_path, "tag-index-missing-new-connection", TAG_BROWSE_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("tag-index-missing-new-connection", log_text, missing, 0);
    free(log_text);
    require_int("tag-index-probe-error/authorizer",
                sqlite3_set_authorizer(db, ondeck_failure_authorizer_cb, &probe),
                SQLITE_OK);
    expect_ondeck_skip_log(db, log_path, "tag-index-probe-error-first", TAG_BROWSE_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("tag-index-probe-error-first", log_text, probe_error, 1);
    free(log_text);
    expect_ondeck_skip_log(db, log_path, "tag-index-probe-error-second", TAG_BROWSE_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("tag-index-probe-error-second", log_text, probe_error, 1);
    free(log_text);
    require_int("tag-index-probe-error/authorizer-clear",
                sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    require_int("tag-index-missing/close-second", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [tag-index-log-dedup]\n");
    return 0;
}

static int child_ondeck_capture_miss_log(void) {
    static const char *per_guid_values[] = {
        "648dd4c8004a8e8652751de2",
        "111111111111111111111111",
        "aaaaaaaaaaaaaaaaaaaaaaaa",
        "0123456789abcdef01234567"
    };
    const struct {
        const char *label;
        const char *sql;
        const char *sub_reason;
    } threshold_misses[] = {
        {"ondeck-threshold-cross-product-log", ONDECK_THRESHOLD_CROSS_PRODUCT_SQL, "post_id"},
        {"ondeck-threshold-bind-log", ONDECK_THRESHOLD_BIND_SQL, "threshold"},
        {"ondeck-threshold-named-log", ONDECK_THRESHOLD_NAMED_SQL, "threshold"},
        {"ondeck-threshold-positive-sign-log", ONDECK_THRESHOLD_POSITIVE_SIGN_SQL, "threshold"},
        {"ondeck-threshold-negative-sign-log", ONDECK_THRESHOLD_NEGATIVE_SIGN_SQL, "threshold"},
        {"ondeck-threshold-decimal-log", ONDECK_THRESHOLD_DECIMAL_SQL, "post_id"},
        {"ondeck-threshold-expression-log", ONDECK_THRESHOLD_EXPRESSION_SQL, "post_id"},
        {"ondeck-threshold-overflow-log", ONDECK_THRESHOLD_OVERFLOW_SQL, "threshold"},
        {"ondeck-threshold-no-selector-log", ONDECK_NO_SELECTOR_SQL, "selector"},
        {"ondeck-threshold-wrong-tail-log", ONDECK_THRESHOLD_WRONG_TAIL_SQL, "tail"}
    };
    sqlite3 *db;
    const char *per_guid_sqls[
        sizeof(per_guid_values) / sizeof(per_guid_values[0])
    ];
    char *per_guid_owned[
        sizeof(per_guid_values) / sizeof(per_guid_values[0]) - 1u
    ];
    char *per_guid_near_miss;
    char log_path[512];
    char *log_text;
    uint64_t param_shapes[4] = {0};
    size_t i;
    static const char *capture_needle =
        "event=rewrite_skipped target=plex reason=capture_miss mode=ondeck";
    static const char *out_of_scope_needle =
        "event=rewrite_skipped target=plex reason=out_of_scope mode=ondeck";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [ondeck-capture-miss-log]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_ondeck_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "ondeck-capture-miss.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");

    if (strlen(ONDECK_PER_GUID_SQL) != 1075u) {
        failf("FAIL [ondeck-per-guid/fixture-length]: got=%lu want=1075",
              (unsigned long)strlen(ONDECK_PER_GUID_SQL));
    }
    per_guid_sqls[0] = ONDECK_PER_GUID_SQL;
    for (i = 1; i < sizeof(per_guid_values) / sizeof(per_guid_values[0]); i++) {
        per_guid_owned[i - 1u] = copy_ondeck_per_guid_sql(per_guid_values[i]);
        per_guid_sqls[i] = per_guid_owned[i - 1u];
    }
    expect_ondeck_skip_logs(
        db, log_path, "ondeck-per-guid-originals", per_guid_sqls,
        sizeof(per_guid_sqls) / sizeof(per_guid_sqls[0])
    );
    log_text = read_text_file(log_path);
    require_occurrences(
        "ondeck-per-guid/out-of-scope-once", log_text, out_of_scope_needle, 1
    );
    require_same_line(
        "ondeck-per-guid/reason", log_text, out_of_scope_needle,
        "sub_reason=ondeck_per_guid"
    );
    require_same_line(
        "ondeck-per-guid/sample", log_text, out_of_scope_needle,
        "sample=first count=1"
    );
    require_same_line(
        "ondeck-per-guid/verbatim", log_text, out_of_scope_needle,
        ONDECK_PER_GUID_SQL
    );
    require_same_line(
        "ondeck-per-guid/length", log_text, out_of_scope_needle,
        "sql_len=1075"
    );
    free(log_text);
    for (i = 0; i < sizeof(per_guid_owned) / sizeof(per_guid_owned[0]); i++) {
        free(per_guid_owned[i]);
    }

    per_guid_near_miss = make_ondeck_per_guid_near_miss();
    expect_ondeck_skip_log(
        db, log_path, "ondeck-per-guid-near-miss", per_guid_near_miss
    );
    log_text = read_text_file(log_path);
    require_occurrences(
        "ondeck-per-guid-near-miss/capture", log_text, capture_needle, 1
    );
    require_same_line(
        "ondeck-per-guid-near-miss/selector", log_text, capture_needle,
        "sub_reason=selector"
    );
    require_absent(
        "ondeck-per-guid-near-miss/out-of-scope", log_text, out_of_scope_needle
    );
    free(log_text);
    free(per_guid_near_miss);

    expect_ondeck_skip_log(db, log_path, "ondeck-param-list-log", ONDECK_PARAM_LIST_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("ondeck-param-list-log/rewrite-skipped", log_text, capture_needle, 1);
    require_contains("ondeck-param-list-log/sub-reason", log_text, "sub_reason=id_list");
    free(log_text);
    expect_ondeck_skip_log(db, log_path, "ondeck-param-named-log", ONDECK_PARAM_NAMED_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("ondeck-param-named-log/rewrite-skipped", log_text, capture_needle, 1);
    require_contains("ondeck-param-named-log/sub-reason", log_text, "sub_reason=section");
    require_contains("ondeck-param-named-log/sample", log_text, "sample=new");
    param_shapes[0] = require_shape_field("ondeck-param-named-log/shape", log_text);
    free(log_text);
    expect_ondeck_skip_log(db, log_path, "ondeck-param-leading-zero-log",
                           ONDECK_PARAM_LEADING_ZERO_SQL);
    log_text = read_text_file(log_path);
    require_occurrences("ondeck-param-leading-zero-log/rewrite-skipped",
                        log_text, capture_needle, 1);
    require_contains("ondeck-param-leading-zero-log/sub-reason", log_text, "sub_reason=section");
    require_contains("ondeck-param-leading-zero-log/sample", log_text, "sample=new");
    param_shapes[1] = require_shape_field(
        "ondeck-param-leading-zero-log/shape", log_text
    );
    free(log_text);
    for (i = 0; i < sizeof(threshold_misses) / sizeof(threshold_misses[0]); i++) {
        expect_ondeck_skip_log(
            db, log_path, threshold_misses[i].label, threshold_misses[i].sql
        );
        log_text = read_text_file(log_path);
        require_occurrences(
            threshold_misses[i].label, log_text, capture_needle, 1
        );
        if (i == 1u || i == 2u) {
            require_contains(threshold_misses[i].label, log_text, "sample=new");
            param_shapes[i + 1u] = require_shape_field(
                threshold_misses[i].label, log_text
            );
        }
        {
            char metadata[160];
            int rc = snprintf(
                metadata, sizeof(metadata),
                "sub_reason=%s db=", threshold_misses[i].sub_reason
            );
            if (rc < 0 || (size_t)rc >= sizeof(metadata)) {
                failf("FATAL: capture metadata buffer overflow");
            }
            require_contains(threshold_misses[i].label, log_text, metadata);
            require_same_line(
                threshold_misses[i].label, log_text, capture_needle, metadata
            );
            rc = snprintf(
                metadata, sizeof(metadata),
                "sql_len=%lu corr=%016llx",
                (unsigned long)strlen(threshold_misses[i].sql),
                (unsigned long long)sql_corr_key(
                    threshold_misses[i].sql, strlen(threshold_misses[i].sql)
                )
            );
            if (rc < 0 || (size_t)rc >= sizeof(metadata)) {
                failf("FATAL: capture correlation buffer overflow");
            }
            require_contains(threshold_misses[i].label, log_text, metadata);
            require_same_line(
                threshold_misses[i].label, log_text, capture_needle, metadata
            );
            require_same_line(
                threshold_misses[i].label, log_text,
                "event=SQLITE_TRACE_STMT", metadata
            );
            require_contains(threshold_misses[i].label, log_text, threshold_misses[i].sql);
            require_same_line(
                threshold_misses[i].label, log_text, capture_needle, " sql=\""
            );
            require_same_line(
                threshold_misses[i].label, log_text,
                capture_needle, threshold_misses[i].sql
            );
        }
        free(log_text);
    }
    for (i = 0; i < sizeof(param_shapes) / sizeof(param_shapes[0]); i++) {
        size_t j;
        for (j = i + 1u; j < sizeof(param_shapes) / sizeof(param_shapes[0]); j++) {
            if (param_shapes[i] == param_shapes[j]) {
                failf(
                    "FAIL [ondeck-param-shapes]: index=%lu and index=%lu "
                    "share shape=%016llx",
                    (unsigned long)i, (unsigned long)j,
                    (unsigned long long)param_shapes[i]
                );
            }
        }
    }
    expect_ondeck_skip_log(db, log_path, "ondeck-early-head-miss", NONMATCH_SQL);
    log_text = read_text_file(log_path);
    require_absent("ondeck-early-head-miss", log_text, "reason=capture_miss");
    require_same_line(
        "ondeck-early-head-trace", log_text, "event=SQLITE_TRACE_STMT", " sql=\"select 1\""
    );
    require_no_same_line(
        "ondeck-early-head-source", log_text,
        "event=rewrite_skipped target=plex", " sql=\""
    );
    free(log_text);
    require_int("ondeck-capture-miss-log/close", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [ondeck-capture-miss-log]\n");
    return 0;
}

static int run_child_body(const char *name) {
    if (strcmp(name, "positive") == 0) return child_positive();
    if (strcmp(name, "env-default") == 0) return child_env_case(NULL, "env-default", 1);
    if (strcmp(name, "env-one-disabled") == 0) return child_env_case("1", "env-one-disabled", 0);
    if (strcmp(name, "env-garbage-enabled") == 0) return child_env_case("false", "env-garbage-enabled", 1);
    if (strcmp(name, "path-negative") == 0) return child_path_negative();
    if (strcmp(name, "nonmatch") == 0) return child_nonmatch();
    if (strcmp(name, "guid-like") == 0) return child_guid_like();
    if (strcmp(name, "guid-like-env-default") == 0) {
        return child_guid_like_env_case(NULL, "guid-like-env-default", 0);
    }
    if (strcmp(name, "guid-like-env-one-disabled") == 0) {
        return child_guid_like_env_case("1", "guid-like-env-one-disabled", 0);
    }
    if (strcmp(name, "guid-like-env-garbage-disabled") == 0) {
        return child_guid_like_env_case("false", "guid-like-env-garbage-disabled", 0);
    }
    if (strcmp(name, "tag-membership") == 0) return child_tag_membership();
    if (strcmp(name, "tag-row-parity") == 0) return child_tag_row_parity();
    if (strcmp(name, "tag-env-default") == 0) return child_tag_env_case(NULL, "tag-env-default", 1);
    if (strcmp(name, "tag-env-zero-enabled") == 0) return child_tag_env_case("0", "tag-env-zero-enabled", 1);
    if (strcmp(name, "tag-env-one-disabled") == 0) return child_tag_env_case("1", "tag-env-one-disabled", 0);
    if (strcmp(name, "tag-env-garbage-enabled") == 0) return child_tag_env_case("false", "tag-env-garbage-enabled", 1);
    if (strcmp(name, "ondeck") == 0) return child_ondeck();
    if (strcmp(name, "ondeck-threshold") == 0) return child_ondeck_threshold();
    if (strcmp(name, "ondeck-threshold-fail-open") == 0) {
        return child_ondeck_threshold_fail_open();
    }
    if (strcmp(name, "ondeck-env-default") == 0) return child_ondeck_env_case(NULL, "ondeck-env-default", 0);
    if (strcmp(name, "ondeck-env-one-disabled") == 0) return child_ondeck_env_case("1", "ondeck-env-one-disabled", 0);
    if (strcmp(name, "ondeck-env-garbage-disabled") == 0) return child_ondeck_env_case("false", "ondeck-env-garbage-disabled", 0);
    if (strcmp(name, "skip-log") == 0) return child_skip_log();
    if (strcmp(name, "tag-applied-log") == 0) return child_tag_applied_log();
    if (strcmp(name, "guid-like-applied-sampling") == 0) {
        return child_guid_like_applied_sampling();
    }
    if (strcmp(name, "ondeck-capture-miss-log") == 0) return child_ondeck_capture_miss_log();
    if (strcmp(name, "tag-applied-sql-suppressed") == 0) return child_tag_applied_sql_suppressed();
    if (strcmp(name, "tag-index-log-dedup") == 0) return child_tag_index_log_dedup();
    failf("FATAL: unknown child %s", name);
    return 1;
}

static void wait_for_child(pid_t pid, const char *name) {
    int status;
    if (waitpid(pid, &status, 0) < 0) failf("FATAL: waitpid(%s) failed: %s", name, strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failf("FATAL: child %s failed status=%d", name, status);
    }
}

static void run_child(const char *name) {
    pid_t pid = fork();
    if (pid < 0) failf("FATAL: fork(%s) failed: %s", name, strerror(errno));
    if (pid == 0) _exit(run_child_body(name));
    wait_for_child(pid, name);
}

static void run_exec_child(const char *name) {
    pid_t pid = fork();
    if (pid < 0) failf("FATAL: fork-exec(%s) failed: %s", name, strerror(errno));
    if (pid == 0) {
        if (strcmp(name, "ondeck-capture-miss-log") == 0) {
            if (!configure_obs_ondeck_enabled_env() ||
                !safe_unsetenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")) {
                _exit(127);
            }
        } else if (strcmp(name, "guid-like-applied-sampling") == 0) {
            if (!configure_obs_guid_like_enabled_env()) _exit(127);
        } else if (strcmp(name, "tag-applied-log") == 0 ||
                   strcmp(name, "tag-applied-sql-suppressed") == 0 ||
                   strcmp(name, "tag-index-log-dedup") == 0) {
            if (!configure_obs_tag_enabled_env()) _exit(127);
            if (strcmp(name, "tag-applied-sql-suppressed") == 0 &&
                !safe_setenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL", "1")) {
                _exit(127);
            }
            if (strcmp(name, "tag-applied-sql-suppressed") != 0 &&
                !safe_unsetenv("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")) {
                _exit(127);
            }
        } else if (!configure_obs_enabled_env()) {
            _exit(127);
        }
        execlp(g_program_path, g_program_path, "--child", name, (char *)NULL);
        fprintf(stderr, "exec(%s) failed: %s\n", g_program_path, strerror(errno));
        _exit(127);
    }
    wait_for_child(pid, name);
}

int main(int argc, char **argv) {
    g_program_path = argv[0];
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return run_child_body(argv[2]);
    }
    if (argc != 1) failf("FATAL: unexpected arguments");

    run_child("positive");
    run_child("env-default");
    run_child("env-one-disabled");
    run_child("env-garbage-enabled");
    run_child("path-negative");
    run_child("nonmatch");
    run_child("guid-like");
    run_child("guid-like-env-default");
    run_child("guid-like-env-one-disabled");
    run_child("guid-like-env-garbage-disabled");
    run_child("tag-membership");
    run_child("tag-row-parity");
    run_child("tag-env-default");
    run_child("tag-env-zero-enabled");
    run_child("tag-env-one-disabled");
    run_child("tag-env-garbage-enabled");
    run_child("ondeck");
    run_child("ondeck-threshold");
    run_child("ondeck-threshold-fail-open");
    run_child("ondeck-env-default");
    run_child("ondeck-env-one-disabled");
    run_child("ondeck-env-garbage-disabled");
    if (sqlite3_compileoption_used("ENABLE_ICU") != 0) {
        run_exec_child("skip-log");
        run_exec_child("guid-like-applied-sampling");
        run_exec_child("tag-applied-log");
        run_exec_child("tag-applied-sql-suppressed");
        run_exec_child("tag-index-log-dedup");
        run_exec_child("ondeck-capture-miss-log");
    } else {
        printf("SKIP [skip-log]: ENABLE_ICU=0\n");
        printf("SKIP [guid-like-applied-sampling]: ENABLE_ICU=0\n");
        printf("SKIP [tag-applied-log]: ENABLE_ICU=0\n");
        printf("SKIP [tag-applied-sql-suppressed]: ENABLE_ICU=0\n");
        printf("SKIP [tag-index-log-dedup]: ENABLE_ICU=0\n");
        printf("SKIP [ondeck-capture-miss-log]: ENABLE_ICU=0\n");
    }
    printf("plex fts rewrite smoke passed\n");
    return 0;
}
