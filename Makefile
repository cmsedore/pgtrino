# trino_fdw: PostgreSQL foreign-data wrapper for Trino
MODULE_big = trino_fdw
OBJS = src/trino_fdw.o src/trino_client.o src/trino_deparse.o \
       src/trino_json.o src/trino_types.o

EXTENSION = trino_fdw
DATA = trino_fdw--1.0.sql
PGFILEDESC = "trino_fdw - foreign-data wrapper for Trino"

PG_CPPFLAGS = -I$(srcdir)/src $(shell curl-config --cflags)
SHLIB_LINK = $(shell curl-config --libs)

REGRESS = trino_fdw
REGRESS_OPTS = --inputdir=test --outputdir=test --load-extension=trino_fdw

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
