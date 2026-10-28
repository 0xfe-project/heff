// The float functions the runtime carries itself, bundled after the rest
// of it: sin, cos, tan, exp and log, fdlibm's (5.3, Sun's "Freely
// Distributable Math Library"), as V8 has them for JavaScript's `Math` --
// so that a program computes the same float on every platform, whatever
// libm it would be linked with (zig's own on Linux and with musl, the
// system's on macOS), and the same as the JavaScript backends.  One thing
// is V8's, not fdlibm's: `exp(1)` is e, correctly rounded, where fdlibm's
// is a unit in the last place off.  (sqrt is IEEE's, correctly rounded by
// every libm; asin, acos, atan, atan2 and pow are still the libm's.)
//
// Each function is written as fdlibm has it, its names `heff_`'s, its
// words got at through a union, and floats never contracted (`a*b + c`
// is not fused into one rounding: fdlibm's arithmetic is IEEE's, step by
// step, as V8's is).  Its constants are within the functions, which makes
// them a part's own where a program is split into parts.

/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

// (a double's high and low words, and a double of two words)
#define HEFF_FDL_BITS(x) (((union { double d; uint64_t u; }){.d = (x)}).u)
#define HEFF_FDL_HI(x) ((int32_t)(HEFF_FDL_BITS(x) >> 32))
#define HEFF_FDL_LO(x) ((uint32_t)HEFF_FDL_BITS(x))
#define HEFF_FDL_WORDS(hi, lo) \
  (((union { uint64_t u; double d; }){.u = (uint64_t)(uint32_t)(hi) << 32 | (uint32_t)(lo)}).d)
#define HEFF_FDL_SET_LO(x, lo) HEFF_FDL_WORDS(HEFF_FDL_HI(x), lo)

/*
 * __kernel_sin( x, y, iy)
 * kernel sin function on [-pi/4, pi/4], pi/4 ~ 0.7854
 * Input x is assumed to be bounded by ~pi/4 in magnitude.
 * Input y is the tail of x.
 * Input iy indicates whether y is 0. (if iy=0, y assume to be 0).
 *
 * Algorithm
 *	1. Since sin(-x) = -sin(x), we need only to consider positive x.
 *	2. if x < 2^-27 (hx<0x3e400000 0), return x with inexact if x!=0.
 *	3. sin(x) is approximated by a polynomial of degree 13 on
 *	   [0,pi/4]
 *		  	         3            13
 *	   	sin(x) ~ x + S1*x + ... + S6*x
 *	   where
 *
 * 	|sin(x)         2     4     6     8     10     12  |     -58
 * 	|----- - (1+S1*x +S2*x +S3*x +S4*x +S5*x  +S6*x   )| <= 2
 * 	|  x 					           |
 *
 *	4. sin(x+y) = sin(x) + sin'(x')*y
 *		    ~ sin(x) + (1-x*x/2)*y
 *	   For better accuracy, let
 *		     3      2      2      2      2
 *		r = x *(S2+x *(S3+x *(S4+x *(S5+x *S6))))
 *	   then                   3    2
 *		sin(x) = x + (S1*x + (x *(r-y/2)+y))
 */
HEFF_FN double heff_fdl_kernel_sin(double x, double y, int iy) {
#pragma STDC FP_CONTRACT OFF
  static const double
      half = 5.00000000000000000000e-01, /* 0x3FE00000, 0x00000000 */
      S1 = -1.66666666666666324348e-01,  /* 0xBFC55555, 0x55555549 */
      S2 = 8.33333333332248946124e-03,   /* 0x3F811111, 0x1110F8A6 */
      S3 = -1.98412698298579493134e-04,  /* 0xBF2A01A0, 0x19C161D5 */
      S4 = 2.75573137070700676789e-06,   /* 0x3EC71DE3, 0x57B1FE7D */
      S5 = -2.50507602534068634195e-08,  /* 0xBE5AE5E6, 0x8A2B9CEB */
      S6 = 1.58969099521155010221e-10;   /* 0x3DE5D93A, 0x5ACFD57C */
  double z, r, v;
  int32_t ix;
  ix = HEFF_FDL_HI(x) & 0x7fffffff; /* high word of x */
  if (ix < 0x3e400000) {            /* |x| < 2**-27 */
    if ((int)x == 0) return x;      /* generate inexact */
  }
  z = x * x;
  v = z * x;
  r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  if (iy == 0) return x + v * (S1 + z * r);
  else return x - ((z * (half * y - v * r) - y) - v * S1);
}

/*
 * __kernel_cos( x,  y )
 * kernel cos function on [-pi/4, pi/4], pi/4 ~ 0.785398164
 * Input x is assumed to be bounded by ~pi/4 in magnitude.
 * Input y is the tail of x.
 *
 * Algorithm
 *	1. Since cos(-x) = cos(x), we need only to consider positive x.
 *	2. if x < 2^-27 (hx<0x3e400000 0), return 1 with inexact if x!=0.
 *	3. cos(x) is approximated by a polynomial of degree 14 on
 *	   [0,pi/4]
 *		  	                 4            14
 *	   	cos(x) ~ 1 - x*x/2 + C1*x + ... + C6*x
 *	   where the remez error is
 *
 * 	|              2     4     6     8     10    12     14 |     -58
 * 	|cos(x)-(1-.5*x +C1*x +C2*x +C3*x +C4*x +C5*x  +C6*x  )| <= 2
 * 	|    					               |
 *
 * 	               4     6     8     10    12     14
 *	4. let r = C1*x +C2*x +C3*x +C4*x +C5*x  +C6*x  , then
 *	       cos(x) = 1 - x*x/2 + r
 *	   since cos(x+y) ~ cos(x) - sin(x)*y
 *			  ~ cos(x) - x*y,
 *	   a correction term is necessary in cos(x) and hence
 *		cos(x+y) = 1 - (x*x/2 - (r - x*y))
 *	   For better accuracy when x > 0.3, let qx = |x|/4 with
 *	   the last 32 bits mask off, and if x > 0.78125, let qx = 0.28125.
 *	   Then
 *		cos(x+y) = (1-qx) - ((x*x/2-qx) - (r-x*y)).
 *	   Note that 1-qx and (x*x/2-qx) is EXACT here, and the
 *	   magnitude of the latter is at least a quarter of x*x/2,
 *	   thus, reducing the rounding error in the subtraction.
 */
