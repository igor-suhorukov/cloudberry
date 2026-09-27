/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * gp_stats_collector--1.1.sql
 *
 * Cloudberry's gpcontrib/gp_stats_collector/gp_stats_collector--1.1.sql.
 *
 * Ported to PostgreSQL 19:
 *   - the log table, gpsc.__log, is made here, which every node runs, and
 *     not by __init_log_on_master() and __init_log_on_segments(), whose
 *     heap_create_with_catalog() gave each node's table an OID of its own:
 *     here every node's has the coordinator's, and stays that node's own, the
 *     table of one of the port's modules (gp_sql's distribution.c).  Its
 *     columns are LogSchema.h's, in its order, which the collector writes
 *     by; the table of another shape is not written;
 *   - gpsc.log reads the segments' rows of gp_dist_random() alone, those of
 *     a content of 0 or more: on a node on its own, gp_dist_random() reads
 *     the node's own table, whose rows the view's first half has.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_stats_collector" to load this file. \quit

CREATE SCHEMA gpsc;

CREATE FUNCTION gpsc.__stat_messages_reset_f_on_master()
RETURNS SETOF void
AS 'MODULE_PATHNAME', 'gpsc_stat_messages_reset'
LANGUAGE C EXECUTE ON COORDINATOR;

CREATE FUNCTION gpsc.__stat_messages_reset_f_on_segments()
RETURNS SETOF void
AS 'MODULE_PATHNAME', 'gpsc_stat_messages_reset'
LANGUAGE C EXECUTE ON ALL SEGMENTS;

CREATE FUNCTION gpsc.stat_messages_reset()
RETURNS SETOF void
AS
$$
  SELECT gpsc.__stat_messages_reset_f_on_master();
  SELECT gpsc.__stat_messages_reset_f_on_segments();
$$
LANGUAGE SQL EXECUTE ON COORDINATOR;

CREATE FUNCTION gpsc.__stat_messages_f_on_master()
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gpsc_stat_messages'
LANGUAGE C STRICT VOLATILE EXECUTE ON COORDINATOR;

CREATE FUNCTION gpsc.__stat_messages_f_on_segments()
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gpsc_stat_messages'
LANGUAGE C STRICT VOLATILE EXECUTE ON ALL SEGMENTS;

CREATE VIEW gpsc.stat_messages AS
  SELECT C.*
	FROM gpsc.__stat_messages_f_on_master() as C (
    segid int,
    total_messages bigint,
    send_failures bigint,
    connection_failures bigint,
    other_errors bigint,
    max_message_size int
	)
  UNION ALL
  SELECT C.*
	FROM gpsc.__stat_messages_f_on_segments() as C (
    segid int,
    total_messages bigint,
    send_failures bigint,
    connection_failures bigint,
    other_errors bigint,
    max_message_size int
	)
ORDER BY segid;

