#ifndef KANBUDB_FTS_RANKER_H
#define KANBUDB_FTS_RANKER_H

double bm25_score(double term_freq, double doc_len, double avg_dl,
                  double num_docs, double doc_freq,
                  double k1, double b);

#endif /* KANBUDB_FTS_RANKER_H */
