/*
    Copyright (C) 2016-2017 Tomas Flouri and Ziheng Yang

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    Contact: Tomas Flouri <Tomas.Flouri@h-its.org>,
    Heidelberg Institute for Theoretical Studies,
    Schloss-Wolfsbrunnenweg 35, D-69118 Heidelberg, Germany
*/

#include <assert.h>
#include <search.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <getopt.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>
#if (!defined(WINNT) && !defined(WIN32) && !defined(WIN64))
#include <sys/resource.h>
#endif
#include <x86intrin.h>

/* constants */

#define PROG_NAME "bpp"
#define PROG_VERSION "v0.0.0"

#ifdef __APPLE__
#define PROG_ARCH "macosx_x86_64"
#else
#define PROG_ARCH "linux_x86_64"
#endif

#define BPP_FAILURE  0
#define BPP_SUCCESS  1

#define LINEALLOC 2048
#define ASCII_SIZE 256

#define RTREE_SHOW_LABEL                1
#define RTREE_SHOW_BRANCH_LENGTH        2

#define TREE_TRAVERSE_POSTORDER         1
#define TREE_TRAVERSE_PREORDER          2

#define COMPRESS_GENERAL                1
#define COMPRESS_JC69                   2

/* libpll related definitions */

#define PLL_ALIGNMENT_CPU   8
#define PLL_ALIGNMENT_SSE  16
#define PLL_ALIGNMENT_AVX  32

#define PLL_ATTRIB_ARCH_CPU            0
#define PLL_ATTRIB_ARCH_SSE       (1 << 0)
#define PLL_ATTRIB_PATTERN_TIP    (1 << 4)

#define PLL_ATTRIB_ARCH_AVX       (1 << 1)
#define PLL_ATTRIB_ARCH_AVX2      (1 << 2)
#define PLL_ATTRIB_ARCH_AVX512    (1 << 3)
#define PLL_ATTRIB_ARCH_MASK         0xF

#define PLL_ATTRIB_PATTERN_TIP    (1 << 4)

#define PLL_ATTRIB_RATE_SCALERS   (1 << 9)

#define PLL_SCALE_FACTOR 115792089237316195423570985008687907853269984665640564039457584007913129639936.0  /*  2**256 (exactly)  */
#define PLL_SCALE_THRESHOLD (1.0/PLL_SCALE_FACTOR)
#define PLL_SCALE_FACTOR_SQRT 340282366920938463463374607431768211456.0 /* 2**128 */
#define PLL_SCALE_THRESHOLD_SQRT (1.0/PLL_SCALE_FACTOR_SQRT)
#define PLL_SCALE_BUFFER_NONE -1

#define PLL_MISC_EPSILON 1e-8
/* error codes */

#define ERROR_PHYLIP_SYNTAX            106
#define ERROR_PHYLIP_LONGSEQ           107
#define ERROR_PHYLIP_NONALIGNED        108
#define ERROR_PHYLIP_ILLEGALCHAR       109
#define ERROR_PHYLIP_UNPRINTABLECHAR   110

/* structures and data types */

typedef unsigned int UINT32;
typedef unsigned short WORD;
typedef unsigned char BYTE;

typedef struct dlist_item_s
{
  void * data;
  struct dlist_item_s * prev;
  struct dlist_item_s * next;
} dlist_item_t;

typedef struct dlist_s
{
  dlist_item_t * head;
  dlist_item_t * tail;
} dlist_t;

typedef struct snode_s
{
  char * label;
  double length;
  double theta;
  double tau;
  double old_tau;
  double old_theta;
  struct snode_s * left;
  struct snode_s * right;
  struct snode_s * parent;
  unsigned int leaves;
  unsigned int * gene_leaves;
  int mark;

  void * data;

  /* list of per-locus coalescent events */
  dlist_t ** event;

  int * event_count;

  /* number of lineages coming in the population */
  int * seqin_count;
  double * logpr_contrib;
  double * old_logpr_contrib;
  //unsigned int * seqin_count;
  //unsigned int * seqout_count;

  //unsigned int ** seqin_indices;

  unsigned int node_index;
} snode_t;

typedef struct stree_s
{
  unsigned int tip_count;
  unsigned int inner_count;
  unsigned int edge_count;

  unsigned int locus_count;

  snode_t ** nodes;

  int ** pptable;

  snode_t * root;
} stree_t;

