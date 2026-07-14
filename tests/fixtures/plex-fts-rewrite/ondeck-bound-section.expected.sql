SELECT grandparents_id AS id,
       originally_available_at AS originally_available_at,
       parent_index AS parent_index,
       metadata_item_views_index AS "index",
       +viewed_at AS "max(viewed_at)",
       library_section_id AS library_section_id,
       grandparents_extra_data AS extra_data
FROM (
  SELECT grandparents.id AS grandparents_id,
         metadata_item_views.originally_available_at AS originally_available_at,
         metadata_item_views.parent_index AS parent_index,
         metadata_item_views.`index` AS metadata_item_views_index,
         metadata_item_views.viewed_at AS viewed_at,
         grandparents.library_section_id AS library_section_id,
         grandparentsSettings.extra_data AS grandparents_extra_data,
         row_number() OVER (PARTITION BY grandparents.id ORDER BY metadata_item_views.viewed_at DESC, metadata_item_views.id DESC, grandparentsSettings.id DESC, metadata_item_settings.id DESC) AS dshadow_on_deck_rank
  FROM metadata_items AS grandparents
  JOIN metadata_item_views
  JOIN metadata_item_settings
  JOIN metadata_item_settings AS grandparentsSettings
  WHERE grandparents.guid=metadata_item_views.grandparent_guid
    AND metadata_item_settings.guid=metadata_item_views.guid
    AND metadata_item_views.account_id=metadata_item_settings.account_id
    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid
    AND metadata_item_views.account_id=grandparentsSettings.account_id
    AND metadata_item_views.library_section_id=?
    AND grandparents.id IN (101,101,102)
    AND metadata_item_settings.view_count>0
    AND metadata_item_views.account_id=42
) AS dshadow_on_deck_ranked
WHERE dshadow_on_deck_rank=1
ORDER BY viewed_at DESC, grandparents_id DESC;
