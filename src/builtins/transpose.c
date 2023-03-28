// Transpose and Reorder Axes (⍉)

// Transpose
// One length-2 axis: dedicated code
//   Boolean: pdep for height 2; pext for width 2
//     SHOULD use a generic implementation if BMI2 not present
// SHOULD optimize other short lengths with pdep/pext and shuffles
// Boolean 𝕩: convert to integer
//   SHOULD have bit matrix transpose kernel
// CPU sizes: native or SIMD code
//   Large SIMD kernels used when they fit, overlapping for odd sizes
//     i8: 16×16; i16: 16×8; i32: 8×8; f64: 4×4
//   COULD use half-width or smaller kernels to improve odd sizes
//   Scalar transpose or loop used for overhang of 1

// Reorder Axes: generate indices and select with +⌜ and ⊏

// Transpose inverse ⍉⁼
//   Same as ⍉ for a rank ≤2 argument
//   SHOULD share data movement with ⍉ for other sizes
// COULD implement fast ⍉⍟n
// SHOULD convert ⍉ with rank to a Reorder Axes call

#include "../core.h"
#include "../utils/each.h"
#include "../utils/talloc.h"
#include "../builtins.h"
#include "../utils/calls.h"

#ifdef __BMI2__
  #include <immintrin.h>
  #if USE_VALGRIND
    #define _pdep_u64 vg_pdep_u64
  #endif
#endif

#define TRANSPOSE_LOOP( DST, SRC,         W, H) PLAINLOOP for(usz y=0;y< H;y++) NOVECTORIZE for(usz x=0;x< W;x++) DST[x*H+y] = SRC[xi++]
#define TRANSPOSE_BLOCK(DST, SRC, BW, BH, W, H) PLAINLOOP for(usz y=0;y<BH;y++) NOVECTORIZE for(usz x=0;x<BW;x++) DST[x*H+y] = SRC[y*W+x]

#if SINGELI_X86_64
  #define DECL_BASE(T) \
    static NOINLINE void base_transpose_##T(T* rp, T* xp, u64 bw, u64 bh, u64 w, u64 h) { \
      TRANSPOSE_BLOCK(rp, xp, bw, bh, w, h); \
    }
  DECL_BASE(i8) DECL_BASE(i16) DECL_BASE(i32) DECL_BASE(i64)
  #undef DECL_BASE
  #define SINGELI_FILE transpose
  #include "../utils/includeSingeli.h"
  #define TRANSPOSE_SIMD(T, DST, SRC, W, H) simd_transpose_##T(DST, SRC, W, H)
#else
  #define TRANSPOSE_SIMD(T, DST, SRC, W, H) TRANSPOSE_LOOP(DST, SRC, W, H)
#endif