HEFF_FN double heff_fdl_kernel_cos(double x, double y) {
#pragma STDC FP_CONTRACT OFF
  static const double
      one = 1.00000000000000000000e+00, /* 0x3FF00000, 0x00000000 */
      C1 = 4.16666666666666019037e-02,  /* 0x3FA55555, 0x5555554C */
      C2 = -1.38888888888741095749e-03, /* 0xBF56C16C, 0x16C15177 */
      C3 = 2.48015872894767294178e-05,  /* 0x3EFA01A0, 0x19CB1590 */
      C4 = -2.75573143513906633035e-07, /* 0xBE927E4F, 0x809C52AD */
      C5 = 2.08757232129817482790e-09,  /* 0x3E21EE9E, 0xBDB4B1C4 */
      C6 = -1.13596475577881948265e-11; /* 0xBDA8FAE9, 0xBE8838D4 */
  double a, hz, z, r, qx;
  int32_t ix;
  ix = HEFF_FDL_HI(x) & 0x7fffffff; /* ix = |x|'s high word*/
  if (ix < 0x3e400000) {            /* if x < 2**27 */
    if (((int)x) == 0) return one;  /* generate inexact */
  }
  z = x * x;
  r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  if (ix < 0x3FD33333) /* if |x| < 0.3 */
    return one - (0.5 * z - (z * r - x * y));
  else {
    if (ix > 0x3fe90000) { /* x > 0.78125 */
      qx = 0.28125;
    } else {
      qx = HEFF_FDL_WORDS(ix - 0x00200000, 0); /* x/4 */
    }
    hz = 0.5 * z - qx;
    a = one - qx;
    return a - (hz - (z * r - x * y));
  }
}

/* __kernel_tan( x, y, k )
 * kernel tan function on [-pi/4, pi/4], pi/4 ~ 0.7854
 * Input x is assumed to be bounded by ~pi/4 in magnitude.
 * Input y is the tail of x.
 * Input k indicates whether tan (if k = 1) or -1/tan (if k = -1) is returned.
 *
 * Algorithm
 *	1. Since tan(-x) = -tan(x), we need only to consider positive x.
 *	2. if x < 2^-28 (hx<0x3e300000 0), return x with inexact if x!=0.
 *	3. tan(x) is approximated by a odd polynomial of degree 27 on
 *	   [0,0.67434]
 *		  	         3             27
 *	   	tan(x) ~ x + T1*x + ... + T13*x
 *	   where
 *
 * 	        |tan(x)         2     4            26   |     -59.2
 * 	        |----- - (1+T1*x +T2*x +.... +T13*x    )| <= 2
 * 	        |  x 					|
 *
 *	   Note: tan(x+y) = tan(x) + tan'(x)*y
 *		          ~ tan(x) + (1+x*x)*y
 *	   Therefore, for better accuracy in computing tan(x+y), let
 *		     3      2      2       2       2
 *		r = x *(T2+x *(T3+x *(...+x *(T12+x *T13))))
 *	   then
 *		 		    3    2
 *		tan(x+y) = x + (T1*x + (x *(r+y)+y))
 *
 *      4. For x in [0.67434,pi/4],  let y = pi/4 - x, then
 *		tan(x) = tan(pi/4-y) = (1-tan(y))/(1+tan(y))
 *		       = 1 - 2*(tan(y) - (tan(y)^2)/(1+tan(y)))
 */
HEFF_FN double heff_fdl_kernel_tan(double x, double y, int iy) {
#pragma STDC FP_CONTRACT OFF
  static const double T[] = {
      3.33333333333334091986e-01,  /* 3FD55555, 55555563 */
      1.33333333333201242699e-01,  /* 3FC11111, 1110FE7A */
      5.39682539762260521377e-02,  /* 3FABA1BA, 1BB341FE */
      2.18694882948595424599e-02,  /* 3F9664F4, 8406D637 */
      8.86323982359930005737e-03,  /* 3F8226E3, E96E8493 */
      3.59207910759131235356e-03,  /* 3F6D6D22, C9560328 */
      1.45620945432529025516e-03,  /* 3F57DBC8, FEE08315 */
      5.88041240820264096874e-04,  /* 3F4344D8, F2F26501 */
      2.46463134818469906812e-04,  /* 3F3026F7, 1A8D1068 */
      7.81794442939557092300e-05,  /* 3F147E88, A03792A6 */
      7.14072491382608190305e-05,  /* 3F12B80F, 32F0A7E9 */
      -1.85586374855275456654e-05, /* BEF375CB, DB605373 */
      2.59073051863633712884e-05,  /* 3EFB2A70, 74BF7AD4 */
  };
  static const double
      one = 1.00000000000000000000e+00,    /* 3FF00000, 00000000 */
      pio4 = 7.85398163397448278999e-01,   /* 3FE921FB, 54442D18 */
      pio4lo = 3.06161699786838301793e-17; /* 3C81A626, 33145C07 */
  double z, r, v, w, s;
  int32_t ix, hx;
  hx = HEFF_FDL_HI(x);   /* high word of x */
  ix = hx & 0x7fffffff;  /* high word of |x| */
  if (ix < 0x3e300000) { /* x < 2**-28 */
    if ((int)x == 0) {   /* generate inexact */
      if (((ix | HEFF_FDL_LO(x)) | (iy + 1)) == 0) return one / fabs(x);
      else {
        if (iy == 1) return x;
        else { /* compute -1 / (x+y) carefully */
          double a, t;
          z = w = x + y;
          z = HEFF_FDL_SET_LO(z, 0);
          v = y - (z - x);
          t = a = -one / w;
          t = HEFF_FDL_SET_LO(t, 0);
          s = one + t * z;
          return t + a * (s + t * v);
        }
      }
    }
  }
  if (ix >= 0x3FE59428) { /* |x| >= 0.6744 */
    if (hx < 0) {
      x = -x;
      y = -y;
    }
    z = pio4 - x;
    w = pio4lo - y;
    x = z + w;
    y = 0.0;
  }
  z = x * x;
  w = z * z;
  /*
   * Break x^5*(T[1]+x^2*T[2]+...) into
   * x^5(T[1]+x^4*T[3]+...+x^20*T[11]) +
   * x^5(x^2*(T[2]+x^4*T[4]+...+x^22*[T12]))
   */
  r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
  v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
  s = z * x;
  r = y + z * (s * (r + v) + y);
  r += T[0] * s;
  w = x + r;
  if (ix >= 0x3FE59428) {
    v = (double)iy;
    return (double)(1 - ((hx >> 30) & 2)) * (v - 2.0 * (x - (w * w / (w + v) - r)));
  }
  if (iy == 1) return w;
  else {
    /*
     * if allow error up to 2 ulp, simply return
     * -1.0 / (x+r) here
     */
    /* compute -1.0 / (x+r) accurately */
    double a, t;
    z = w;
    z = HEFF_FDL_SET_LO(z, 0);
    v = r - (z - x);  /* z+v = r+x */
    t = a = -1.0 / w; /* a = -1.0/w */
    t = HEFF_FDL_SET_LO(t, 0);
    s = 1.0 + t * z;
    return t + a * (s + t * v);
  }
}

