#ifndef KANBUDB_FTS_PARSER_H
#define KANBUDB_FTS_PARSER_H

#include "macros.h"

typedef enum {
  FTS_TERM,
  FTS_PHRASE,
  FTS_FUZZY,
  FTS_BOOLEAN_AND,
  FTS_BOOLEAN_OR,
  FTS_BOOLEAN_NOT,
  FTS_RANGE,
  FTS_BOOST
} fts_query_op_t;

typedef struct {
  fts_query_op_t op;
  char           field[64];
  char           text[256];
  char           text2[256];
  double         boost;
  int            fuzzy_distance;
} fts_query_node_t;

int fts_query_parse(const char* query, fts_query_node_t* nodes, int max_nodes);

#endif /* KANBUDB_FTS_PARSER_H */