-- The log table, each node's own: the columns of LogSchema.h's log_tbl_desc.
CREATE TABLE gpsc.__log (
  query_id                                   bigint,
  plan_id                                    bigint,
  nested_level                               bigint,
  slice_id                                   bigint,
  systemstat_vsize                           bigint,
  systemstat_rss                             bigint,
  systemstat_vmsizekb                        bigint,
  systemstat_vmpeakkb                        bigint,
  systemstat_rchar                           bigint,
  systemstat_wchar                           bigint,
  systemstat_syscr                           bigint,
  systemstat_syscw                           bigint,
  systemstat_read_bytes                      bigint,
  systemstat_write_bytes                     bigint,
  systemstat_cancelled_write_bytes           bigint,
  instrumentation_ntuples                    bigint,
  instrumentation_nloops                     bigint,
  instrumentation_tuplecount                 bigint,
  instrumentation_shared_blks_hit            bigint,
  instrumentation_shared_blks_read           bigint,
  instrumentation_shared_blks_dirtied        bigint,
  instrumentation_shared_blks_written        bigint,
  instrumentation_local_blks_hit             bigint,
  instrumentation_local_blks_read            bigint,
  instrumentation_local_blks_dirtied         bigint,
  instrumentation_local_blks_written         bigint,
  instrumentation_temp_blks_read             bigint,
  instrumentation_temp_blks_written          bigint,
  instrumentation_inherited_calls            bigint,
  instrumentation_sent_total_bytes           bigint,
  instrumentation_sent_tuple_bytes           bigint,
  instrumentation_sent_chunks                bigint,
  instrumentation_received_total_bytes       bigint,
  instrumentation_received_tuple_bytes       bigint,
  instrumentation_received_chunks            bigint,
  interconnect_total_recv_queue_size         bigint,
  interconnect_recv_queue_size_counting_time bigint,
  interconnect_total_capacity                bigint,
  interconnect_capacity_counting_time        bigint,
  interconnect_total_buffers                 bigint,
  interconnect_buffer_counting_time          bigint,
  interconnect_active_connections_num        bigint,
  interconnect_retransmits                   bigint,
  interconnect_startup_cached_pkt_num        bigint,
  interconnect_mismatch_num                  bigint,
  interconnect_crc_errors                    bigint,
  interconnect_snd_pkt_num                   bigint,
  interconnect_recv_pkt_num                  bigint,
  interconnect_disordered_pkt_num            bigint,
  interconnect_duplicated_pkt_num            bigint,
  interconnect_recv_ack_num                  bigint,
  interconnect_status_query_msg_num          bigint,
  spill_totalbytes                           bigint,
  systemstat_runningtimeseconds              double precision,
  systemstat_usertimeseconds                 double precision,
  systemstat_kerneltimeseconds               double precision,
  instrumentation_firsttuple                 double precision,
  instrumentation_startup                    double precision,
  instrumentation_total                      double precision,
  instrumentation_blk_read_time              double precision,
  instrumentation_blk_write_time             double precision,
  instrumentation_startup_time               double precision,
  instrumentation_inherited_time             double precision,
  datetime                                   timestamp with time zone,
  submit_time                                timestamp with time zone,
  start_time                                 timestamp with time zone,
  end_time                                   timestamp with time zone,
  tmid                                       integer,
  ssid                                       integer,
  ccnt                                       integer,
  dbid                                       integer,
  segid                                      integer,
  spill_filecount                            integer,
  generator                                  text,
  query_text                                 text,
  plan_text                                  text,
  template_query_text                        text,
  template_plan_text                         text,
  user_name                                  text,
  database_name                              text,
  rsgname                                    text,
  analyze_text                               text,
  error_message                              text,
  query_status                               text,
  utility                                    boolean
);

CREATE VIEW gpsc.log AS
  SELECT * FROM gpsc.__log -- master
  UNION ALL
  SELECT * FROM gp_dist_random('gpsc.__log') -- segments
  WHERE gp_segment_id >= 0
ORDER BY tmid, ssid, ccnt;

CREATE FUNCTION gpsc.__truncate_log_on_master()
RETURNS SETOF void
AS 'MODULE_PATHNAME', 'gpsc_truncate_log'
LANGUAGE C STRICT VOLATILE EXECUTE ON COORDINATOR;

CREATE FUNCTION gpsc.__truncate_log_on_segments()
RETURNS SETOF void
AS 'MODULE_PATHNAME', 'gpsc_truncate_log'
LANGUAGE C STRICT VOLATILE EXECUTE ON ALL SEGMENTS;

CREATE FUNCTION gpsc.truncate_log()
RETURNS SETOF void AS $$
BEGIN
    PERFORM gpsc.__truncate_log_on_master();
    PERFORM gpsc.__truncate_log_on_segments();
END;
$$ LANGUAGE plpgsql VOLATILE;

CREATE FUNCTION gpsc.__test_uds_start_server(path text)
RETURNS SETOF void
AS 'MODULE_PATHNAME', 'gpsc_test_uds_start_server'
LANGUAGE C STRICT EXECUTE ON COORDINATOR;

CREATE FUNCTION gpsc.__test_uds_receive(timeout_ms int DEFAULT 2000)
RETURNS SETOF bigint
AS 'MODULE_PATHNAME', 'gpsc_test_uds_receive'
LANGUAGE C STRICT EXECUTE ON COORDINATOR;

CREATE FUNCTION gpsc.__test_uds_stop_server()
RETURNS SETOF void
AS 'MODULE_PATHNAME', 'gpsc_test_uds_stop_server'
LANGUAGE C EXECUTE ON COORDINATOR;