/*
 * __kernel_rem_pio2(x,y,e0,nx,prec,ipio2)
 * double x[],y[]; int e0,nx,prec; int ipio2[];
 *
 * __kernel_rem_pio2 return the last three digits of N with
 *		y = x - N*pi/2
 * so that |y| < pi/2.
 *
 * The method is to compute the integer (mod 8) and fraction parts of
 * (2/pi)*x without doing the full multiplication. In general we
 * skip the part of the product that are known to be a huge integer (
 * more accurately, = 0 mod 8 ). Thus the number of operations are
 * independent of the exponent of the input.
 *
 * (2/pi) is represented by an array of 24-bit integers in ipio2[].
 *
 * Input parameters:
 * 	x[]	The input value (must be positive) is broken into nx
 *		pieces of 24-bit integers in double precision format.
 *		x[i] will be the i-th 24 bit of x. The scaled exponent
 *		of x[0] is given in input parameter e0 (i.e., x[0]*2^e0
 *		match x's up to 24 bits.
 *
 *		Example of breaking a double positive z into x[0]+x[1]+x[2]:
 *			e0 = ilogb(z)-23
 *			z  = scalbn(z,-e0)
 *		for i = 0,1,2
 *			x[i] = floor(z)
 *			z    = (z-x[i])*2**24
 *
 *
 *	y[]	output result in an array of double precision numbers.
 *		The dimension of y[] is:
 *			24-bit  precision	1
 *			53-bit  precision	2
 *			64-bit  precision	2
 *			113-bit precision	3
 *		The actual value is the sum of them. Thus for 113-bit
 *		precison, one may have to do something like:
 *
 *		long double t,w,r_head, r_tail;
 *		t = (long double)y[2] + (long double)y[1];
 *		w = (long double)y[0];
 *		r_head = t+w;
 *		r_tail = w - (r_head - t);
 *
 *	e0	The exponent of x[0]
 *
 *	nx	dimension of x[]
 *
 *  	prec	an integer indicating the precision:
 *			0	24  bits (single)
 *			1	53  bits (double)
 *			2	64  bits (extended)
 *			3	113 bits (quad)
 *
 *	ipio2[]
 *		integer array, contains the (24*i)-th to (24*i+23)-th
 *		bit of 2/pi after binary point. The corresponding
 *		floating value is
 *
 *			ipio2[i] * 2^(-24(i+1)).
 *
 * External function:
 *	double scalbn(), floor();
 *
 *
 * Here is the description of some local variables:
 *
 * 	jk	jk+1 is the initial number of terms of ipio2[] needed
 *		in the computation. The recommended value is 2,3,4,
 *		6 for single, double, extended,and quad.
 *
 * 	jz	local integer variable indicating the number of
 *		terms of ipio2[] used.
 *
 *	jx	nx - 1
 *
 *	jv	index for pointing to the suitable ipio2[] for the
 *		computation. In general, we want
 *			( 2^e0*x[0] * ipio2[jv-1]*2^(-24jv) )/8
 *		is an integer. Thus
 *			e0-3-24*jv >= 0 or (e0-3)/24 >= jv
 *		Hence jv = max(0,(e0-3)/24).
 *
 *	jp	jp+1 is the number of terms in PIo2[] needed, jp = jk.
 *
 * 	q[]	double array with integral value, representing the
 *		24-bits chunk of the product of x and 2/pi.
 *
 *	q0	the corresponding exponent of q[0]. Note that the
 *		exponent for q[i] would be q0-24*i.
 *
 *	PIo2[]	double precision array, obtained by cutting pi/2
 *		into 24 bits chunks.
 *
 *	f[]	ipio2[] in floating point
 *
 *	iq[]	integer array by breaking up q[] in 24-bits chunk.
 *
 *	fq[]	final product of x*(2/pi) in fq[0],..,fq[jk]
 *
 *	ih	integer. If >0 it indicates q[] is >= 0.5, hence
 *		it also indicates the *sign* of the result.
 *
 */
