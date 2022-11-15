// Self-search: Mark Firsts (∊), Occurrence Count (⊒), Classify (⊐), Deduplicate (⍷)

// Except for trivial cases, ⍷ is implemented as ∊⊸/
// Other functions use adaptations of the same set of methods

// Boolean ∊: 1 at first element and first ¬⊑𝕩
// Boolean ⊒: Branchless sum thing
//   COULD vectorize boolean ⊒ with +`
// Boolean ⊐: ⥊¬⍟⊑𝕩
// SHOULD implement boolean ⍷ directly
// Sorted flags: start with r0⌾⊑»⊸≠𝕩 (r0 is 0 for ⊐ and 1 otherwise)
//   ∊: ⊢; ⊐: +`; ⊒: ↕∘≠⊸(⊣-⌈`∘×)
//   COULD determine ⊒ result type by direct comparisons on 𝕩
// Brute force or all-pairs comparison for small lengths
//   Branchless, not vectorized (+´∧` structure for ⊐)
// Full-size table lookups for 1- and 2-byte 𝕩
//   2-byte table can be "sparse" initialized with an extra pass over 𝕩
//   4-byte ⊐ can use a small-range lookup table
//   COULD add small-range 4-byte tables for ∊ and ⊒
// Radix-assisted lookups are fallbacks for 4-byte ∊ and ⊒
//   COULD do radix-assisted ⊐ as ⍷⊸⊐ or similar
//   Specializes on constant top 1/2 bytes, but hashes make this rare

// Specialized 4-byte and 8-byte hash tables
//   In-place resizing by factor of 4 based on measured collisions
//   Max collisions ensures bounded time spent here before giving up
//   First element used as sentinel (not good for ⊒)
//   COULD prefetch when table gets larger
// Generic hash table for other cases
//   Resizing is pretty expensive here

#include "../core.h"
#include "../utils/hash.h"
#include "../utils/talloc.h"
#include "../utils/calls.h"
#include "../builtins.h"

extern B not_c1(B, B);
extern B shape_c1(B, B);
extern B slash_c2(B, B, B);
extern B scan_c1(Md1D*, B);
extern B ud_c1(B, B);
extern B sub_c2(B, B, B);
extern B mul_c2(B, B, B);

// These hashes are stored in tables and must be invertible!
#if defined(__SSE4_2__)
static inline u32 hash32(u32 x) { return _mm_crc32_u32(0x973afb51, x); }
#else
// Murmur3
static inline u32 hash32(u32 x) {
  x ^= x >> 16; x *= 0x85ebca6b;
  x ^= x >> 13; x *= 0xc2b2ae35;
  x ^= x >> 16;
  return x;
}
#endif
static inline u64 hash64(u64 x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccd;
  x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53;
  x ^= x >> 33;
  return x;
}

static inline bool use_sorted(B x, u8 logw) {
  if (!FL_HAS(x, fl_asc|fl_dsc)) return 0;
  if (logw==6) return TI(x, elType) == el_f64;
  return 3<=logw & logw<=5;
}
static inline B shift_ne(B x, usz n, u8 lw, bool r0) { // consumes x
  u64* rp; B r = m_bitarrv(&rp, n);
  u8* xp = tyany_ptr(x);
  u8 lb = lw - 3;
  CMP_AA_IMM(ne, el_i8+lb, rp, xp-(1<<lb), xp, n);
  bitp_set(rp, 0, r0);
  decG(x); return r;
}

static bool canCompare64_norm(B x, usz n) {
  u8 e = TI(x,elType);
  if (e == el_B) return 0;
  if (e == el_f64) {
    f64* pf = f64any_ptr(x);
    u64* pu = (u64*)pf;
    for (usz i = 0; i < n; i++) {
      f64 f = pf[i];
      if (f!=f) return 0;
      if (pu[i] == m_f64(-0.0).u) return 0;
    }
  }
  return 1;
}

