-- One-time additive migration for databases created before health scoring.
-- HostManager intentionally fails fast until these columns exist.
ALTER TABLE server_performance
    ADD COLUMN health_score FLOAT NULL AFTER score,
    ADD COLUMN resource_score FLOAT NULL AFTER health_score,
    ADD COLUMN anomaly_score FLOAT NULL AFTER resource_score,
    ADD COLUMN anomaly_rate_5m FLOAT NULL AFTER anomaly_score,
    ADD COLUMN confidence FLOAT NULL AFTER anomaly_rate_5m,
    ADD COLUMN health_state VARCHAR(32) NULL AFTER confidence,
    ADD COLUMN health_model_state VARCHAR(16) NULL AFTER health_state,
    ADD COLUMN health_valid BOOLEAN NOT NULL DEFAULT FALSE AFTER health_model_state,
    ADD COLUMN health_top_signals TEXT NULL AFTER health_valid;

UPDATE server_performance
SET resource_score = score
WHERE resource_score IS NULL;

ALTER TABLE server_performance
    MODIFY COLUMN resource_score FLOAT NOT NULL DEFAULT 0;

-- Legacy BIGINT rows represented cumulative counters, while current Worker
-- writes events/s. Archive them before this migration if historical retention
-- is required; they cannot be converted to rates without original timestamps.
TRUNCATE TABLE server_softirq_detail;

ALTER TABLE server_softirq_detail
    MODIFY COLUMN hi FLOAT DEFAULT 0,
    MODIFY COLUMN timer FLOAT DEFAULT 0,
    MODIFY COLUMN net_tx FLOAT DEFAULT 0,
    MODIFY COLUMN net_rx FLOAT DEFAULT 0,
    MODIFY COLUMN block FLOAT DEFAULT 0,
    MODIFY COLUMN irq_poll FLOAT DEFAULT 0,
    MODIFY COLUMN tasklet FLOAT DEFAULT 0,
    MODIFY COLUMN sched FLOAT DEFAULT 0,
    MODIFY COLUMN hrtimer FLOAT DEFAULT 0,
    MODIFY COLUMN rcu FLOAT DEFAULT 0;