HEFF_FN int heff_fdl_kernel_rem_pio2(double *x, double *y, int e0, int nx, int prec,
                                     const int32_t *ipio2) {
#pragma STDC FP_CONTRACT OFF
  static const int init_jk[] = {2, 3, 4, 6}; /* initial value for jk */
  static const double PIo2[] = {
      1.57079625129699707031e+00, /* 0x3FF921FB, 0x40000000 */
      7.54978941586159635335e-08, /* 0x3E74442D, 0x00000000 */
      5.39030252995776476554e-15, /* 0x3CF84698, 0x80000000 */
      3.28200341580791294123e-22, /* 0x3B78CC51, 0x60000000 */
      1.27065575308067607349e-29, /* 0x39F01B83, 0x80000000 */
      1.22933308981111328932e-36, /* 0x387A2520, 0x40000000 */
      2.73370053816464559624e-44, /* 0x36E38222, 0x80000000 */
      2.16741683877804819444e-51, /* 0x3569F31D, 0x00000000 */
  };
  static const double
      zero = 0.0,
      one = 1.0,
      two24 = 1.67772160000000000000e+07,  /* 0x41700000, 0x00000000 */
      twon24 = 5.96046447753906250000e-08; /* 0x3E700000, 0x00000000 */
  int jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
  double z, fw, f[20], fq[20], q[20];

  /* initialize jk*/
  jk = init_jk[prec];
  jp = jk;

  /* determine jx,jv,q0, note that 3>q0 */
  jx = nx - 1;
  jv = (e0 - 3) / 24;
  if (jv < 0) jv = 0;
  q0 = e0 - 24 * (jv + 1);

  /* set up f[0] to f[jx+jk] where f[jx+jk] = ipio2[jv+jk] */
  j = jv - jx;
  m = jx + jk;
  for (i = 0; i <= m; i++, j++) f[i] = (j < 0) ? zero : (double)ipio2[j];

  /* compute q[0],q[1],...q[jk] */
  for (i = 0; i <= jk; i++) {
    for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
    q[i] = fw;
  }

  jz = jk;
recompute:
  /* distill q[] into iq[] reversingly */
  for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--) {
    fw = (double)((int)(twon24 * z));
    iq[i] = (int)(z - two24 * fw);
    z = q[j - 1] + fw;
  }

  /* compute n */
  z = scalbn(z, q0);              /* actual value of z */
  z -= 8.0 * floor(z * 0.125);    /* trim off integer >= 8 */
  n = (int)z;
  z -= (double)n;
  ih = 0;
  if (q0 > 0) { /* need iq[jz-1] to determine n */
    i = (iq[jz - 1] >> (24 - q0));
    n += i;
    iq[jz - 1] -= i << (24 - q0);
    ih = iq[jz - 1] >> (23 - q0);
  } else if (q0 == 0) ih = iq[jz - 1] >> 23;
  else if (z >= 0.5) ih = 2;

  if (ih > 0) { /* q > 0.5 */
    n += 1;
    carry = 0;
    for (i = 0; i < jz; i++) { /* compute 1-q */
      j = iq[i];
      if (carry == 0) {
        if (j != 0) {
          carry = 1;
          iq[i] = 0x1000000 - j;
        }
      } else iq[i] = 0xffffff - j;
    }
    if (q0 > 0) { /* rare case: chance is 1 in 12 */
      switch (q0) {
      case 1: iq[jz - 1] &= 0x7fffff; break;
      case 2: iq[jz - 1] &= 0x3fffff; break;
      }
    }
    if (ih == 2) {
      z = one - z;
      if (carry != 0) z -= scalbn(one, q0);
    }
  }

  /* check if recomputation is needed */
  if (z == zero) {
    j = 0;
    for (i = jz - 1; i >= jk; i--) j |= iq[i];
    if (j == 0) { /* need recomputation */
      for (k = 1; iq[jk - k] == 0; k++); /* k = no. of terms needed */

      for (i = jz + 1; i <= jz + k; i++) { /* add q[jz+1] to q[jz+k] */
        f[jx + i] = (double)ipio2[jv + i];
        for (j = 0, fw = 0.0; j <= jx; j++) fw += x[j] * f[jx + i - j];
        q[i] = fw;
      }
      jz += k;
      goto recompute;
    }
  }

  /* chop off zero terms */
  if (z == 0.0) {
    jz -= 1;
    q0 -= 24;
    while (iq[jz] == 0) {
      jz--;
      q0 -= 24;
    }
  } else { /* break z into 24-bit if necessary */
    z = scalbn(z, -q0);
    if (z >= two24) {
      fw = (double)((int)(twon24 * z));
      iq[jz] = (int)(z - two24 * fw);
      jz += 1;
      q0 += 24;
      iq[jz] = (int)fw;
    } else iq[jz] = (int)z;
  }

  /* convert integer "bit" chunk to floating-point value */
  fw = scalbn(one, q0);
  for (i = jz; i >= 0; i--) {
    q[i] = fw * (double)iq[i];
    fw *= twon24;
  }

  /* compute PIo2[0,...,jp]*q[jz,...,0] */
  for (i = jz; i >= 0; i--) {
    for (fw = 0.0, k = 0; k <= jp && k <= jz - i; k++) fw += PIo2[k] * q[i + k];
    fq[jz - i] = fw;
  }

  /* compress fq[] into y[] */
  switch (prec) {
  case 0:
    fw = 0.0;
    for (i = jz; i >= 0; i--) fw += fq[i];
    y[0] = (ih == 0) ? fw : -fw;
    break;
  case 1:
  case 2:
    fw = 0.0;
    for (i = jz; i >= 0; i--) fw += fq[i];
    y[0] = (ih == 0) ? fw : -fw;
    fw = fq[0] - fw;
    for (i = 1; i <= jz; i++) fw += fq[i];
    y[1] = (ih == 0) ? fw : -fw;
    break;
  case 3: /* painful */
    for (i = jz; i > 0; i--) {
      fw = fq[i - 1] + fq[i];
      fq[i] += fq[i - 1] - fw;
      fq[i - 1] = fw;
    }
    for (i = jz; i > 1; i--) {
      fw = fq[i - 1] + fq[i];
      fq[i] += fq[i - 1] - fw;
      fq[i - 1] = fw;
    }
    for (fw = 0.0, i = jz; i >= 2; i--) fw += fq[i];
    if (ih == 0) {
      y[0] = fq[0];
      y[1] = fq[1];
      y[2] = fw;
    } else {
      y[0] = -fq[0];
      y[1] = -fq[1];
      y[2] = -fw;
    }
  }
  return n & 7;
}