#define GRADE_UD(U,D) U
#include "radix.h"
u8 radix_offsets_2_u32(usz* c0, u32* v0, usz n) {
  usz rx = 256;
  usz* c1 = c0 + rx;
  // Count keys
  for (usz j=0; j<2*rx+1; j++) c0[j] = 0;
  for (usz i=0; i<n; i++) { u32 v=v0[i]; (c0+1)[(u8)(v>>16)]++; (c1+1)[(u8)(v>>24)]++; }
  u32 v=v0[0];
  // Inclusive prefix sum; note c offsets above
  if ((c1+1)[(u8)(v>>24)] < n) { c1[0]-=n; RADIX_SUM_2_u32; return 2; }
  if ((c0+1)[(u8)(v>>16)] < n) {           RADIX_SUM_1_u32; return 1; }
  return 0;
}
#undef GRADE_UD
#define RADIX_LOOKUP_32(INIT, SETTAB) \
  u8 bytes = radix_offsets_2_u32(c0, v0, n);                                                    \
  usz tim = tn/(64/sizeof(*tab)); /* sparse table init max */                                   \
  if (bytes==0) {                                                                               \
    if (n<tim) for (usz i=0; i< n; i++) tab[(u16)v0[i]]=INIT;                                   \
    else       for (usz j=0; j<tn; j++) tab[j]=INIT;                                            \
    for (usz i=0; i<n; i++) { u32 j=(u16)v0[i]; r0[i]=tab[j]; tab[j]SETTAB; }                   \
  } else {                                                                                      \
    if (bytes==1) { v1=v2; r1=r2; }                                                             \
    for (usz i=0; i<n; i++) { u32 v=v0[i]; u8 k=k0[i]=(u8)(v>>16); usz c=c0[k]++; v1[c]=v; }    \
    if (bytes==1) {                                                                             \
      /* Table lookup, getting radix boundaries from c0 */                                      \
      for (usz i=0; i<n; ) {                                                                    \
        usz l=c0[(u8)(v1[i]>>16)];                                                              \
        if (l-i < tim) for (usz ii=i; ii<l; ii++) tab[(u16)v1[ii]]=INIT;                        \
        else           for (usz  j=0; j<tn;  j++) tab[j]=INIT;                                  \
        for (; i<l; i++) { u32 j=(u16)v1[i]; r1[i]=tab[j]; tab[j]SETTAB; }                      \
      }                                                                                         \
    } else {                                                                                    \
      /* Radix move */                                                                          \
      for (usz i=0; i<n; i++) { u32 v=v1[i]; u8 k=k1[i]=(u8)(v>>24); usz c=c1[k]++; v2[c]=v; }  \
      /* Table lookup */                                                                        \
      u32 tv=v2[0]>>16; v2[n]=~v2[n-1];                                                         \
      for (usz l=0, i=0; l<n; ) {                                                               \
        for (;    ; l++) { u32 v=v2[l], t0=tv; tv=v>>16; if (tv!=t0) break; tab[(u16)v]=INIT; } \
        for (; i<l; i++) { u32 j=(u16)v2[i]; r2[i]=tab[j]; tab[j]SETTAB; }                      \
      }                                                                                         \
      /* Radix unmove; back up c0 to account for increments in radix step  */                   \
      *--c1=0; for (usz i=0; i<n; i++) { r1[i]=r2[c1[k1[i]]++]; }                               \
    }                                                                                           \
    *--c0=0; for (usz i=0; i<n; i++) { r0[i]=r1[c0[k0[i]]++]; }                                 \
  }                                                                                             \
  decG(x); TFREE(alloc);

static NOINLINE void memset32(u32* p, u32 v, usz l) { for (usz i=0; i<l; i++) p[i]=v; }
static NOINLINE void memset64(u64* p, u64 v, usz l) { for (usz i=0; i<l; i++) p[i]=v; }

