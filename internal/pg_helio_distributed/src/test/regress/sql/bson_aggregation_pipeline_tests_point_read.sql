SET search_path TO helio_core,helio_api,helio_api_catalog,helio_api_internal;

SET citus.next_shard_id TO 423000;
SET helio_api.next_collection_id TO 4230;
SET helio_api.next_collection_index_id TO 4230;

SELECT helio_api.insert_one('db','aggregation_find_point_read_noncoll','{"_id":"1", "int": 10, "a" : { "b" : [ "x", 1, 2.0, true ] } }', NULL);
SELECT helio_api.insert_one('db','aggregation_find_point_read_noncoll','{"_id":"2", "double": 2.0, "a" : { "b" : {"c": 3} } }', NULL);
SELECT helio_api.insert_one('db','aggregation_find_point_read_noncoll','{"_id":"3", "boolean": false, "a" : "no", "b": "yes", "c": true }', NULL);


SELECT helio_api.insert_one('agg_db','aggregation_find_point_read','{"_id":"1", "int": 10, "a" : { "b" : [ "x", 1, 2.0, true ] } }', NULL);
SELECT helio_api.insert_one('agg_db','aggregation_find_point_read','{"_id":"2", "double": 2.0, "a" : { "b" : {"c": 3} } }', NULL);
SELECT helio_api.insert_one('agg_db','aggregation_find_point_read','{"_id":"3", "boolean": false, "a" : "no", "b": "yes", "c": true }', NULL);

SELECT helio_api_internal.create_indexes_non_concurrently('agg_db', '{ "createIndexes": "aggregation_find_point_read", "indexes": [ { "key": { "$**": "text" }, "name": "my_txt" }]}', true);

-- fetch all rows
SELECT shard_key_value, object_id, document FROM helio_api.collection('agg_db', 'aggregation_find_point_read') ORDER BY object_id;

-- basic point read (colocated table) - uses fast path
SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }}');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }}');

-- turned off takes a medium path planner
set helio_api.enableFastPathPointLookupPlanner to off;
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }}');
reset helio_api.enableFastPathPointLookupPlanner;

-- same point read on non-colocated table - uses slow path
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('db', '{ "find": "aggregation_find_point_read_noncoll", "filter": { "_id": "2" }}');

-- now test point reads with various find features for fast path

-- limit
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "limit": 0 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "limit": 1 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "limit": 2 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "limit": 3 }');

-- skip
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 0 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 1 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 2 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 3 }');

-- skip + limit
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 0, "limit": 0 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 0, "limit": 1 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 1, "limit": 1 }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "skip": 2, "limit": 2 }');

-- sort
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "sort": { "_id": 1 } }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "sort": { "_id": -1 } }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "sort": { "a": -1 } }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "sort": { "a": 1 } }');

-- projection
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "projection": { "a.b": 1 } }');

-- filter
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2", "a": { "$exists": true } }, "sort": { "a": 1 } }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2", "a": { "$exists": false } }, "sort": { "a": 1 } }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2", "_id": { "$gt": 1 } }, "sort": { "a": 1 } }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2", "_id": { "$gt": 2 } }, "sort": { "a": 1 } }');
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2", "$text": { "$search": "abc" } }, "sort": { "$meta": 1 } }');

-- singleBatch
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2" }, "sort": { "a": 1 }, "singleBatch": true }');

-- batchSize
EXPLAIN (ANALYZE ON, VERBOSE ON, COSTS ON, BUFFERS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_find('agg_db', '{ "find": "aggregation_find_point_read", "filter": { "_id": "2", "_id": { "$gt": 2 } }, "sort": { "a": 1 }, "batchSize": 0 }');