/* __ieee754_rem_pio2(x,y)
 *
 * return the remainder of x rem pi/2 in y[0]+y[1]
 * use __kernel_rem_pio2()
 */
HEFF_FN int heff_fdl_rem_pio2(double x, double *y) {
#pragma STDC FP_CONTRACT OFF
  /*
   * Table of constants for 2/pi, 396 Hex digits (476 decimal) of 2/pi
   */
  static const int32_t two_over_pi[] = {
      0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 0x95993C, 0x439041, 0xFE5163,
      0xABDEBB, 0xC561B7, 0x246E3A, 0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129,
      0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41, 0x3991D6, 0x398353, 0x39F49C,
      0x845F8B, 0xBDF928, 0x3B1FF8, 0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF,
      0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5, 0xF17B3D, 0x0739F7, 0x8A5292,
      0xEA6BFB, 0x5FB11F, 0x8D5D08, 0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3,
      0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 0x4D7327, 0x310606, 0x1556CA,
      0x73A8C9, 0x60E27B, 0xC08C6B,
  };
  static const int32_t npio2_hw[] = {
      0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C, 0x4025FDBB,
      0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C, 0x40346B9C, 0x4035FDBB,
      0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A, 0x403DD85A, 0x403F6A7A, 0x40407E4C,
      0x4041475C, 0x4042106C, 0x4042D97C, 0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB,
      0x4046C6CB, 0x40478FDB, 0x404858EB, 0x404921FB,
  };
  /*
   * invpio2:  53 bits of 2/pi
   * pio2_1:   first  33 bit of pi/2
   * pio2_1t:  pi/2 - pio2_1
   * pio2_2:   second 33 bit of pi/2
   * pio2_2t:  pi/2 - (pio2_1+pio2_2)
   * pio2_3:   third  33 bit of pi/2
   * pio2_3t:  pi/2 - (pio2_1+pio2_2+pio2_3)
   */
  static const double
      zero = 0.00000000000000000000e+00,    /* 0x00000000, 0x00000000 */
      half = 5.00000000000000000000e-01,    /* 0x3FE00000, 0x00000000 */
      two24 = 1.67772160000000000000e+07,   /* 0x41700000, 0x00000000 */
      invpio2 = 6.36619772367581382433e-01, /* 0x3FE45F30, 0x6DC9C883 */
      pio2_1 = 1.57079632673412561417e+00,  /* 0x3FF921FB, 0x54400000 */
      pio2_1t = 6.07710050650619224932e-11, /* 0x3DD0B461, 0x1A626331 */
      pio2_2 = 6.07710050630396597660e-11,  /* 0x3DD0B461, 0x1A600000 */
      pio2_2t = 2.02226624879595063154e-21, /* 0x3BA3198A, 0x2E037073 */
      pio2_3 = 2.02226624871116645580e-21,  /* 0x3BA3198A, 0x2E000000 */
      pio2_3t = 8.47842766036889956997e-32; /* 0x397B839A, 0x252049C1 */
  double z, w, t, r, fn;
  double tx[3];
  int32_t e0, i, j, nx, n, ix, hx;

  hx = HEFF_FDL_HI(x); /* high word of x */
  ix = hx & 0x7fffffff;
  if (ix <= 0x3fe921fb) { /* |x| ~<= pi/4 , no need for reduction */
    y[0] = x;
    y[1] = 0;
    return 0;
  }
  if (ix < 0x4002d97c) { /* |x| < 3pi/4, special case with n=+-1 */
    if (hx > 0) {
      z = x - pio2_1;
      if (ix != 0x3ff921fb) { /* 33+53 bit pi is good enough */
        y[0] = z - pio2_1t;
        y[1] = (z - y[0]) - pio2_1t;
      } else { /* near pi/2, use 33+33+53 bit pi */
        z -= pio2_2;
        y[0] = z - pio2_2t;
        y[1] = (z - y[0]) - pio2_2t;
      }
      return 1;
    } else { /* negative x */
      z = x + pio2_1;
      if (ix != 0x3ff921fb) { /* 33+53 bit pi is good enough */
        y[0] = z + pio2_1t;
        y[1] = (z - y[0]) + pio2_1t;
      } else { /* near pi/2, use 33+33+53 bit pi */
        z += pio2_2;
        y[0] = z + pio2_2t;
        y[1] = (z - y[0]) + pio2_2t;
      }
      return -1;
    }
  }
  if (ix <= 0x413921fb) { /* |x| ~<= 2^19*(pi/2), medium size */
    t = fabs(x);
    n = (int32_t)(t * invpio2 + half);
    fn = (double)n;
    r = t - fn * pio2_1;
    w = fn * pio2_1t; /* 1st round good to 85 bit */
    if (n < 32 && ix != npio2_hw[n - 1]) {
      y[0] = r - w; /* quick check no cancellation */
    } else {
      j = ix >> 20;
      y[0] = r - w;
      i = j - ((HEFF_FDL_HI(y[0]) >> 20) & 0x7ff);
      if (i > 16) { /* 2nd iteration needed, good to 118 */
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        i = j - ((HEFF_FDL_HI(y[0]) >> 20) & 0x7ff);
        if (i > 49) { /* 3rd iteration need, 151 bits acc */
          t = r;      /* will cover all possible cases */
          w = fn * pio2_3;
          r = t - w;
          w = fn * pio2_3t - ((t - r) - w);
          y[0] = r - w;
        }
      }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) {
      y[0] = -y[0];
      y[1] = -y[1];
      return -n;
    } else return n;
  }
  /*
   * all other (large) arguments
   */
  if (ix >= 0x7ff00000) { /* x is inf or NaN */
    y[0] = y[1] = x - x;
    return 0;
  }
  /* set z = scalbn(|x|,ilogb(x)-23) */
  e0 = (ix >> 20) - 1046; /* e0 = ilogb(z)-23; */
  z = HEFF_FDL_WORDS(ix - (e0 << 20), HEFF_FDL_LO(x));
  for (i = 0; i < 2; i++) {
    tx[i] = (double)((int32_t)(z));
    z = (z - tx[i]) * two24;
  }
  tx[2] = z;
  nx = 3;
  while (tx[nx - 1] == zero) nx--; /* skip zero term */
  n = heff_fdl_kernel_rem_pio2(tx, y, e0, nx, 2, two_over_pi);
  if (hx < 0) {
    y[0] = -y[0];
    y[1] = -y[1];
    return -n;
  }
  return n;
}

