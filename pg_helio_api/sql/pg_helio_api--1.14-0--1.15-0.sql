SET search_path TO helio_api;

#include "udfs/schema_mgmt/rename_collection--1.15-0.sql"

#include "udfs/aggregation/group_aggregates_support--1.15-0.sql"
#include "udfs/aggregation/group_aggregates--1.15-0.sql"

RESET search_path;
