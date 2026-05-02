-- Sbywow mercenary system: ownership table.
-- Maps each mercenary character (created on the dedicated MERCENARIES service
-- account) to the player who hired them. Cascade is managed by MercenaryMgr in
-- code, so no foreign keys.

CREATE TABLE IF NOT EXISTS `mod_sbywow_mercenaries` (
    `merc_guid`  INT UNSIGNED     NOT NULL COMMENT 'characters.guid of the mercenary bot',
    `owner_guid` INT UNSIGNED     NOT NULL COMMENT 'characters.guid of the player who hired this merc',
    `class_id`   TINYINT UNSIGNED NOT NULL COMMENT 'class of the mercenary (1=Warrior, ...)',
    `hired_at`   TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`merc_guid`),
    KEY `idx_owner` (`owner_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Sbywow mercenary ownership: player -> hired mercenary bot character';