/* sin(x)
 * Return sine function of x.
 *
 * kernel function:
 *	__kernel_sin		... sine function on [-pi/4,pi/4]
 *	__kernel_cos		... cose function on [-pi/4,pi/4]
 *	__ieee754_rem_pio2	... argument reduction routine
 *
 * Method.
 *      Let S,C and T denote the sin, cos and tan respectively on
 *	[-PI/4, +PI/4]. Reduce the argument x to y1+y2 = x-k*pi/2
 *	in [-pi/4 , +pi/4], and let n = k mod 4.
 *	We have
 *
 *          n        sin(x)      cos(x)        tan(x)
 *     ----------------------------------------------------------
 *	    0	       S	   C		 T
 *	    1	       C	  -S		-1/T
 *	    2	      -S	  -C		 T
 *	    3	      -C	   S		-1/T
 *     ----------------------------------------------------------
 *
 * Special cases:
 *      Let trig be any of sin, cos, or tan.
 *      trig(+-INF)  is NaN, with signals;
 *      trig(NaN)    is that NaN;
 *
 * Accuracy:
 *	TRIG(x) returns trig(x) nearly rounded
 */
HEFF_FN double heff_sin(double x) {
  double y[2], z = 0.0;
  int32_t n, ix;
  ix = HEFF_FDL_HI(x) & 0x7fffffff;                              /* |x| ~< pi/4 */
  if (ix <= 0x3fe921fb) return heff_fdl_kernel_sin(x, z, 0);
  else if (ix >= 0x7ff00000) return x - x;                        /* sin(Inf or NaN) is NaN */
  else {                                                          /* argument reduction needed */
    n = heff_fdl_rem_pio2(x, y);
    switch (n & 3) {
    case 0: return heff_fdl_kernel_sin(y[0], y[1], 1);
    case 1: return heff_fdl_kernel_cos(y[0], y[1]);
    case 2: return -heff_fdl_kernel_sin(y[0], y[1], 1);
    default: return -heff_fdl_kernel_cos(y[0], y[1]);
    }
  }
}

/* cos(x)
 * Return cosine function of x (as sin(x) above says).
 */
HEFF_FN double heff_cos(double x) {
  double y[2], z = 0.0;
  int32_t n, ix;
  ix = HEFF_FDL_HI(x) & 0x7fffffff;                              /* |x| ~< pi/4 */
  if (ix <= 0x3fe921fb) return heff_fdl_kernel_cos(x, z);
  else if (ix >= 0x7ff00000) return x - x;                        /* cos(Inf or NaN) is NaN */
  else {                                                          /* argument reduction needed */
    n = heff_fdl_rem_pio2(x, y);
    switch (n & 3) {
    case 0: return heff_fdl_kernel_cos(y[0], y[1]);
    case 1: return -heff_fdl_kernel_sin(y[0], y[1], 1);
    case 2: return -heff_fdl_kernel_cos(y[0], y[1]);
    default: return heff_fdl_kernel_sin(y[0], y[1], 1);
    }
  }
}

/* tan(x)
 * Return tangent function of x (as sin(x) above says).
 */
HEFF_FN double heff_tan(double x) {
  double y[2], z = 0.0;
  int32_t n, ix;
  ix = HEFF_FDL_HI(x) & 0x7fffffff;                              /* |x| ~< pi/4 */
  if (ix <= 0x3fe921fb) return heff_fdl_kernel_tan(x, z, 1);
  else if (ix >= 0x7ff00000) return x - x;                        /* tan(Inf or NaN) is NaN */
  else {                                                          /* argument reduction needed */
    n = heff_fdl_rem_pio2(x, y);
    return heff_fdl_kernel_tan(y[0], y[1], 1 - ((n & 1) << 1)); /*   1 -- n even, -1 -- n odd */
  }
}