// Resizing hash table, with fallback
#define SELFHASHTAB(T, W, RAD, STOP, RES0, RESULT, RESWRITE, THRESHMUL, THRESH, AUXSIZE, AUXINIT, AUXEXTEND, AUXMOVE) \
  usz log = 64 - CLZ(n);                                                 \
  usz msl = (64 - CLZ(n+n/2)) + 1; if (RAD && msl>20) msl=20;            \
  usz sh = W - (msl<14? msl : 12+(msl&1)); /* Shift to fit to table */   \
  usz sz = 1 << (W - sh); /* Initial size */                             \
  usz msz = 1ull << msl;  /* Max sz       */                             \
  usz b = 64;  /* Block size */                                          \
  /* Resize or abort if more than 1/2^thresh collisions/element */       \
  usz thresh = THRESH;                                                   \
  /* Filling e slots past the end requires e*(e+1)/2 collisions, so */   \
  /* n entries with <2 each can fill <sqrt(4*n) */                       \
  usz ext = n<=b? n : b + (1ull << (log/2 + 1));                         \
  TALLOC(u8, halloc, (msz+ext)*(sizeof(T)+AUXSIZE));                     \
  T* hash = (T*)halloc + msz-sz;                                         \
  T x0 = hash##W(xp[0]); rp[0] = RES0;                                   \
  memset##W(hash, x0, sz+ext);                                           \
  AUXINIT                                                                \
  usz cc = 0; /* Collision counter */                                    \
  usz i=1; while (1) {                                                   \
    usz e = n-i>b? i+b : n;                                              \
    for (; i < e; i++) {                                                 \
      T h = hash##W(xp[i]); T j0 = h>>sh, j = j0; T k;                   \
      while (k=hash[j], k!=h & k!=x0) j++;                               \
      cc += j-j0;                                                        \
      RESWRITE                                                           \
    }                                                                    \
    if (i == n) break;                                                   \
    i64 dc = (i64)cc - ((THRESHMUL*i)>>thresh);                          \
    if (dc >= 0) {                                                       \
      if (sz == msz || (RAD && i < n/2 && sz >= 1<<18)) break; /*Abort*/ \
      /* Avoid resizing if close to the end */                           \
      if (cc<STOP && (n-i)*dc < (i*((i64)n+i))>>(5+log-(W-sh))) continue;\
      /* Resize hash, factor of 4 */                                     \
      usz m = 2;                                                         \
      usz dif = sz*((1<<m)-1);                                           \
      sh -= m; sz <<= m;                                                 \
      hash -= dif;                                                       \
      usz j = 0;                                                         \
      cc = 0;                                                            \
      memset##W(hash, x0, dif);                                          \
      AUXEXTEND                                                          \
      for (j = dif; j < sz + ext; j++) {                                 \
        T h = hash[j]; if (h==x0) continue; hash[j] = x0;                \
        T k0 = h>>sh, k = k0; while (hash[k]!=x0) k++;                   \
        cc += k-k0;                                                      \
        hash[k] = h; AUXMOVE                                             \
      }                                                                  \
      if (cc >= STOP) break;                                             \
      thresh = THRESH;                                                   \
    }                                                                    \
  }                                                                      \
  TFREE(halloc);                                                         \
  if (i==n) { decG(x); return RESULT; }
#define SELFHASHTAB_VAL(T, W, RAD, STOP, RES0, RESULT, RESWRITE, THRESHMUL, THRESH, INIT) \
  SELFHASHTAB(T, W, RAD, STOP, RES0, RESULT, RESWRITE, THRESHMUL, THRESH, \
    /*AUXSIZE*/sizeof(u32),                                \
    /* AUXINIT */                                          \
    u32* val = (u32*)(hash+sz+ext) + msz-sz;               \
    memset32(val, 0, sz+ext);                              \
    INIT ,                                                 \
    /*AUXEXTEND*/val -= dif; memset32(val, 0, dif); ,      \
    /*AUXMOVE*/u32 v = val[j]; val[j] = 0; val[k] = v;)