typedef struct gnode_s
{
  char * label;
  double length;
  double time;
  double old_time;
  struct gnode_s * left;
  struct gnode_s * right;
  struct gnode_s * parent;
  unsigned int leaves;

  void * data;

  snode_t * pop;

  /* pointer to the dlist item this node is wrapped into */
  dlist_item_t * event;

  unsigned int node_index;
  unsigned int clv_valid;

  unsigned int clv_index;
  int scaler_index;
  unsigned int pmatrix_index;

  int mark;

} gnode_t;

typedef struct gtree_s
{
  unsigned int tip_count;
  unsigned int inner_count;
  unsigned int edge_count;

  gnode_t ** nodes;
  gnode_t * root;

  /* auxiliary space for traversals */
  double logl;
  double logpr;
  double old_logpr;
  double old_logl;

} gtree_t;

typedef struct msa_s
{
  int count;
  int length;

  char ** sequence;
  char ** label;

} msa_t;

typedef struct locus_s
{
  unsigned int tips;
  unsigned int clv_buffers;
  unsigned int states;
  unsigned int sites;
  unsigned int rate_matrices;
  unsigned int prob_matrices;
  unsigned int rate_cats;
  unsigned int scale_buffers;
  unsigned int attributes;

  /* vectorization parameters */
  size_t alignment;
  unsigned int states_padded;

  double ** clv;
  double ** pmatrix;
  double * rates;
  double * rate_weights;
  double ** subst_params;
  unsigned int ** scale_buffer;
  double ** frequencies;
  unsigned int * pattern_weights;

  int * eigen_decomp_valid;
  double ** eigenvecs;
  double ** inv_eigenvecs;
  double ** eigenvals;

  /* tip-tip precomputation data */
  unsigned int maxstates;
  unsigned char ** tipchars;
  unsigned char * charmap;
  double * ttlookup;
  unsigned int * tipmap;
} locus_t;

/* Simple structure for handling PHYLIP parsing */

typedef struct fasta
{
  FILE * fp;
  char line[LINEALLOC];
  const unsigned int * chrstatus;
  long no;
  long filesize;
  long lineno;
  long stripped_count;
  long stripped[256];
} fasta_t;


/* Simple structure for handling PHYLIP parsing */

typedef struct phylip_s
{
  FILE * fp;
  char * line;
  size_t line_size;
  size_t line_maxsize;
  char buffer[LINEALLOC];
  const unsigned int * chrstatus;
  long no;
  long filesize;
  long lineno;
  long stripped_count;
  long stripped[256];
} phylip_t;

typedef struct mapping_s
{
  char * individual;
  char * species;
  int lineno;
} mapping_t;

typedef struct list_item_s
{
  void * data;
  struct list_item_s * next;
} list_item_t;

typedef struct list_s
{
  list_item_t * head;
  list_item_t * tail;
  long count;
} list_t;

typedef struct ht_item_s
{
  unsigned long key;
  void * value;
} ht_item_t;

typedef struct hashtable_s
{
  unsigned long table_size;
  unsigned long entries_count;
  list_t ** entries;
} hashtable_t;

typedef struct pair_s
{
  char * label;
  void * data;
} pair_t;

/* macros */

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define SWAP(x,y) do { __typeof__ (x) _t = x; x = y; y = _t; } while(0)

#define legacy_rndexp(mean) (-(mean)*log(legacy_rndu()))

/* options */

extern long opt_help;
extern long opt_version;
extern long opt_quiet;
extern long opt_seed;
extern long opt_stree;
extern long opt_delimit;
extern long opt_cleandata;
extern long opt_debug;
extern long opt_samples;
extern long opt_samplefreq;
extern long opt_burnin;
extern long opt_finetune_reset;
extern double opt_tau_alpha;
extern double opt_tau_beta;
extern double opt_theta_alpha;
extern double opt_theta_beta;
extern double opt_finetune_gtage;
extern double opt_finetune_gtspr;
extern double opt_finetune_theta;
extern double opt_finetune_tau;
extern double opt_finetune_mix;
extern char * opt_mapfile;
extern char * opt_msafile;
extern char * opt_mapfile;
extern char * opt_mcmcfile;
extern char * opt_reorder;
extern char * opt_outfile;
extern char * opt_streefile;
extern char * cmdline;