/* __ieee754_exp(x)
 * Returns the exponential of x.
 *
 * Method
 *   1. Argument reduction:
 *      Reduce x to an r so that |r| <= 0.5*ln2 ~ 0.34658.
 *	Given x, find r and integer k such that
 *
 *               x = k*ln2 + r,  |r| <= 0.5*ln2.
 *
 *      Here r will be represented as r = hi-lo for better
 *	accuracy.
 *
 *   2. Approximation of exp(r) by a special rational function on
 *	the interval [0,0.34658]:
 *	Write
 *	    R(r**2) = r*(exp(r)+1)/(exp(r)-1) = 2 + r*r/6 - r**4/360 + ...
 *      We use a special Reme algorithm on [0,0.34658] to generate
 * 	a polynomial of degree 5 to approximate R. The maximum error
 *	of this polynomial approximation is bounded by 2**-59. In
 *	other words,
 *	    R(z) ~ 2.0 + P1*z + P2*z**2 + P3*z**3 + P4*z**4 + P5*z**5
 *  	(where z=r*r, and the values of P1 to P5 are listed below)
 *	and
 *	    |                  5          |     -59
 *	    | 2.0+P1*z+...+P5*z   -  R(z) | <= 2
 *	    |                             |
 *	The computation of exp(r) thus becomes
 *                             2*r
 *		exp(r) = 1 + -------
 *		              R - r
 *                                 r*R1(r)
 *		       = 1 + r + ----------- (for better accuracy)
 *		                  2 - R1(r)
 *	where
 *			         2       4             10
 *		R1(r) = r - (P1*r  + P2*r  + ... + P5*r   ).
 *
 *   3. Scale back to obtain exp(x):
 *	From step 1, we have
 *	   exp(x) = 2^k * exp(r)
 *
 * Special cases:
 *	exp(INF) is INF, exp(NaN) is NaN;
 *	exp(-INF) is 0, and
 *	for finite argument, only exp(0)=1 is exact.
 *
 * Accuracy:
 *	according to an error analysis, the error is always less than
 *	1 ulp (unit in the last place).
 *
 * Misc. info.
 *	For IEEE double
 *	    if x >  7.09782712893383973096e+02 then exp(x) overflow
 *	    if x < -7.45133219101941108420e+02 then exp(x) underflow
 *
 * Constants:
 * The hexadecimal values are the intended ones for the following
 * constants. The decimal values may be used, provided that the
 * compiler will convert from decimal to binary accurately enough
 * to produce the hexadecimal values shown.
 */
HEFF_FN double heff_exp(double x) {
#pragma STDC FP_CONTRACT OFF
  static const double
      one = 1.0,
      halF[2] = {0.5, -0.5},
      huge = 1.0e+300,
      twom1000 = 9.33263618503218878990e-302,    /* 2**-1000=0x01700000,0*/
      o_threshold = 7.09782712893383973096e+02,  /* 0x40862E42, 0xFEFA39EF */
      u_threshold = -7.45133219101941108420e+02, /* 0xc0874910, 0xD52D3051 */
      ln2HI[2] = {6.93147180369123816490e-01,    /* 0x3fe62e42, 0xfee00000 */
                  -6.93147180369123816490e-01},  /* 0xbfe62e42, 0xfee00000 */
      ln2LO[2] = {1.90821492927058770002e-10,    /* 0x3dea39ef, 0x35793c76 */
                  -1.90821492927058770002e-10},  /* 0xbdea39ef, 0x35793c76 */
      invln2 = 1.44269504088896338700e+00,       /* 0x3ff71547, 0x652b82fe */
      P1 = 1.66666666666666019037e-01,           /* 0x3FC55555, 0x5555553E */
      P2 = -2.77777777770155933842e-03,          /* 0xBF66C16C, 0x16BEBD93 */
      P3 = 6.61375632143793436117e-05,           /* 0x3F11566A, 0xAF25DE2C */
      P4 = -1.65339022054652515390e-06,          /* 0xBEBBBD41, 0xC5D26BF1 */
      P5 = 4.13813679705723846039e-08,           /* 0x3E663769, 0x72BEA4D0 */
      E = 2.718281828459045;                     /* 0x4005bf0a, 0x8b145769 */
  double y, hi = 0.0, lo = 0.0, c, t;
  int32_t k = 0, xsb;
  uint32_t hx;

  hx = (uint32_t)HEFF_FDL_HI(x); /* high word of x */
  xsb = (hx >> 31) & 1;          /* sign bit of x */
  hx &= 0x7fffffff;              /* high word of |x| */

  /* filter out non-finite argument */
  if (hx >= 0x40862E42) { /* if |x|>=709.78... */
    if (hx >= 0x7ff00000) {
      if (((hx & 0xfffff) | HEFF_FDL_LO(x)) != 0) return x + x; /* NaN */
      else return (xsb == 0) ? x : 0.0;                        /* exp(+-inf)={inf,0} */
    }
    if (x > o_threshold) return huge * huge;         /* overflow */
    if (x < u_threshold) return twom1000 * twom1000; /* underflow */
  }

  /* argument reduction */
  if (hx > 0x3fd62e42) {   /* if  |x| > 0.5 ln2 */
    if (hx < 0x3FF0A2B2) { /* and |x| < 1.5 ln2 */
      /* (V8's, not fdlibm's: e, which the computation below would get
       * a unit in the last place off) */
      if (x == 1.0) return E;
      hi = x - ln2HI[xsb];
      lo = ln2LO[xsb];
      k = 1 - xsb - xsb;
    } else {
      k = (int32_t)(invln2 * x + halF[xsb]);
      t = k;
      hi = x - t * ln2HI[0]; /* t*ln2HI is exact here */
      lo = t * ln2LO[0];
    }
    x = hi - lo;
  } else if (hx < 0x3e300000) {         /* when |x|<2**-28 */
    if (huge + x > one) return one + x; /* trigger inexact */
  } else k = 0;

  /* x is now in primary range */
  t = x * x;
  c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  if (k == 0) return one - ((x * c) / (c - 2.0) - x);
  else y = one - ((lo - (x * c) / (2.0 - c)) - hi);
  if (k >= -1021) {
    return HEFF_FDL_WORDS((uint32_t)HEFF_FDL_HI(y) + ((uint32_t)k << 20), HEFF_FDL_LO(y)); /* add k to y's exponent */
  } else {
    y = HEFF_FDL_WORDS((uint32_t)HEFF_FDL_HI(y) + ((uint32_t)(k + 1000) << 20), HEFF_FDL_LO(y)); /* add k to y's exponent */
    return y * twom1000;
  }
}