B memberOf_c1(B t, B x) {
  if (isAtm(x) || RNK(x)==0) thrM("∊: Argument cannot have rank 0");
  u64 n = *SH(x);
  if (n<=1) { decG(x); return n ? taga(arr_shVec(allOnes(1))) : emptyIVec(); }
  
  u8 lw = cellWidthLog(x);
  void* xv = tyany_ptr(x);
  if (lw == 0) {
    usz i = bit_find(xv, n, 1 &~ *(u64*)xv); decG(x);
    B r = taga(arr_shVec(allZeroes(n)));
    u64* rp = tyany_ptr(r);
    rp[0]=1; if (i<n) bitp_set(rp, i, 1);
    return r;
  }
  if (use_sorted(x, lw)) {
    return shift_ne(x, n, lw, 1);
  }
  #define BRUTE(T) \
    i##T* xp = xv;                                                     \
    u64 rv = 1;                                                        \
    for (usz i=1; i<n; i++) {                                          \
      bool c=1; i##T xi=xp[i];                                         \
      PLAINLOOP for (usz j=0; j<i; j++) c &= xi!=xp[j];                \
      rv |= c<<i;                                                      \
    }                                                                  \
    decG(x); u64* rp; B r = m_bitarrv(&rp, n); rp[0] = rv;             \
    return r;
  #define LOOKUP(T) \
    usz tn = 1<<T;                                                     \
    u##T* xp = (u##T*)xv;                                              \
    i8* rp; B r = m_i8arrv(&rp, n);                                    \
    TALLOC(u8, tab, tn);                                               \
    if (T>8 && n<tn/64) for (usz i=0; i<n;  i++) tab[xp[i]]=1;         \
    else                for (usz j=0; j<tn; j++) tab[j]=1;             \
    for (usz i=0; i<n; i++) { u##T j=xp[i]; rp[i]=tab[j]; tab[j]=0; }  \
    decG(x); TFREE(tab);                                               \
    return num_squeeze(r)
  if (lw == 3) { if (n<8) { BRUTE(8); } else { LOOKUP(8); } }
  if (lw == 4) { if (n<8) { BRUTE(16); } else { LOOKUP(16); } }
  #undef LOOKUP
  #define HASHTAB(T, W, RAD, STOP, THRESH) T* xp = (T*)xv; SELFHASHTAB( \
    T, W, RAD, STOP,                                    \
    1, taga(cpyBitArr(r)), hash[j]=h; rp[i]=k!=h;,      \
    1, THRESH, 0,,,)
  if (lw == 5) {
    if (n<12) { BRUTE(32); }
    i8* rp; B r = m_i8arrv(&rp, n);
    HASHTAB(u32, 32, 1, n/2, sz==msz? 1 : sz>=(1<<15)? 3 : 5)

    // Radix-assisted lookup when hash table gives up
    usz rx = 256, tn = 1<<16; // Radix; table length
    u32* v0 = (u32*)xv;
    i8* r0 = rp;
    
    TALLOC(u8, alloc, 6*n+(4+(tn>3*n?tn:3*n)+(2*rx+1)*sizeof(usz)));
    //                                         timeline
    // Allocations               len  count radix hash deradix     bytes  layout:
    usz *c0 = (usz*)(alloc)+1; // rx   [+++................]     c0   rx  #
    usz *c1 = (usz*)(c0+rx);   // rx    [++................]     c1   rx   #
    u8  *k0 = (u8 *)(c1+rx);   //  n        [+.............]     k0    n    ##
    u32 *v2 = (u32*)(k0+n);    //  n+1       [+.......]          v2  4*n+4    ########
    u8  *k1 = (u8 *)(v2+n+1);  //  n         [+............]     k1    n              ##
    u32 *v1 = (u32*)(k1);      //  n        [+-]                 v1  4*n              ########
    u8  *r2 = (u8 *)(v2);      //  n              [+.....]       r2    n      ##
    u8  *r1 = (u8 *)(k1+n);    //  n                   [+..]     r1    n                ##
    u8  *tab= (u8 *)(r1);      // tn              [+]            tab  tn                #####
   
    RADIX_LOOKUP_32(1, =0)
    return taga(cpyBitArr(r));
  }
  if (lw == 6 && canCompare64_norm(x, n)) {
    if (n<20) { BRUTE(64); }
    i8* rp; B r = m_i8arrv(&rp, n);
    HASHTAB(u64, 64, 0, n, sz==msz? 0 : sz>=(1<<18)? 0 : sz>=(1<<14)? 3 : 5)
    decG(r); // Fall through
  }
  #undef HASHTAB
  #undef BRUTE
  
  if (RNK(x)>1) x = toCells(x);
  u64* rp; B r = m_bitarrv(&rp, n);
  H_Sb* set = m_Sb(64);
  SGetU(x)
  for (usz i = 0; i < n; i++) bitp_set(rp, i, !ins_Sb(&set, GetU(x,i)));
  free_Sb(set); decG(x);
  return r;
}

