#include "fts/ranker.h"
#include <math.h>

double bm25_score(double term_freq, double doc_len, double avg_dl,
                  double num_docs, double doc_freq,
                  double k1, double b) {
  if (doc_freq <= 0.0 || num_docs <= 0.0) return 0.0;
  double idf = log(1.0 + (num_docs - doc_freq + 0.5) / (doc_freq + 0.5));
  double tf = term_freq * (k1 + 1.0) / (term_freq + k1 * (1.0 - b + b * doc_len / avg_dl));
  return idf * tf;
}