/* common data */

extern __thread int bpp_errno;
extern __thread char bpp_errmsg[200];

extern const unsigned int pll_map_nt[256];
extern const unsigned int pll_map_fasta[256];
extern const unsigned int pll_map_amb[256];
extern const unsigned int pll_map_validjc69[16];

extern long mmx_present;
extern long sse_present;
extern long sse2_present;
extern long sse3_present;
extern long ssse3_present;
extern long sse41_present;
extern long sse42_present;
extern long popcnt_present;
extern long avx_present;
extern long avx2_present;

/* functions in util.c */

void fatal(const char * format, ...) __attribute__ ((noreturn));
void progress_init(const char * prompt, unsigned long size);
void progress_update(unsigned int progress);
void progress_done(void);
void * xmalloc(size_t size);
void * xcalloc(size_t nmemb, size_t size);
void * xrealloc(void *ptr, size_t size);
char * xstrchrnul(char *s, int c);
char * xstrdup(const char * s);
char * xstrndup(const char * s, size_t len);
long getusec(void);
void show_rusage(void);
FILE * xopen(const char * filename, const char * mode);
void * pll_aligned_alloc(size_t size, size_t alignment);
void pll_aligned_free(void * ptr);

/* functions in bpp.c */

void args_init(int argc, char ** argv);

void cmd_help(void);

void getentirecommandline(int argc, char * argv[]);

void fillheader(void);

void show_header(void);

void cmd_ml(void);

/* functions in parse_stree.y */

stree_t * stree_parse_newick(const char * filename);

void stree_destroy(stree_t * tree,
                   void (*cb_destroy)(void *));

/* functions in phylip.c */

phylip_t * phylip_open(const char * filename,
                       const unsigned int * map);

int phylip_rewind(phylip_t * fd);

void phylip_close(phylip_t * fd);

msa_t * phylip_parse_interleaved(phylip_t * fd);

msa_t * phylip_parse_sequential(phylip_t * fd);

msa_t ** phylip_parse_multisequential(phylip_t * fd, int * count);

/* functions in stree.c */

void stree_show_ascii(const snode_t * root, int options);

char * stree_export_newick(const snode_t * root,
                           char * (*cb_serialize)(const snode_t *));

int stree_traverse(snode_t * root,
                   int traversal,
                   int (*cbtrav)(snode_t *),
                   snode_t ** outbuffer,
                   unsigned int * trav_size);

stree_t ** stree_tipstring_nodes(stree_t * root,
                                 char * tipstring,
                                 unsigned int * tiplist_count);

hashtable_t * species_hash(stree_t * tree);

hashtable_t * maplist_hash(list_t * maplist, hashtable_t * sht);

double stree_propose_theta(gtree_t ** gtree, stree_t * stree);

double stree_propose_tau(gtree_t ** gtree, stree_t * stree, locus_t ** loci);

void stree_fini(void);

/* functions in arch.c */

unsigned long arch_get_memused(void);

unsigned long arch_get_memtotal(void);

/* functions in msa.c */

void msa_print(msa_t * msa);

void msa_destroy(msa_t * msa);

int msa_remove_ambiguous(msa_t * msa);

/* functions in parse_map.y */

list_t * yy_parse_map(const char * filename);

/* functions in mapping.c */

void maplist_print(list_t * map_list);

void map_dealloc(void * data);

hashtable_t * maplist_hash(list_t * maplist, hashtable_t * sht);

/* functions in list.c */

void list_append(list_t * list, void * data);

void list_prepend(list_t * list, void * data);

void list_clear(list_t * list, void (*cb_dealloc)(void *));

/* functions in dlist.c */

dlist_item_t * dlist_append(dlist_t * dlist, void * data);
void dlist_clear(dlist_t * dlist, void (*cb_dealloc)(void *));
void dlist_item_remove(dlist_item_t * item);
void dlist_item_append(dlist_t * dlist, dlist_item_t * item);
void dlist_item_prepend(dlist_t * dlist, dlist_item_t * item);
dlist_t * dlist_create();
void dlist_destroy(dlist_t *);

/* functions in hash.c */

void * hashtable_find(hashtable_t * ht,
                      void * x,
                      unsigned long hash,
                      int (*cb_cmp)(void *, void *));