/* __ieee754_log(x)
 * Return the logrithm of x
 *
 * Method :
 *   1. Argument Reduction: find k and f such that
 *			x = 2^k * (1+f),
 *	   where  sqrt(2)/2 < 1+f < sqrt(2) .
 *
 *   2. Approximation of log(1+f).
 *	Let s = f/(2+f) ; based on log(1+f) = log(1+s) - log(1-s)
 *		 = 2s + 2/3 s**3 + 2/5 s**5 + .....,
 *	     	 = 2s + s*R
 *      We use a special Reme algorithm on [0,0.1716] to generate
 * 	a polynomial of degree 14 to approximate R The maximum error
 *	of this polynomial approximation is bounded by 2**-58.45. In
 *	other words,
 *		        2      4      6      8      10      12      14
 *	    R(z) ~ Lg1*s +Lg2*s +Lg3*s +Lg4*s +Lg5*s  +Lg6*s  +Lg7*s
 *  	(the values of Lg1 to Lg7 are listed in the program)
 *	and
 *	    |      2          14          |     -58.45
 *	    | Lg1*s +...+Lg7*s    -  R(z) | <= 2
 *	    |                             |
 *	Note that 2s = f - s*f = f - hfsq + s*hfsq, where hfsq = f*f/2.
 *	In order to guarantee error in log below 1ulp, we compute log
 *	by
 *		log(1+f) = f - s*(f - R)	(if f is not too large)
 *		log(1+f) = f - (hfsq - s*(hfsq+R)).	(better accuracy)
 *
 *	3. Finally,  log(x) = k*ln2 + log(1+f).
 *			    = k*ln2_hi+(f-(hfsq-(s*(hfsq+R)+k*ln2_lo)))
 *	   Here ln2 is split into two floating point number:
 *			ln2_hi + ln2_lo,
 *	   where n*ln2_hi is always exact for |n| < 2000.
 *
 * Special cases:
 *	log(x) is NaN with signal if x < 0 (including -INF) ;
 *	log(+INF) is +INF; log(0) is -INF with signal;
 *	log(NaN) is that NaN with no signal.
 *
 * Accuracy:
 *	according to an error analysis, the error is always less than
 *	1 ulp (unit in the last place).
 *
 * Constants:
 * The hexadecimal values are the intended ones for the following
 * constants. The decimal values may be used, provided that the
 * compiler will convert from decimal to binary accurately enough
 * to produce the hexadecimal values shown.
 */
HEFF_FN double heff_log(double x) {
#pragma STDC FP_CONTRACT OFF
  static const double
      ln2_hi = 6.93147180369123816490e-01, /* 3fe62e42 fee00000 */
      ln2_lo = 1.90821492927058770002e-10, /* 3dea39ef 35793c76 */
      two54 = 1.80143985094819840000e+16,  /* 43500000 00000000 */
      Lg1 = 6.666666666666735130e-01,      /* 3FE55555 55555593 */
      Lg2 = 3.999999999940941908e-01,      /* 3FD99999 9997FA04 */
      Lg3 = 2.857142874366239149e-01,      /* 3FD24924 94229359 */
      Lg4 = 2.222219843214978396e-01,      /* 3FCC71C5 1D8E78AF */
      Lg5 = 1.818357216161805012e-01,      /* 3FC74664 96CB03DE */
      Lg6 = 1.531383769920937332e-01,      /* 3FC39A09 D078C69F */
      Lg7 = 1.479819860511658591e-01;      /* 3FC2F112 DF3E5244 */
  static const double zero = 0.0;
  double hfsq, f, s, z, R, w, t1, t2, dk;
  int32_t k, hx, i, j;
  uint32_t lx;

  hx = HEFF_FDL_HI(x); /* high word of x */
  lx = HEFF_FDL_LO(x); /* low  word of x */

  k = 0;
  if (hx < 0x00100000) {                                   /* x < 2**-1022  */
    if (((hx & 0x7fffffff) | lx) == 0) return -two54 / zero; /* log(+-0)=-inf */
    if (hx < 0) return (x - x) / zero;                     /* log(-#) = NaN */
    k -= 54;
    x *= two54; /* subnormal number, scale up x */
    hx = HEFF_FDL_HI(x); /* high word of x */
  }
  if (hx >= 0x7ff00000) return x + x;
  k += (hx >> 20) - 1023;
  hx &= 0x000fffff;
  i = (hx + 0x95f64) & 0x100000;
  x = HEFF_FDL_WORDS(hx | (i ^ 0x3ff00000), HEFF_FDL_LO(x)); /* normalize x or x/2 */
  k += (i >> 20);
  f = x - 1.0;
  if ((0x000fffff & (2 + hx)) < 3) { /* |f| < 2**-20 */
    if (f == zero) {
      if (k == 0) return zero;
      else {
        dk = (double)k;
        return dk * ln2_hi + dk * ln2_lo;
      }
    }
    R = f * f * (0.5 - 0.33333333333333333 * f);
    if (k == 0) return f - R;
    else {
      dk = (double)k;
      return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
  }
  s = f / (2.0 + f);
  dk = (double)k;
  z = s * s;
  i = hx - 0x6147a;
  w = z * z;
  j = 0x6b851 - hx;
  t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
  t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
  i |= j;
  R = t2 + t1;
  if (i > 0) {
    hfsq = 0.5 * f * f;
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    else return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
  } else {
    if (k == 0) return f - s * (f - R);
    else return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
  }
}

// (std.math's, of a boxed float)
FLT1(heff_floatSin, heff_sin)
FLT1(heff_floatCos, heff_cos)
FLT1(heff_floatTan, heff_tan)
FLT1(heff_floatExp, heff_exp)
FLT1(heff_floatLog, heff_log)