B transp_c1(B t, B x) {
  if (RARE(isAtm(x))) return m_atomUnit(x);
  ur xr = RNK(x);
  if (xr<=1) return x;
  
  usz ia = IA(x);
  usz* xsh = SH(x);
  usz h = xsh[0];
  if (ia==0 || h==1) {
    no_reorder:;
    Arr* r = cpyWithShape(x);
    ShArr* sh = m_shArr(xr);
    shcpy(sh->a, xsh+1, xr-1);
    sh->a[xr-1] = h;
    arr_shReplace(r, xr, sh);
    return taga(r);
  }
  usz w = xsh[1] * shProd(xsh, 2, xr);
  if (w==1) goto no_reorder;
  
  Arr* r;
  usz xi = 0;
  u8 xe = TI(x,elType);
  bool toBit = false;
  if (h==2) {
    if (xe==el_B) {
      B* xp = TO_BPTR(x);
      B* x0 = xp; B* x1 = x0+w;
      HArr_p rp = m_harrUp(ia);
      for (usz i=0; i<w; i++) { rp.a[i*2] = inc(x0[i]); rp.a[i*2+1] = inc(x1[i]); }
      NOGC_E;
      r = (Arr*) rp.c;
    } else {
      #ifndef __BMI2__
      if (xe==el_bit) { x = taga(cpyI8Arr(x)); xsh=SH(x); xe=el_i8; toBit=true; }
      void* rp = m_tyarrp(&r,elWidth(xe),ia,el2t(xe));
      #else
      void* rp = m_tyarrlbp(&r,elWidthLogBits(xe),ia,el2t(xe));
      #endif
      void* xp = tyany_ptr(x);
      switch(xe) { default: UD;
        #ifdef __BMI2__
        case el_bit:;
          u32* x0 = xp;
          Arr* x1o = TI(x,slice)(inc(x),w,w);
          u32* x1 = (u32*) ((TyArr*)x1o)->a;
          for (usz i=0; i<BIT_N(ia); i++) ((u64*)rp)[i] = _pdep_u64(x0[i], 0x5555555555555555) | _pdep_u64(x1[i], 0xAAAAAAAAAAAAAAAA);
          mm_free((Value*)x1o);
          break;
        #endif
        case el_i8: case el_c8:  { u8*  x0=xp; u8*  x1=x0+w; for (usz i=0; i<w; i++) { ((u8* )rp)[i*2] = x0[i]; ((u8* )rp)[i*2+1] = x1[i]; } } break;
        case el_i16:case el_c16: { u16* x0=xp; u16* x1=x0+w; for (usz i=0; i<w; i++) { ((u16*)rp)[i*2] = x0[i]; ((u16*)rp)[i*2+1] = x1[i]; } } break;
        case el_i32:case el_c32: { u32* x0=xp; u32* x1=x0+w; for (usz i=0; i<w; i++) { ((u32*)rp)[i*2] = x0[i]; ((u32*)rp)[i*2+1] = x1[i]; } } break;
        case el_f64:             { u64* x0=xp; u64* x1=x0+w; for (usz i=0; i<w; i++) { ((u64*)rp)[i*2] = x0[i]; ((u64*)rp)[i*2+1] = x1[i]; } } break;
      }
    }
  } else if (w==2 && xe!=el_B) {
    #ifndef __BMI2__
      if (xe==el_bit) { x = taga(cpyI8Arr(x)); xsh=SH(x); xe=el_i8; toBit=true; }
    #endif
    void* rp = m_tyarrlbp(&r,elWidthLogBits(xe),ia,el2t(xe));
    void* xp = tyany_ptr(x);
    switch(xe) { default: UD;
      #if __BMI2__
      case el_bit:;
        u64* r0 = rp; TALLOC(u64, r1, BIT_N(h));
        for (usz i=0; i<BIT_N(ia); i++) {
          u64 v = ((u64*)xp)[i];
          ((u32*)r0)[i] = _pext_u64(v, 0x5555555555555555);
          ((u32*)r1)[i] = _pext_u64(v, 0xAAAAAAAAAAAAAAAA);
        }
        bit_cpy(r0, h, r1, 0, h);
        TFREE(r1);
        break;
      #endif
      case el_i8: case el_c8:  { u8*  r0=rp; u8*  r1=r0+h; for (usz i=0; i<h; i++) { r0[i] = ((u8* )xp)[i*2]; r1[i] = ((u8* )xp)[i*2+1]; } } break;
      case el_i16:case el_c16: { u16* r0=rp; u16* r1=r0+h; for (usz i=0; i<h; i++) { r0[i] = ((u16*)xp)[i*2]; r1[i] = ((u16*)xp)[i*2+1]; } } break;
      case el_i32:case el_c32: { u32* r0=rp; u32* r1=r0+h; for (usz i=0; i<h; i++) { r0[i] = ((u32*)xp)[i*2]; r1[i] = ((u32*)xp)[i*2+1]; } } break;
      case el_f64:             { f64* r0=rp; f64* r1=r0+h; for (usz i=0; i<h; i++) { r0[i] = ((f64*)xp)[i*2]; r1[i] = ((f64*)xp)[i*2+1]; } } break;
    }
  } else {
    switch(xe) { default: UD;
      case el_bit: x = taga(cpyI8Arr(x)); xsh=SH(x); xe=el_i8; toBit=true; // fallthough
      case el_i8: case el_c8:  { u8*  xp=tyany_ptr(x); u8*  rp = m_tyarrp(&r,1,ia,el2t(xe)); TRANSPOSE_SIMD( i8, rp, xp, w, h); break; }
      case el_i16:case el_c16: { u16* xp=tyany_ptr(x); u16* rp = m_tyarrp(&r,2,ia,el2t(xe)); TRANSPOSE_SIMD(i16, rp, xp, w, h); break; }
      case el_i32:case el_c32: { u32* xp=tyany_ptr(x); u32* rp = m_tyarrp(&r,4,ia,el2t(xe)); TRANSPOSE_SIMD(i32, rp, xp, w, h); break; }
      case el_f64:             { f64* xp=f64any_ptr(x); f64* rp; r=m_f64arrp(&rp,ia);        TRANSPOSE_SIMD(i64, rp, xp, w, h); break; }
      case el_B: { // can't be bothered to implement a bitarr transpose
        B xf = getFillR(x);
        B* xp = TO_BPTR(x);
        
        HArr_p p = m_harrUp(ia);
        for(usz y=0;y<h;y++) for(usz x=0;x<w;x++) p.a[x*h+y] = inc(xp[xi++]); // TODO inc afterwards, but don't when there's a method of freeing a HArr without freeing its elements
        NOGC_E;
        
        usz* rsh = arr_shAlloc((Arr*)p.c, xr);
        if (xr==2) {
          rsh[0] = w;
          rsh[1] = h;
        } else {
          shcpy(rsh, xsh+1, xr-1);
          rsh[xr-1] = h;
        }
        decG(x); return qWithFill(p.b, xf);
      }
    }
  }
  usz* rsh = arr_shAlloc(r, xr);
  if (xr==2) {
    rsh[0] = w;
    rsh[1] = h;
  } else {
    shcpy(rsh, xsh+1, xr-1);
    rsh[xr-1] = h;
  }
  decG(x); return taga(toBit? (Arr*)cpyBitArr(taga(r)) : r);
}