hashtable_t * hashtable_create(unsigned long items_count);

int hashtable_strcmp(void * x, void * y);

int hashtable_ptrcmp(void * x, void * y);

unsigned long hash_djb2a(char * s);

unsigned long hash_fnv(char * s);

int hashtable_insert(hashtable_t * ht,
                     void * x,
                     unsigned long hash,
                     int (*cb_cmp)(void *, void *));

void hashtable_destroy(hashtable_t * ht, void (*cb_dealloc)(void *));

int cb_cmp_pairlabel(void * a, void * b);

/* functions in stree.c */

void stree_init(stree_t * stree, msa_t ** msa, list_t * maplist, int msa_count);

/* functions in random.c */

double legacy_rndu(void);
double legacy_rnd_symmetrical(void);

/* functions in gtree.c */

gtree_t ** gtree_init(stree_t * stree,
                      msa_t ** msalist,
                      list_t * maplist,
                      int msa_count);

char * gtree_export_newick(const gnode_t * root,
                           char * (*cb_serialize)(const gnode_t *));

void gtree_destroy(gtree_t * tree, void (*cb_destroy)(void *));

int gtree_traverse(gnode_t * root,
                   int traversal,
                   int (*cbtrav)(gnode_t *),
                   gnode_t ** outbuffer,
                   unsigned int * trav_size);

void gtree_update_branch_lengths(gtree_t ** gtree_list, int msa_count);

double gtree_propose_ages(locus_t ** locus, gtree_t ** gtree, stree_t * stree);

void gtree_fini(int msa_count);

double gtree_logprob(stree_t * stree, int msa_index);
double gtree_update_logprob_contrib(snode_t * snode, int msa_index);
double gtree_propose_spr(locus_t ** locus, gtree_t ** gtree, stree_t * stree);
double reflect(double t, double minage, double maxage);
gnode_t ** gtree_return_partials(gnode_t * root,
                                 unsigned int msa_index,
                                 unsigned int * trav_size);

/* functions in prop_mixing.c */

long proposal_mixing(gtree_t ** gtree, stree_t * stree, locus_t ** locus);

/* functions in locus.c */

locus_t * locus_create(unsigned int tips,
                       unsigned int clv_buffers,
                       unsigned int states,
                       unsigned int sites,
                       unsigned int rate_matrices,
                       unsigned int prob_matrices,
                       unsigned int rate_cats,
                       unsigned int scale_buffers,
                       unsigned int attributes);

void locus_destroy(locus_t * locus);

int pll_set_tip_states(locus_t * locus,
                       unsigned int tip_index,
                       const unsigned int * map,
                       const char * sequence);

int pll_set_tip_clv(locus_t * locus,
                    unsigned int tip_index,
                    const double * clv,
                    int padding);

void pll_set_frequencies(locus_t * locus,
                         unsigned int freqs_index,
                         const double * frequencies);

void locus_update_partials(locus_t * locus, gnode_t ** traversal, int count);

void pll_set_pattern_weights(locus_t * locus,
                             const unsigned int * pattern_weights);

void locus_update_matrices_jc69(locus_t * locus,
                                gnode_t ** traversal,
                                unsigned int count);

double locus_root_loglikelihood(locus_t * locus,
                                gnode_t * root,
                                const unsigned int * freqs_indices,
                                double * persite_lnl);

/* functions in compress.c */


unsigned int * compress_site_patterns(char ** sequence,
                                      const unsigned int * map,
                                      int count,
                                      int * length,
                                      int attrib);

/* functions in core_partials.c */

void pll_core_update_partial_tt_4x4(unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const unsigned char * left_tipchars,
                                    const unsigned char * right_tipchars,
                                    const double * lookup,
                                    unsigned int attrib);

void pll_core_update_partial_tt(unsigned int states,
                                unsigned int sites,
                                unsigned int rate_cats,
                                double * parent_clv,
                                unsigned int * parent_scaler,
                                const unsigned char * left_tipchars,
                                const unsigned char * right_tipchars,
                                const unsigned int * tipmap,
                                unsigned int tipmap_size,
                                const double * lookup,
                                unsigned int attrib);

void pll_core_update_partial_ti_4x4(unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const unsigned char * left_tipchars,
                                    const double * right_clv,
                                    const double * left_matrix,
                                    const double * right_matrix,
                                    const unsigned int * right_scaler,
                                    unsigned int attrib);