B count_c1(B t, B x) {
  if (isAtm(x) || RNK(x)==0) thrM("⊒: Argument cannot have rank 0");
  u64 n = *SH(x);
  if (n<=1) { decG(x); return n ? taga(arr_shVec(allZeroes(1))) : emptyIVec(); }
  if (n>(usz)I32_MAX+1) thrM("⊒: Argument length >2⋆31 not supported");
  
  u8 lw = cellWidthLog(x);
  if (lw==0) {
    u64* xp = bitarr_ptr(x);
    B r;
    #define COUNT_BOOL(T) \
      T* rp; r = m_##T##arrv(&rp, n);         \
      usz n1 = 0;                             \
      for (usz i=0; i<n; ) {                  \
        u64 bb = xp[i/64];                    \
        for (usz e=n-i<64?n:i+64; i<e; i++) { \
          bool b = bb&1; bb>>=1;              \
          rp[i] = b? n1 : i-n1;               \
          n1 += b;                            \
        }                                     \
      }
    if      (n <= 128)   { COUNT_BOOL(i8) }
    else if (n <= 1<<15) { COUNT_BOOL(i16) }
    else                 { COUNT_BOOL(i32) }
    decG(x); return r;
    #undef COUNT_LOOP
    #undef COUNT_BOOL
  }
  if (use_sorted(x, lw) && n>16 && (lw>4 || n<1<<16)) { // ↕∘≠(⊣-⌈`∘×)∊
    B c = shift_ne(x, n, lw, 1);
    B i = C1(ud, m_f64(n));
    B m = M1C1(scan, ceil, C2(mul, c, inc(i)));
    return C2(sub, i, m);
  }
  void* xv = tyany_ptr(x);
  #define BRUTE(T) \
    i##T* xp = xv;                                             \
    i8* rp; B r = m_i8arrv(&rp, n); rp[0]=0;                   \
    for (usz i=1; i<n; i++) {                                  \
      usz c=0; i##T xi=xp[i];                                  \
      PLAINLOOP for (usz j=0; j<i; j++) c += xi==xp[j];        \
      rp[i] = c;                                               \
    }                                                          \
    decG(x); return r;
  #define LOOKUP(T) \
    usz tn = 1<<T;                                             \
    u##T* xp = (u##T*)xv;                                      \
    i32* rp; B r = m_i32arrv(&rp, n);                          \
    TALLOC(i32, tab, tn);                                      \
    if (T>8 && n<tn/16) for (usz i=0; i<n;  i++) tab[xp[i]]=0; \
    else                for (usz j=0; j<tn; j++) tab[j]=0;     \
    for (usz i=0; i<n;  i++) rp[i]=tab[xp[i]]++;               \
    decG(x); TFREE(tab);                                       \
    return num_squeeze(r)
  if (lw==3) { if (n<12) { BRUTE(8); } else { LOOKUP(8); } }
  if (lw==4) { if (n<12) { BRUTE(16); } else { LOOKUP(16); } }
  #undef LOOKUP
  #define HASHTAB(T, W, RAD, STOP, THRESH) T* xp = (T*)xv; SELFHASHTAB_VAL( \
    T, W, RAD, STOP,                                       \
    /*RES0*/0, /*RESULT*/r,                                \
    /* RESWRITE */                                         \
    bool e0=h==x0; rp[i]=val[j]+(ctr0&-(u32)e0);           \
    hash[j]=h; val[j]+=!e0; ctr0+=e0; ,                    \
    /*THRESHMUL*/1, THRESH,                                \
    /*INIT*/u32 ctr0 = 1;)
  if (lw==5) {
    if (n<20) { BRUTE(32); }
    i32* rp; B r = m_i32arrv(&rp, n);
    HASHTAB(u32, 32, 1, n/2, sz==msz? 1 : sz>=(1<<14)? 3 : 5)
    // Radix-assisted lookup
    usz rx = 256, tn = 1<<16; // Radix; table length
    u32* v0 = (u32*)xv;
    i32* r0 = rp;
    
    TALLOC(u8, alloc, 6*n+(4+4*(tn>n?tn:n)+(2*rx+1)*sizeof(usz)));
    //                                         timeline
    // Allocations               len  count radix hash deradix     bytes  layout:
    usz *c0 = (usz*)(alloc)+1; // rx   [+++................]    c0    rx  #
    usz *c1 = (usz*)(c0+rx);   // rx    [++................]    c1    rx   #
    u8  *k0 = (u8 *)(c1+rx);   //  n        [+.............]    k0     n    ##
    u8  *k1 = (u8 *)(k0+n);    //  n         [+............]    k1     n      ##
    u32 *v2 = (u32*)(k1+n);    //  n+1       [+....-]           v2   4*n        ########
    u32 *v1 = (u32*)(v2+n+1);  //  n        [+..]               v1   4*n                ########
    u32 *r2 = (u32*)v2;        //  n              [+.....]      r2   4*n        ########
    u32 *r1 = (u32*)v1;        //  n                   [+..]    r1   4*n                ########
    u32 *tab= (u32*)v1;        // tn              [+]           tab 4*tn                ###########
    
    RADIX_LOOKUP_32(0, ++)
    return num_squeeze(r);
  }
  if (lw == 6 && canCompare64_norm(x, n)) {
    if (n<20) { BRUTE(64); }
    i32* rp; B r = m_i32arrv(&rp, n);
    HASHTAB(u64, 64, 0, n, sz==msz? 0 : sz>=(1<<18)? 0 : sz>=(1<<14)? 3 : 5)
    decG(r); // Fall through
  }
  #undef HASHTAB
  #undef BRUTE
  
  if (RNK(x)>1) x = toCells(x);
  i32* rp; B r = m_i32arrv(&rp, n);
  H_b2i* map = m_b2i(64);
  SGetU(x)
  for (usz i = 0; i < n; i++) {
    bool had; u64 p = mk_b2i(&map, GetU(x,i), &had);
    rp[i] = had? ++map->a[p].val : (map->a[p].val = 0);
  }
  decG(x); free_b2i(map);
  return r;
}

