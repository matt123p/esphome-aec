/* Copyright (C) 2005-2006 Jean-Marc Valin
   File: fftwrap.c

   Wrapper for various FFTs

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "config.h"

#include "arch.h"
#include "os_support.h"
#include "fftwrap.h"

#include <esp_cpu.h>
static volatile unsigned long long spx_fft_forward_cycles;
static volatile unsigned long long spx_fft_inverse_cycles;
static volatile unsigned int spx_fft_forward_calls;
static volatile unsigned int spx_fft_inverse_calls;

void spx_fft_profile_get(spx_fft_profile_t *profile)
{
   profile->forward_cycles = spx_fft_forward_cycles;
   profile->inverse_cycles = spx_fft_inverse_cycles;
   profile->forward_calls = spx_fft_forward_calls;
   profile->inverse_calls = spx_fft_inverse_calls;
}

#define MAX_FFT_SIZE 2048

#ifdef FIXED_POINT
static int maximize_range(spx_word16_t *in, spx_word16_t *out, spx_word16_t bound, int len)
{
   int i, shift;
   spx_word16_t max_val = 0;
   for (i=0;i<len;i++)
   {
      if (in[i]>max_val)
         max_val = in[i];
      if (-in[i]>max_val)
         max_val = -in[i];
   }
   shift=0;
   while (max_val <= (bound>>1) && max_val != 0)
   {
      max_val <<= 1;
      shift++;
   }
   for (i=0;i<len;i++)
   {
      out[i] = SHL16(in[i], shift);
   }
   return shift;
}

static void renorm_range(spx_word16_t *in, spx_word16_t *out, int shift, int len)
{
   int i;
   for (i=0;i<len;i++)
   {
      out[i] = PSHR16(in[i], shift);
   }
}
#endif

#ifdef USE_SMALLFT

#include "smallft.h"
#include <math.h>

void *spx_fft_init(int size)
{
   struct drft_lookup *table;
   table = speex_alloc(sizeof(struct drft_lookup));
   spx_drft_init((struct drft_lookup *)table, size);
   return (void*)table;
}

void spx_fft_destroy(void *table)
{
   spx_drft_clear(table);
   speex_free(table);
}

void spx_fft(void *table, float *in, float *out)
{
   if (in==out)
   {
      int i;
      float scale = 1./((struct drft_lookup *)table)->n;
      speex_warning("FFT should not be done in-place");
      for (i=0;i<((struct drft_lookup *)table)->n;i++)
         out[i] = scale*in[i];
   } else {
      int i;
      float scale = 1./((struct drft_lookup *)table)->n;
      for (i=0;i<((struct drft_lookup *)table)->n;i++)
         out[i] = scale*in[i];
   }
   spx_drft_forward((struct drft_lookup *)table, out);
}

void spx_ifft(void *table, float *in, float *out)
{
   if (in==out)
   {
      speex_warning("FFT should not be done in-place");
   } else {
      int i;
      for (i=0;i<((struct drft_lookup *)table)->n;i++)
         out[i] = in[i];
   }
   spx_drft_backward((struct drft_lookup *)table, out);
}

#elif defined(USE_INTEL_MKL)
#include <mkl.h>

struct mkl_config {
  DFTI_DESCRIPTOR_HANDLE desc;
  int N;
};

void *spx_fft_init(int size)
{
  struct mkl_config *table = (struct mkl_config *) speex_alloc(sizeof(struct mkl_config));
  table->N = size;
  DftiCreateDescriptor(&table->desc, DFTI_SINGLE, DFTI_REAL, 1, size);
  DftiSetValue(table->desc, DFTI_PACKED_FORMAT, DFTI_PACK_FORMAT);
  DftiSetValue(table->desc, DFTI_PLACEMENT, DFTI_NOT_INPLACE);
  DftiSetValue(table->desc, DFTI_FORWARD_SCALE, 1.0f / size);
  DftiCommitDescriptor(table->desc);
  return table;
}

void spx_fft_destroy(void *table)
{
  struct mkl_config *t = (struct mkl_config *) table;
  DftiFreeDescriptor(t->desc);
  speex_free(table);
}

void spx_fft(void *table, spx_word16_t *in, spx_word16_t *out)
{
  struct mkl_config *t = (struct mkl_config *) table;
  DftiComputeForward(t->desc, in, out);
}

void spx_ifft(void *table, spx_word16_t *in, spx_word16_t *out)
{
  struct mkl_config *t = (struct mkl_config *) table;
  DftiComputeBackward(t->desc, in, out);
}

#elif defined(USE_INTEL_IPP)

#include <ipps.h>

struct ipp_fft_config
{
  IppsDFTSpec_R_32f *dftSpec;
  Ipp8u *buffer;
};

void *spx_fft_init(int size)
{
  int bufferSize = 0;
  int hint;
  struct ipp_fft_config *table;

  table = (struct ipp_fft_config *)speex_alloc(sizeof(struct ipp_fft_config));

  /* there appears to be no performance difference between ippAlgHintFast and
     ippAlgHintAccurate when using the with the floating point version
     of the fft. */
  hint = ippAlgHintAccurate;

  ippsDFTInitAlloc_R_32f(&table->dftSpec, size, IPP_FFT_DIV_FWD_BY_N, hint);

  ippsDFTGetBufSize_R_32f(table->dftSpec, &bufferSize);
  table->buffer = ippsMalloc_8u(bufferSize);

  return table;
}

