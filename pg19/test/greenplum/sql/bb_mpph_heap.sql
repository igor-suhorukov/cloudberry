--
-- The tables of bb_mpph's that bb_memory_quota reads -- heap_lineitem,
-- heap_orders and heap_part, made and loaded as bb_mpph makes and loads
-- them -- where bb_mpph itself does not run (manifest).
--
CREATE TABLE heap_lineitem (
    l_orderkey bigint NOT NULL,
    l_partkey integer NOT NULL,
    l_suppkey integer NOT NULL,
    l_linenumber integer NOT NULL,
    l_quantity numeric(15,2) NOT NULL,
    l_extendedprice numeric(15,2) NOT NULL,
    l_discount numeric(15,2) NOT NULL,
    l_tax numeric(15,2) NOT NULL,
    l_returnflag character(1) NOT NULL,
    l_linestatus character(1) NOT NULL,
    l_shipdate date NOT NULL,
    l_commitdate date NOT NULL,
    l_receiptdate date NOT NULL,
    l_shipinstruct character(25) NOT NULL,
    l_shipmode character(10) NOT NULL,
    l_comment character varying(44) NOT NULL
)
WITH (appendonly=false) DISTRIBUTED BY (l_orderkey);

CREATE TABLE heap_orders (
    o_orderkey bigint NOT NULL,
    o_custkey integer NOT NULL,
    o_orderstatus character(1) NOT NULL,
    o_totalprice numeric(15,2) NOT NULL,
    o_orderdate date NOT NULL,
    o_orderpriority character(15) NOT NULL,
    o_clerk character(15) NOT NULL,
    o_shippriority integer NOT NULL,
    o_comment character varying(79) NOT NULL
)
WITH (appendonly=false) DISTRIBUTED BY (o_orderkey);

CREATE TABLE heap_part (
    p_partkey integer NOT NULL,
    p_name character varying(55) NOT NULL,
    p_mfgr character(25) NOT NULL,
    p_brand character(10) NOT NULL,
    p_type character varying(25) NOT NULL,
    p_size integer NOT NULL,
    p_container character(10) NOT NULL,
    p_retailprice numeric(15,2) NOT NULL,
    p_comment character varying(23) NOT NULL
)
WITH (appendonly=false) DISTRIBUTED BY (p_partkey);

\getenv abs_srcdir PG_ABS_SRCDIR
\set lineitem_small_file :abs_srcdir '/data/lineitem_small.csv'
\set lineitem_file :abs_srcdir '/data/lineitem.csv'
\set order_small_file :abs_srcdir '/data/order_small.csv'
\set order_file :abs_srcdir '/data/order.csv'
\set part_file :abs_srcdir '/data/part.csv'

copy heap_lineitem ( L_ORDERKEY, L_PARTKEY, L_SUPPKEY,L_LINENUMBER,L_QUANTITY, L_EXTENDEDPRICE,L_DISCOUNT,L_TAX,L_RETURNFLAG,L_LINESTATUS,L_SHIPDATE,L_COMMITDATE,L_RECEIPTDATE,L_SHIPINSTRUCT,L_SHIPMODE,L_COMMENT) from :'lineitem_small_file' with delimiter '|';
copy heap_lineitem ( L_ORDERKEY, L_PARTKEY, L_SUPPKEY,L_LINENUMBER,L_QUANTITY, L_EXTENDEDPRICE,L_DISCOUNT,L_TAX,L_RETURNFLAG,L_LINESTATUS,L_SHIPDATE,L_COMMITDATE,L_RECEIPTDATE,L_SHIPINSTRUCT,L_SHIPMODE,L_COMMENT) from :'lineitem_file' with delimiter '|';
copy heap_orders ( O_ORDERKEY,O_CUSTKEY,O_ORDERSTATUS,O_TOTALPRICE,O_ORDERDATE,O_ORDERPRIORITY,O_CLERK,O_SHIPPRIORITY,O_COMMENT) from :'order_small_file' with delimiter '|';
copy heap_orders ( O_ORDERKEY,O_CUSTKEY,O_ORDERSTATUS,O_TOTALPRICE,O_ORDERDATE,O_ORDERPRIORITY,O_CLERK,O_SHIPPRIORITY,O_COMMENT) from :'order_file' with delimiter '|';
copy heap_part (P_PARTKEY,P_NAME,P_MFGR,P_BRAND,P_TYPE,P_SIZE,P_CONTAINER,P_RETAILPRICE,P_COMMENT)from :'part_file' with delimiter '|';
