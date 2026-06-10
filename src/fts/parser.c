#include "fts/parser.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static int is_boolean_op(const char* s, int len) {
  if (len == 3 && strncasecmp(s, "AND", 3) == 0) return FTS_BOOLEAN_AND;
  if (len == 2 && strncasecmp(s, "OR", 2) == 0) return FTS_BOOLEAN_OR;
  if (len == 3 && strncasecmp(s, "NOT", 3) == 0) return FTS_BOOLEAN_NOT;
  return 0;
}

int fts_query_parse(const char* query, fts_query_node_t* nodes, int max_nodes) {
  if (!query || !nodes || max_nodes <= 0) return 0;
  int count = 0;
  const char* p = query;

  while (*p && count < max_nodes) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;

    if (*p == '"') {
      p++;
      fts_query_node_t* node = &nodes[count];
      memset(node, 0, sizeof(fts_query_node_t));
      node->op = FTS_PHRASE;
      node->boost = 1.0;
      int i = 0;
      while (*p && *p != '"' && i < (int)sizeof(node->text) - 1) {
        node->text[i++] = *p++;
      }
      node->text[i] = '\0';
      if (*p == '"') p++;
      count++;
    } else if (*p == '[') {
      p++;
      fts_query_node_t* node = &nodes[count];
      memset(node, 0, sizeof(fts_query_node_t));
      node->op = FTS_RANGE;
      node->boost = 1.0;
      while (*p && isspace((unsigned char)*p)) p++;
      int i = 0;
      while (*p && *p != 'T' && *p != 't' && i < (int)sizeof(node->text) - 1) {
        node->text[i++] = *p++;
      }
      node->text[i] = '\0';
      if ((*p == 'T' || *p == 't') && *(p+1) == 'O' && (*(p+2) == 'T' || *(p+2) == 'o' || *(p+2) == 't' || *(p+2) == 'o'))
        p += 3;
      else if (*p) p++;
      while (*p && isspace((unsigned char)*p)) p++;
      i = 0;
      while (*p && *p != ']' && i < (int)sizeof(node->text2) - 1) {
        node->text2[i++] = *p++;
      }
      node->text2[i] = '\0';
      if (*p == ']') p++;
      count++;
    } else {
      const char* start = p;
      while (*p && !isspace((unsigned char)*p) && *p != '"') p++;
      int len = (int)(p - start);

      int op_type = is_boolean_op(start, len);
      if (op_type && count > 0 && count < max_nodes) {
        fts_query_node_t* node = &nodes[count];
        memset(node, 0, sizeof(fts_query_node_t));
        node->op = (fts_query_op_t)op_type;
        node->boost = 1.0;
        count++;
        continue;
      }

      fts_query_node_t* node = &nodes[count];
      memset(node, 0, sizeof(fts_query_node_t));
      node->op = FTS_TERM;
      node->boost = 1.0;

      int has_field = 0;
      int field_end = -1;
      for (int i = 0; i < len; i++) {
        if (start[i] == ':') {
          has_field = 1;
          field_end = i;
          break;
        }
      }

      if (has_field) {
        int fld_len = field_end;
        if (fld_len > 63) fld_len = 63;
        memcpy(node->field, start, fld_len);
        node->field[fld_len] = '\0';
        start += field_end + 1;
        len -= field_end + 1;
      }

      int is_fuzzy = 0;
      int fuzzy_dist = 0;
      for (int i = 0; i < len; i++) {
        if (start[i] == '~') {
          is_fuzzy = 1;
          fuzzy_dist = 0;
          for (int j = i + 1; j < len; j++) {
            if (start[j] >= '0' && start[j] <= '9')
              fuzzy_dist = fuzzy_dist * 10 + (start[j] - '0');
            else
              break;
          }
          len = i;
          break;
        }
      }

      if (is_fuzzy) {
        node->op = FTS_FUZZY;
        node->fuzzy_distance = fuzzy_dist;
      }

      double boost = 1.0;
      for (int i = 0; i < len; i++) {
        if (start[i] == '^') {
          boost = atof(start + i + 1);
          len = i;
          break;
        }
      }
      if (boost != 1.0 && !is_fuzzy) {
        /* we set op back to FTS_TERM but keep boost */
      }
      node->boost = boost;

      int text_len = len;
      if (text_len > (int)sizeof(node->text) - 1) text_len = (int)sizeof(node->text) - 1;
      memcpy(node->text, start, text_len);
      node->text[text_len] = '\0';
      count++;
    }
  }
  return count;
}