void spx_fft_destroy(void *table)
{
  struct ipp_fft_config *t = (struct ipp_fft_config *)table;
  ippsFree(t->buffer);
  ippsDFTFree_R_32f(t->dftSpec);
  speex_free(t);
}

void spx_fft(void *table, spx_word16_t *in, spx_word16_t *out)
{
  struct ipp_fft_config *t = (struct ipp_fft_config *)table;
  ippsDFTFwd_RToPack_32f(in, out, t->dftSpec, t->buffer);
}

void spx_ifft(void *table, spx_word16_t *in, spx_word16_t *out)
{
  struct ipp_fft_config *t = (struct ipp_fft_config *)table;
  ippsDFTInv_PackToR_32f(in, out, t->dftSpec, t->buffer);
}

#elif defined(USE_GPL_FFTW3)

#include <fftw3.h>

struct fftw_config {
  float *in;
  float *out;
  fftwf_plan fft;
  fftwf_plan ifft;
  int N;
};

void *spx_fft_init(int size)
{
  struct fftw_config *table = (struct fftw_config *) speex_alloc(sizeof(struct fftw_config));
  table->in = fftwf_malloc(sizeof(float) * (size+2));
  table->out = fftwf_malloc(sizeof(float) * (size+2));

  table->fft = fftwf_plan_dft_r2c_1d(size, table->in, (fftwf_complex *) table->out, FFTW_PATIENT);
  table->ifft = fftwf_plan_dft_c2r_1d(size, (fftwf_complex *) table->in, table->out, FFTW_PATIENT);

  table->N = size;
  return table;
}

void spx_fft_destroy(void *table)
{
  struct fftw_config *t = (struct fftw_config *) table;
  fftwf_destroy_plan(t->fft);
  fftwf_destroy_plan(t->ifft);
  fftwf_free(t->in);
  fftwf_free(t->out);
  speex_free(table);
}


void spx_fft(void *table, spx_word16_t *in, spx_word16_t *out)
{
  int i;
  struct fftw_config *t = (struct fftw_config *) table;
  const int N = t->N;
  float *iptr = t->in;
  float *optr = t->out;
  const float m = 1.0 / N;
  for(i=0;i<N;++i)
    iptr[i]=in[i] * m;

  fftwf_execute(t->fft);

  out[0] = optr[0];
  for(i=1;i<N;++i)
    out[i] = optr[i+1];
}

void spx_ifft(void *table, spx_word16_t *in, spx_word16_t *out)
{
  int i;
  struct fftw_config *t = (struct fftw_config *) table;
  const int N = t->N;
  float *iptr = t->in;
  float *optr = t->out;

  iptr[0] = in[0];
  iptr[1] = 0.0f;
  for(i=1;i<N;++i)
    iptr[i+1] = in[i];
  iptr[N+1] = 0.0f;

  fftwf_execute(t->ifft);

   for(i=0;i<N;++i)
     out[i] = optr[i];
}

#elif defined(USE_ESP_DSP_FFT) && !defined(FIXED_POINT)

/* Espressif ESP-DSP (espressif/esp-dsp) FFT backend. On ESP32-S3 and
 * ESP32-P4 the complex FFT uses target-optimized aes3/arp4 kernels.
 * In particular, P4 float arithmetic is scalar FPU plus hardware loops,
 * not packed floating-point SIMD. Other targets use ANSI C.
 *
 * An N-point real transform is performed as an N/2-point complex transform.
 * This is important: zero-extending the input to an N-point complex transform
 * roughly doubles the FFT work and loses to kiss_fftr despite the optimized kernel.
 * The real-spectrum post-processing is fused with Speex packing/scaling.  The
 * inverse post-processing below mirrors kiss_fftri2, followed by a complex
 * inverse implemented with ESP-DSP's forward FFT and conjugation.  This
 * backend deliberately has no software fallback: invalid sizes are rejected
 * by the component schema and initialization failures are fatal. */