B mul_c2(B,B,B);
B ud_c1(B,B);
B tbl_c2(Md1D*,B,B);
B select_c2(B,B,B);

B transp_c2(B t, B w, B x) {
  usz wia=1;
  if (isArr(w)) {
    if (RNK(w)>1) thrM("⍉: 𝕨 must have rank at most 1");
    wia = IA(w);
    if (wia==0) { decG(w); return isArr(x)? x : m_atomUnit(x); }
  }
  ur xr;
  if (isAtm(x) || (xr=RNK(x))<wia) thrM("⍉: Length of 𝕨 must be at most rank of 𝕩");

  // Axis permutation
  TALLOC(ur, p, xr);
  if (isAtm(w)) {
    usz a=o2s(w);
    if (a>=xr) thrF("⍉: Axis %s does not exist (%i≡=𝕩)", a, xr);
    if (a==xr-1) { TFREE(p); return C1(transp, x); }
    p[0] = a;
  } else {
    SGetU(w)
    for (usz i=0; i<wia; i++) {
      usz a=o2s(GetU(w, i));
      if (a>=xr) thrF("⍉: Axis %s does not exist (%i≡=𝕩)", a, xr);
      p[i] = a;
    }
    decG(w);
  }

  // compute shape for the given axes
  usz* xsh = SH(x);
  TALLOC(usz, rsh, xr);
  usz dup = 0, max = 0;
  usz no_sh = -(usz)1;
  for (usz j=0; j<xr; j++) rsh[j] = no_sh;
  for (usz i=0; i<wia; i++) {
    ur j=p[i];
    usz xl=xsh[i], l=rsh[j];
    dup += l!=no_sh;
    max = j>max? j : max;
    if (xl<l) rsh[j]=xl;
  }

  // fill in remaining axes and check for missing ones
  ur rr = xr-dup;
  if (max >= rr) thrF("⍉: Skipped result axis");
  if (wia<xr) for (usz j=0, i=wia; j<rr; j++) if (rsh[j]==no_sh) {
    p[i] = j;
    rsh[j] = xsh[i];
    i++;
  }

  B r;

  // Empty result
  if (IA(x) == 0) {
    Arr* ra = m_fillarrpEmpty(getFillQ(x));
    if (RARE(rr <= 1)) {
      arr_shVec(ra);
    } else {
      ShArr* sh=m_shArr(rr);
      shcpy(sh->a, rsh, rr);
      arr_shSetU(ra, rr, sh);
    }
    decG(x);
    r = taga(ra); goto ret;
  }

  // Number of axes that move
  ur ar = max+1+dup;
  if (!dup) while (ar>1 && p[ar-1]==ar-1) ar--; // Unmoved trailing
  if (ar <= 1) { r = x; goto ret; }
  // Add up stride for each axis
  TALLOC(u64, st, rr);
  for (usz j=0; j<rr; j++) st[j] = 0;
  usz c = 1;
  for (usz i=ar; i--; ) { st[p[i]]+=c; c*=xsh[i]; }

  // Reshape x for selection, collapsing ar axes
  if (ar != 1) {
    ur zr = xr-ar+1;
    ShArr* zsh;
    if (zr>1) {
      zsh = m_shArr(zr);
      zsh->a[0] = c;
      shcpy(zsh->a+1, xsh+ar, xr-ar);
    }
    Arr* z = TI(x,slice)(x, 0, IA(x));
    if (zr>1) arr_shSetU(z, zr, zsh);
    else arr_shVec(z);
    x = taga(z);
  }
  // (+⌜´st×⟜↕¨rsh)⊏⥊𝕩
  B ind = bi_N;
  for (ur k=ar-dup; k--; ) {
    B v = C2(mul, m_f64(st[k]), C1(ud, m_f64(rsh[k])));
    if (q_N(ind)) ind = v;
    else ind = M1C2(tbl, add, v, ind);
  }
  TFREE(st);
  r = C2(select, ind, x);

  ret:;
  TFREE(rsh);
  TFREE(p);
  return r;
}


B transp_im(B t, B x) {
  if (isAtm(x)) thrM("⍉⁼: 𝕩 must not be an atom");
  if (RNK(x)<=2) return transp_c1(t, x);
  return def_fn_im(bi_transp, x);
}

B transp_uc1(B t, B o, B x) {
  return transp_im(m_f64(0), c1(o,  transp_c1(t, x)));
}

void transp_init(void) {
  c(BFn,bi_transp)->uc1 = transp_uc1;
  c(BFn,bi_transp)->im = transp_im;
}
