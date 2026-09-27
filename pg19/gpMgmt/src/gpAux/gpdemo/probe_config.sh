#!/usr/bin/env bash
#
# Ported to PostgreSQL 19: the port's copy of gpAux/gpdemo/probe_config.sh
# (pg19/gpMgmt/meson.build).  It asks each node over a connection of its
# own, as Cloudberry's asks one in utility mode, which PostgreSQL 19 has not;
# and for the version as gp_core gives it, gp.version(), where Cloudberry
# reads its catalog gp_version_at_initdb, which the port has not.

#***********************************************************
# Look for Apache Cloudberry executables to find path
#***********************************************************
if [ x"$GPHOME" = x ]; then
  if [ x"$BIZHOME" = x ]; then
    echo "Neither GPHOME or BIZHOME are set.  Set one of these variables to point to the location"
	echo "of the Apache Cloudberry installation directory."
    echo ""
	exit 1
  else
    GPSEARCH=$BIZHOME
  fi
else
  GPSEARCH=$GPHOME
fi

GPPATH=`find $GPSEARCH -name gpstart | tail -1`
RETVAL=$?

if [ "$RETVAL" -ne 0 ]; then
  echo "Error attempting to find Apache Cloudberry executables in $GPSEARCH"
  exit 1
fi

if [ ! -x "$GPPATH" ]; then
  echo "No executables found for Apache Cloudberry installation in $GPSEARCH"
  exit 1
fi
GPPATH=`dirname $GPPATH`
#***********************************************************

#***********************************************************
# Create a list of catalog tables to printout
#***********************************************************
declare -a TABLES=(version)
declare -a TABLESQD=(segment_configuration pgdatabase version)

# What a node is asked of each: gp.version() for the version, a table else.
probe_sql() {
    if [ "$1" = version ]; then
        echo "select gp.version()"
    else
        echo "select * from gp_$1"
    fi
}

#***********************************************************
# Declare the max and min port numbers to probe
#***********************************************************
declare -a PORTS=(5432 10001 10002 10003)
((PORT_MIN=0))
((PORT_MAX=$NUM_PRIMARY_MIRROR_PAIRS))

PORT_BASE=$DEMO_PORT_BASE
if [ -z "$PORT_BASE" ] ; then
    echo "set PORT_BASE"
    exit 1
fi

declare -a PORTS=(`expr $PORT_BASE` \
   `expr $PORT_BASE + 2` `expr $PORT_BASE + 3 ` `expr $PORT_BASE + 4`)

#
# Check tables on Coordinator
#

#***********************************************************
# Loop over all ports and all tables, printing out their
# contents
#***********************************************************

for ((i=PORT_MIN; i<PORT_MAX+1; i++)); do
    echo "======================================================================"
    echo "Probing segment instance at port number ${PORTS[$i]}"
    echo "======================================================================"
    if [ ${i} -eq 0 ]; then
        for table in ${TABLESQD[@]}; do
            echo ""
            echo "----------------------------------------"
            echo "Table: gp_$table"
            echo "----------------------------------------"
            $GPPATH/psql --pset pager=off -p ${PORTS[$i]} -d template1 -c "$(probe_sql $table)"
            RETVAL=$?
            if [ $RETVAL -ne 0 ]; then
                echo "$0 failed."
                exit 1
            fi
        done
    else
        for table in ${TABLES[@]}; do
            echo ""
            echo "----------------------------------------"
            echo "Table: gp_$table"
            echo "----------------------------------------"
            $GPPATH/psql --pset pager=off -p ${PORTS[$i]} -d template1 -c "$(probe_sql $table)"
            RETVAL=$?
            if [ $RETVAL -ne 0 ]; then
                echo "$0 failed."
                exit 1
            fi
        done
    fi
done

echo "**********************************************************************"