#include "esp_dsp.h"
#include <math.h>

struct esp_dsp_fft_config {
   int N;
   int radix4;
   float *mem;              /* N floats: N/2 interleaved complex values */
   float *twiddles;         /* N floats: N/2 inverse real-FFT twiddles */
};

static int esp_dsp_fft_is_pow2(int n)
{
   return n > 0 && (n & (n - 1)) == 0;
}

void *spx_fft_init(int size)
{
   struct esp_dsp_fft_config *table;
   table = (struct esp_dsp_fft_config *)speex_alloc(sizeof(struct esp_dsp_fft_config));
   if (table == NULL)
      return NULL;
   table->N = size;
   table->mem = NULL;
   table->twiddles = NULL;
   if (size < 4 || size > MAX_FFT_SIZE || !esp_dsp_fft_is_pow2(size)) {
      speex_free(table);
      speex_fatal("ESP-DSP FFT size must be a power of two");
      return NULL;
   }
   /* ncfft must be a power of FOUR for radix-4. Other supported sizes use
      ESP-DSP radix-2, not a software/backend fallback. */
   const int ncfft = size / 2;
   table->radix4 = (ncfft & 0x55555555) != 0;
   static int esp_dsp_fft_ready[2] = {0, 0};
   if (!esp_dsp_fft_ready[table->radix4]) {
      /* The wrapper supports up to 2048 real points (1024 complex). Avoid
         allocating the library's default 4096-point table for each radix. */
      const esp_err_t err = table->radix4
          ? dsps_fft4r_init_fc32(NULL, MAX_FFT_SIZE / 2)
          : dsps_fft2r_init_fc32(NULL, MAX_FFT_SIZE / 2);
      if (err == ESP_OK || err == ESP_ERR_DSP_REINITIALIZED)
         esp_dsp_fft_ready[table->radix4] = 1;
   }
   if (!esp_dsp_fft_ready[table->radix4]) {
      speex_free(table);
      speex_fatal("ESP-DSP FFT initialization failed");
      return NULL;
   }
   /* One allocation keeps both hot arrays in internal RAM when possible. */
   table->mem = (float *)speex_alloc_scratch(2 * size * sizeof(float));
   if (table->mem == NULL) {
      speex_free(table);
      speex_fatal("ESP-DSP FFT workspace allocation failed");
      return NULL;
   }
   const float pi = 3.14159265358979323846f;
   table->twiddles = table->mem + size;
   for (int k = 0; k < ncfft; k++) {
      const float phase = pi * ((float)k / ncfft + 0.5f);
      table->twiddles[2 * k] = cosf(phase);
      table->twiddles[2 * k + 1] = sinf(phase);
   }
#ifdef ESP_PLATFORM
   ESP_LOGI(SPEEXDSP_LOG_TAG, "FFT complex kernel: radix-%d", table->radix4 ? 4 : 2);
#if CONFIG_IDF_TARGET_ESP32P4
   ESP_LOGI(SPEEXDSP_LOG_TAG,
            "FFT backend: ESP-DSP ARP4 real FFT, N=%d (complex N=%d)",
            size, size / 2);
#elif CONFIG_IDF_TARGET_ESP32S3
   ESP_LOGI(SPEEXDSP_LOG_TAG,
            "FFT backend: ESP-DSP AES3 real FFT, N=%d (complex N=%d)",
            size, size / 2);
#else
   ESP_LOGI(SPEEXDSP_LOG_TAG,
            "FFT backend: ESP-DSP real FFT, N=%d (complex N=%d)",
            size, size / 2);
#endif
#endif
   return table;
}

void spx_fft_destroy(void *table)
{
   struct esp_dsp_fft_config *t = (struct esp_dsp_fft_config *)table;
   if (t->mem != NULL)
      speex_free_scratch(t->mem);
   speex_free(table);
}