void pll_core_update_partial_ti(unsigned int states,
                                unsigned int sites,
                                unsigned int rate_cats,
                                double * parent_clv,
                                unsigned int * parent_scaler,
                                const unsigned char * left_tipchars,
                                const double * right_clv,
                                const double * left_matrix,
                                const double * right_matrix,
                                const unsigned int * right_scaler,
                                const unsigned int * tipmap,
                                unsigned int tipmap_size,
                                unsigned int attrib);

void pll_core_update_partial_ii(unsigned int states,
                                unsigned int sites,
                                unsigned int rate_cats,
                                double * parent_clv,
                                unsigned int * parent_scaler,
                                const double * left_clv,
                                const double * right_clv,
                                const double * left_matrix,
                                const double * right_matrix,
                                const unsigned int * left_scaler,
                                const unsigned int * right_scaler,
                                unsigned int attrib);

void pll_core_create_lookup_4x4(unsigned int rate_cats,
                                double * lookup,
                                const double * left_matrix,
                                const double * right_matrix);

void pll_core_create_lookup(unsigned int states,
                            unsigned int rate_cats,
                            double * lookup,
                            const double * left_matrix,
                            const double * right_matrix,
                            const unsigned int * tipmap,
                            unsigned int tipmap_size,
                            unsigned int attrib);

/* functions in core_pmatrix.c */

int pll_core_update_pmatrix(double ** pmatrix,
                            unsigned int states,
                            unsigned int rate_cats,
                            const double * rates,
                            const double * branch_lengths,
                            const unsigned int * matrix_indices,
                            const unsigned int * params_indices,
                            double * const * eigenvals,
                            double * const * eigenvecs,
                            double * const * inv_eigenvecs,
                            unsigned int count,
                            unsigned int attrib);

int pll_core_update_pmatrix_4x4_jc69(double ** pmatrix,
                                     unsigned int states,
                                     unsigned int rate_cats,
                                     const double * rates,
                                     const double * branch_lengths,
                                     const unsigned int * matrix_indices,
                                     const unsigned int * params_indices,
                                     unsigned int count,
                                     unsigned int attrib);

/* functions in core_likelihood.c */

double pll_core_root_loglikelihood(unsigned int states,
                                   unsigned int sites,
                                   unsigned int rate_cats,
                                   const double * clv,
                                   const unsigned int * scaler,
                                   double * const * frequencies,
                                   const double * rate_weights,
                                   const unsigned int * pattern_weights,
                                   const unsigned int * freqs_indices,
                                   double * persite_lnl,
                                   unsigned int attrib);

/* functions in output.c */

void pll_show_pmatrix(const locus_t * locus,
                                 unsigned int index,
                                 unsigned int float_precision);

void pll_show_clv(const locus_t * locus,
                             unsigned int clv_index,
                             int scaler_index,
                             unsigned int float_precision);

/* functions in method_00.c */

void cmd_a00(void);

/* functions in method_10.c */

void cmd_a10(void);

/* functions in delimit.c */

long delimitations_count(stree_t * stree);

void delimitations_enumerate(stree_t * stree);

/* functions in core_partials_sse.c */

#ifdef HAVE_SSE3
void pll_core_create_lookup_sse(unsigned int states,
                                unsigned int rate_cats,
                                double * ttlookup,
                                const double * left_matrix,
                                const double * right_matrix,
                                const unsigned int * tipmap,
                                unsigned int tipmap_size);

void pll_core_create_lookup_4x4_sse(unsigned int rate_cats,
                                    double * lookup,
                                    const double * left_matrix,
                                    const double * right_matrix);

void pll_core_update_partial_tt_sse(unsigned int states,
                                    unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const unsigned char * left_tipchars,
                                    const unsigned char * right_tipchars,
                                    const double * lookup,
                                    unsigned int tipstates_count,
                                    unsigned int attrib);

void pll_core_update_partial_tt_4x4_sse(unsigned int sites,
                                        unsigned int rate_cats,
                                        double * parent_clv,
                                        unsigned int * parent_scaler,
                                        const unsigned char * left_tipchars,
                                        const unsigned char * right_tipchars,
                                        const double * lookup,
                                        unsigned int attrib);