B indexOf_c1(B t, B x) {
  if (isAtm(x) || RNK(x)==0) thrM("⊐: 𝕩 cannot have rank 0");
  u64 n = *SH(x);
  if (n<=1) { decG(x); return n ? taga(arr_shVec(allZeroes(1))) : emptyIVec(); }
  if (n>(usz)I32_MAX+1) thrM("⊐: Argument length >2⋆31 not supported");
  
  u8 lw = cellWidthLog(x);
  void* xv = tyany_ptr(x);
  if (lw == 0) {
    B r = 1&*(u64*)xv ? C1(not, x) : x;
    return C1(shape, r);
  }
  if (use_sorted(x, lw) && n>8) {
    return M1C1(scan, add, shift_ne(x, n, lw, 0));
  }
  #define BRUTE(T) \
    i##T* xp = xv;                                             \
    i8* rp; B r = m_i8arrv(&rp, n); rp[0]=0;                   \
    TALLOC(i##T, uniq, n); uniq[0]=xp[0];                      \
    for (usz i=1, u=1; i<n; i++) {                             \
      bool c=1; usz s=0; i##T xi=uniq[u]=xp[i];                \
      for (usz j=0; j<u; j++) s += (c &= xi!=uniq[j]);         \
      rp[i]=s; u+=u==s;                                        \
    }                                                          \
    decG(x); TFREE(uniq); return r;
  #define DOTAB(T) \
    i32 u=0;                                                   \
    for (usz i=0; i<n; i++) {                                  \
      T j=xp[i]; i32 t=tab[j];                                 \
      if (t==n) tab[j]=u++;                                    \
      rp[i]=tab[j];                                            \
    }
  #define LOOKUP(T) \
    usz tn = 1<<T;                                             \
    u##T* xp = (u##T*)xv;                                      \
    i32* rp; B r = m_i32arrv(&rp, n);                          \
    TALLOC(i32, tab, tn);                                      \
    if (T>8 && n<tn/16) for (usz i=0; i<n;  i++) tab[xp[i]]=n; \
    else                for (usz j=0; j<tn; j++) tab[j]=n;     \
    DOTAB(u##T)                                                \
    decG(x); TFREE(tab);                                       \
    return num_squeeze(r)
  if (lw==3) { if (n<12) { BRUTE(8); } else { LOOKUP(8); } }
  if (lw==4) { if (n<12) { BRUTE(16); } else { LOOKUP(16); } }
  #undef LOOKUP
  
  #define HASHTAB(T, W, THRESH) SELFHASHTAB_VAL(T, W, 0, 2*n, \
    /*RES0*/0, /*RESULT*/r,                                \
    /* RESWRITE */                                         \
    if (k!=h) { val[j]=ctr++; hash[j]=h; } rp[i]=val[j]; , \
    /*THRESHMUL*/2, THRESH,                                \
    /*INIT*/u32 ctr = 1;)
  if (lw==5) {
    if (n<12) { BRUTE(32); }
    B r;
    i32* rp; r = m_i32arrv(&rp, n);
    i32* xp = tyany_ptr(x);
    i32 min=I32_MAX, max=I32_MIN;
    for (usz i = 0; i < n; i++) {
      i32 c = xp[i];
      if (c<min) min = c;
      if (c>max) max = c;
    }
    i64 dst = 1 + (max-(i64)min);
    if (dst<n*5 || dst<50) {
      TALLOC(i32, tmp, dst); i32* tab = tmp-min;
      for (i64 i = 0; i < dst; i++) tmp[i] = n;
      DOTAB(i32)
      TFREE(tmp);
      decG(x);
      return r;
    }
    HASHTAB(u32, 32, sz==msz? 0 : sz>=(1<<18)? 1 : sz>=(1<<14)? 4 : 6)
    decG(r); // Fall through
  }
  if (lw==6 && canCompare64_norm(x, n)) {
    if (n<16) { BRUTE(64); }
    i32* rp; B r = m_i32arrv(&rp, n);
    u64* xp = tyany_ptr(x);
    HASHTAB(u64, 64, sz==msz? 0 : sz>=(1<<17)? 1 : sz>=(1<<13)? 4 : 6)
    decG(r); // Fall through
  }
  #undef HASHTAB
  #undef BRUTE
  #undef DOTAB
  
  if (RNK(x)>1) x = toCells(x);
  i32* rp; B r = m_i32arrv(&rp, n);
  H_b2i* map = m_b2i(64);
  SGetU(x)
  i32 ctr = 0;
  for (usz i = 0; i < n; i++) {
    bool had; u64 p = mk_b2i(&map, GetU(x,i), &had);
    if (had) rp[i] = map->a[p].val;
    else     rp[i] = map->a[p].val = ctr++;
  }
  free_b2i(map); decG(x);
  return r;
}

B find_c1(B t, B x) {
  if (isAtm(x) || RNK(x)==0) thrM("⍷: Argument cannot have rank 0");
  usz n = *SH(x);
  if (n<=1) return x;
  return C2(slash, C1(memberOf, inc(x)), x);
}