void spx_fft(void *table, spx_word16_t *in, spx_word16_t *out)
{
   const unsigned int profile_start = esp_cpu_get_cycle_count();
   struct esp_dsp_fft_config *t = (struct esp_dsp_fft_config *)table;
   const int N = t->N;
   const int ncfft = N / 2;
   float *mem = t->mem;
   const float scale = 1.f / N;
   for (int i = 0; i < N; i++)
      mem[i] = in[i];

   if (t->radix4) {
      dsps_fft4r_fc32(mem, ncfft);
      dsps_bit_rev4r_fc32(mem, ncfft);
   } else {
      dsps_fft2r_fc32(mem, ncfft);
      dsps_bit_rev2r_fc32(mem, ncfft);
   }

   /* Convert the N/2 complex FFT into an N-point real spectrum while writing
    * Speex's packed layout directly.  This replaces ESP-DSP's scalar
    * dsps_cplx2real_fc32 pass on P4 and folds in the required 1/N scaling. */
   out[0] = (mem[0] + mem[1]) * scale;
   out[N - 1] = (mem[0] - mem[1]) * scale;
   for (int k = 1; k <= ncfft / 2; k++) {
      const int nk = ncfft - k;
      const float f1_re = mem[2 * k] + mem[2 * nk];
      const float f1_im = mem[2 * k + 1] - mem[2 * nk + 1];
      const float f2_re = mem[2 * k] - mem[2 * nk];
      const float f2_im = mem[2 * k + 1] + mem[2 * nk + 1];
      const float c = t->twiddles[2 * k];
      const float s = t->twiddles[2 * k + 1];
      /* Forward super-twiddle is conjugate(twiddles[k]). */
      const float tw_re = f2_re * c + f2_im * s;
      const float tw_im = f2_im * c - f2_re * s;

      out[2 * k - 1] = 0.5f * (f1_re + tw_re) * scale;
      out[2 * k] = 0.5f * (f1_im + tw_im) * scale;
      out[2 * nk - 1] = 0.5f * (f1_re - tw_re) * scale;
      out[2 * nk] = 0.5f * (tw_im - f1_im) * scale;
   }
   spx_fft_forward_cycles += (unsigned int)(esp_cpu_get_cycle_count() - profile_start);
   spx_fft_forward_calls++;
}

void spx_ifft(void *table, spx_word16_t *in, spx_word16_t *out)
{
   const unsigned int profile_start = esp_cpu_get_cycle_count();
   struct esp_dsp_fft_config *t = (struct esp_dsp_fft_config *)table;
   const int N = t->N;
   const int ncfft = N / 2;
   float *mem = t->mem;
   const float *tw = t->twiddles;

   /* Undo the real-FFT post-processing.  These factors intentionally match
    * kiss_fftri2: its unscaled inverse is the contract expected by Speex. */
   mem[0] = in[0] + in[N - 1];
   /* Conjugate bin zero too: its imaginary part encodes DC minus Nyquist.
      Missing this sign corrupts every odd time sample by a constant. */
   mem[1] = in[N - 1] - in[0];
   for (int k = 1; k <= ncfft / 2; k++) {
      const int nk = ncfft - k;
      const float fk_re = in[2 * k - 1];
      const float fk_im = in[2 * k];
      const float fnkc_re = in[2 * nk - 1];
      const float fnkc_im = -in[2 * nk];
      const float fek_re = fk_re + fnkc_re;
      const float fek_im = fk_im + fnkc_im;
      const float tmp_re = fk_re - fnkc_re;
      const float tmp_im = fk_im - fnkc_im;
      const float fok_re = tmp_re * tw[2 * k] - tmp_im * tw[2 * k + 1];
      const float fok_im = tmp_im * tw[2 * k] + tmp_re * tw[2 * k + 1];

      mem[2 * k] = fek_re + fok_re;
      mem[2 * k + 1] = -(fek_im + fok_im); /* conjugate for inverse FFT */
      mem[2 * nk] = fek_re - fok_re;
      mem[2 * nk + 1] = fek_im - fok_im;   /* conjugate of conjugated result */
   }

   /* Unscaled inverse complex FFT: conj(FFT(conj(x))).  There must be no
    * additional 1/N here because spx_fft already scaled its output by 1/N. */
   if (t->radix4) {
      dsps_fft4r_fc32(mem, ncfft);
      dsps_bit_rev4r_fc32(mem, ncfft);
   } else {
      dsps_fft2r_fc32(mem, ncfft);
      dsps_bit_rev2r_fc32(mem, ncfft);
   }
   for (int i = 0; i < ncfft; i++) {
      out[2 * i] = mem[2 * i];
      out[2 * i + 1] = -mem[2 * i + 1];
   }
   spx_fft_inverse_cycles += (unsigned int)(esp_cpu_get_cycle_count() - profile_start);
   spx_fft_inverse_calls++;
}

#elif defined(USE_KISS_FFT)

#include "kiss_fftr.h"
#include "kiss_fft.h"

struct kiss_config {
   kiss_fftr_cfg forward;
   kiss_fftr_cfg backward;
   int N;
};