void pll_core_update_partial_ti_sse(unsigned int states,
                                    unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const unsigned char * left_tipchars,
                                    const double * right_clv,
                                    const double * left_matrix,
                                    const double * right_matrix,
                                    const unsigned int * right_scaler,
                                    const unsigned int * tipmap,
                                    unsigned int tipmap_size,
                                    unsigned int attrib);


void pll_core_update_partial_ti_4x4_sse(unsigned int sites,
                                        unsigned int rate_cats,
                                        double * parent_clv,
                                        unsigned int * parent_scaler,
                                        const unsigned char * left_tipchar,
                                        const double * right_clv,
                                        const double * left_matrix,
                                        const double * right_matrix,
                                        const unsigned int * right_scaler,
                                        unsigned int attrib);

void pll_core_update_partial_ii_sse(unsigned int states,
                                    unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const double * left_clv,
                                    const double * right_clv,
                                    const double * left_matrix,
                                    const double * right_matrix,
                                    const unsigned int * left_scaler,
                                    const unsigned int * right_scaler,
                                    unsigned int attrib);

void pll_core_update_partial_ii_4x4_sse(unsigned int sites,
                                        unsigned int rate_cats,
                                        double * parent_clv,
                                        unsigned int * parent_scaler,
                                        const double * left_clv,
                                        const double * right_clv,
                                        const double * left_matrix,
                                        const double * right_matrix,
                                        const unsigned int * left_scaler,
                                        const unsigned int * right_scaler,
                                        unsigned int attrib);

/* functions in core_likelihood_sse.c */


double pll_core_root_loglikelihood_sse(unsigned int states,
                                       unsigned int sites,
                                       unsigned int rate_cats,
                                       const double * clv,
                                       const unsigned int * scaler,
                                       double * const * frequencies,
                                       const double * rate_weights,
                                       const unsigned int * pattern_weights,
                                       const unsigned int * freqs_indices,
                                       double * persite_lnl);

double pll_core_root_loglikelihood_4x4_sse(unsigned int sites,
                                           unsigned int rate_cats,
                                           const double * clv,
                                           const unsigned int * scaler,
                                           double * const * frequencies,
                                           const double * rate_weights,
                                           const unsigned int * pattern_weights,
                                           const unsigned int * freqs_indices,
                                           double * persite_lnl);
#endif

/* functions in core_partials_avx.c */

#ifdef HAVE_AVX
void pll_core_create_lookup_avx(unsigned int states,
                                unsigned int rate_cats,
                                double * lookup,
                                const double * left_matrix,
                                const double * right_matrix,
                                const unsigned int * tipmap,
                                unsigned int tipmap_size);

void pll_core_create_lookup_4x4_avx(unsigned int rate_cats,
                                    double * lookup,
                                    const double * left_matrix,
                                    const double * right_matrix);

void pll_core_create_lookup_20x20_avx(unsigned int rate_cats,
                                      double * ttlookup,
                                      const double * left_matrix,
                                      const double * right_matrix,
                                      const unsigned int * tipmap,
                                      unsigned int tipmap_size);

void pll_core_update_partial_tt_avx(unsigned int states,
                                    unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const unsigned char * left_tipchars,
                                    const unsigned char * right_tipchars,
                                    const double * lookup,
                                    unsigned int tipstates_count,
                                    unsigned int attrib);

void pll_core_update_partial_tt_4x4_avx(unsigned int sites,
                                        unsigned int rate_cats,
                                        double * parent_clv,
                                        unsigned int * parent_scaler,
                                        const unsigned char * left_tipchars,
                                        const unsigned char * right_tipchars,
                                        const double * lookup,
                                        unsigned int attrib);

void pll_core_update_partial_ti_avx(unsigned int states,
                                    unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const unsigned char * left_tipchars,
                                    const double * right_clv,
                                    const double * left_matrix,
                                    const double * right_matrix,
                                    const unsigned int * right_scaler,
                                    const unsigned int * tipmap,
                                    unsigned int tipmap_size,
                                    unsigned int attrib);

void pll_core_update_partial_ti_4x4_avx(unsigned int sites,
                                        unsigned int rate_cats,
                                        double * parent_clv,
                                        unsigned int * parent_scaler,
                                        const unsigned char * left_tipchar,
                                        const double * right_clv,
                                        const double * left_matrix,
                                        const double * right_matrix,
                                        const unsigned int * right_scaler,
                                        unsigned int attrib);