void *spx_fft_init(int size)
{
   struct kiss_config *table;
   table = (struct kiss_config*)speex_alloc(sizeof(struct kiss_config));
   table->forward = kiss_fftr_alloc(size,0,NULL,NULL);
   table->backward = kiss_fftr_alloc(size,1,NULL,NULL);
   table->N = size;
   return table;
}

void spx_fft_destroy(void *table)
{
   struct kiss_config *t = (struct kiss_config *)table;
   kiss_fftr_free(t->forward);
   kiss_fftr_free(t->backward);
   speex_free(table);
}

#ifdef FIXED_POINT

void spx_fft(void *table, spx_word16_t *in, spx_word16_t *out)
{
   int shift;
   struct kiss_config *t = (struct kiss_config *)table;
   shift = maximize_range(in, in, 32000, t->N);
   kiss_fftr2(t->forward, in, out);
   renorm_range(in, in, shift, t->N);
   renorm_range(out, out, shift, t->N);
}

#else

void spx_fft(void *table, spx_word16_t *in, spx_word16_t *out)
{
   const unsigned int profile_start = esp_cpu_get_cycle_count();
   int i;
   float scale;
   struct kiss_config *t = (struct kiss_config *)table;
   scale = 1./t->N;
   kiss_fftr2(t->forward, in, out);
   for (i=0;i<t->N;i++)
      out[i] *= scale;
   spx_fft_forward_cycles += (unsigned int)(esp_cpu_get_cycle_count() - profile_start);
   spx_fft_forward_calls++;
}
#endif

void spx_ifft(void *table, spx_word16_t *in, spx_word16_t *out)
{
   const unsigned int profile_start = esp_cpu_get_cycle_count();
   struct kiss_config *t = (struct kiss_config *)table;
   kiss_fftri2(t->backward, in, out);
   spx_fft_inverse_cycles += (unsigned int)(esp_cpu_get_cycle_count() - profile_start);
   spx_fft_inverse_calls++;
}


#else

#error No other FFT implemented

#endif


#ifdef FIXED_POINT
/*#include "smallft.h"*/


void spx_fft_float(void *table, float *in, float *out)
{
   int i;
#ifdef USE_SMALLFT
   int N = ((struct drft_lookup *)table)->n;
#elif defined(USE_KISS_FFT)
   int N = ((struct kiss_config *)table)->N;
#else
#endif
#ifdef VAR_ARRAYS
   spx_word16_t _in[N];
   spx_word16_t _out[N];
#else
   spx_word16_t _in[MAX_FFT_SIZE];
   spx_word16_t _out[MAX_FFT_SIZE];
#endif
   for (i=0;i<N;i++)
      _in[i] = (int)floor(.5+in[i]);
   spx_fft(table, _in, _out);
   for (i=0;i<N;i++)
      out[i] = _out[i];
#if 0
   if (!fixed_point)
   {
      float scale;
      struct drft_lookup t;
      spx_drft_init(&t, ((struct kiss_config *)table)->N);
      scale = 1./((struct kiss_config *)table)->N;
      for (i=0;i<((struct kiss_config *)table)->N;i++)
         out[i] = scale*in[i];
      spx_drft_forward(&t, out);
      spx_drft_clear(&t);
   }
#endif
}

void spx_ifft_float(void *table, float *in, float *out)
{
   int i;
#ifdef USE_SMALLFT
   int N = ((struct drft_lookup *)table)->n;
#elif defined(USE_KISS_FFT)
   int N = ((struct kiss_config *)table)->N;
#else
#endif
#ifdef VAR_ARRAYS
   spx_word16_t _in[N];
   spx_word16_t _out[N];
#else
   spx_word16_t _in[MAX_FFT_SIZE];
   spx_word16_t _out[MAX_FFT_SIZE];
#endif
   for (i=0;i<N;i++)
      _in[i] = (int)floor(.5+in[i]);
   spx_ifft(table, _in, _out);
   for (i=0;i<N;i++)
      out[i] = _out[i];
#if 0
   if (!fixed_point)
   {
      int i;
      struct drft_lookup t;
      spx_drft_init(&t, ((struct kiss_config *)table)->N);
      for (i=0;i<((struct kiss_config *)table)->N;i++)
         out[i] = in[i];
      spx_drft_backward(&t, out);
      spx_drft_clear(&t);
   }
#endif
}

#else

void spx_fft_float(void *table, float *in, float *out)
{
   spx_fft(table, in, out);
}
void spx_ifft_float(void *table, float *in, float *out)
{
   spx_ifft(table, in, out);
}

#endif