void pll_core_update_partial_ti_20x20_avx(unsigned int sites,
                                          unsigned int rate_cats,
                                          double * parent_clv,
                                          unsigned int * parent_scaler,
                                          const unsigned char * left_tipchar,
                                          const double * right_clv,
                                          const double * left_matrix,
                                          const double * right_matrix,
                                          const unsigned int * right_scaler,
                                          const unsigned int * tipmap,
                                          unsigned int tipmap_size,
                                          unsigned int attrib);

void pll_core_update_partial_ii_avx(unsigned int states,
                                    unsigned int sites,
                                    unsigned int rate_cats,
                                    double * parent_clv,
                                    unsigned int * parent_scaler,
                                    const double * left_clv,
                                    const double * right_clv,
                                    const double * left_matrix,
                                    const double * right_matrix,
                                    const unsigned int * left_scaler,
                                    const unsigned int * right_scaler,
                                    unsigned int attrib);

void pll_core_update_partial_ii_4x4_avx(unsigned int sites,
                                        unsigned int rate_cats,
                                        double * parent_clv,
                                        unsigned int * parent_scaler,
                                        const double * left_clv,
                                        const double * right_clv,
                                        const double * left_matrix,
                                        const double * right_matrix,
                                        const unsigned int * left_scaler,
                                        const unsigned int * right_scaler,
                                        unsigned int attrib);

/* functions in core_likelihood_avx.c */


double pll_core_root_loglikelihood_avx(unsigned int states,
                                       unsigned int sites,
                                       unsigned int rate_cats,
                                       const double * clv,
                                       const unsigned int * scaler,
                                       double * const * frequencies,
                                       const double * rate_weights,
                                       const unsigned int * pattern_weights,
                                       const unsigned int * freqs_indices,
                                       double * persite_lnl);

double pll_core_root_loglikelihood_4x4_avx(unsigned int sites,
                                           unsigned int rate_cats,
                                           const double * clv,
                                           const unsigned int * scaler,
                                           double * const * frequencies,
                                           const double * rate_weights,
                                           const unsigned int * pattern_weights,
                                           const unsigned int * freqs_indices,
                                           double * persite_lnl);
#endif


#ifdef HAVE_AVX2

/* functions in core_partials_avx2.c */

void pll_core_update_partial_ti_avx2(unsigned int states,
                                     unsigned int sites,
                                     unsigned int rate_cats,
                                     double * parent_clv,
                                     unsigned int * parent_scaler,
                                     const unsigned char * left_tipchars,
                                     const double * right_clv,
                                     const double * left_matrix,
                                     const double * right_matrix,
                                     const unsigned int * right_scaler,
                                     const unsigned int * tipmap,
                                     unsigned int tipmap_size,
                                     unsigned int attrib);


void pll_core_update_partial_ti_20x20_avx2(unsigned int sites,
                                           unsigned int rate_cats,
                                           double * parent_clv,
                                           unsigned int * parent_scaler,
                                           const unsigned char * left_tipchar,
                                           const double * right_clv,
                                           const double * left_matrix,
                                           const double * right_matrix,
                                           const unsigned int * right_scaler,
                                           const unsigned int * tipmap,
                                           unsigned int tipmap_size,
                                           unsigned int attrib);

void pll_core_update_partial_ii_avx2(unsigned int states,
                                     unsigned int sites,
                                     unsigned int rate_cats,
                                     double * parent_clv,
                                     unsigned int * parent_scaler,
                                     const double * left_clv,
                                     const double * right_clv,
                                     const double * left_matrix,
                                     const double * right_matrix,
                                     const unsigned int * left_scaler,
                                     const unsigned int * right_scaler,
                                     unsigned int attrib);

/* functions in core_likelihood_avx2.c */

double pll_core_root_loglikelihood_avx2(unsigned int states,
                                        unsigned int sites,
                                        unsigned int rate_cats,
                                        const double * clv,
                                        const unsigned int * scaler,
                                        double * const * frequencies,
                                        const double * rate_weights,
                                        const unsigned int * pattern_weights,
                                        const unsigned int * freqs_indices,
                                        double * persite_lnl);

#endif
