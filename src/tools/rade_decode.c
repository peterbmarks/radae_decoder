/*---------------------------------------------------------------------------*\

  rade_decode.c

  This is for OpenWebRX and similar.
  
  RADAE streaming decoder.  Reads 16-bit signed mono audio at 8 kHz from
  stdin, decodes RADAE, and writes 16-bit signed mono audio at 8 kHz to
  stdout.

  Combines a streaming Hilbert transform, RADAE RX (OFDM demod + neural
  decoder), and the FARGAN vocoder into a single command-line tool.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <getopt.h>

#include "rade_api.h"
#include "rade_dsp.h"
#include "fargan.h"
#include "lpcnet.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Hilbert transform (coefficients match real2iq.c exactly) ---- */

#define HILBERT_NTAPS   127
#define HILBERT_DELAY   ((HILBERT_NTAPS - 1) / 2)   /* 63 */

static float hilbert_coeffs[HILBERT_NTAPS];
static float hilbert_history[HILBERT_NTAPS];

static void init_hilbert(void) {
    int center = HILBERT_DELAY;
    for (int i = 0; i < HILBERT_NTAPS; i++) {
        int n = i - center;
        if (n == 0 || (n & 1) == 0) {
            hilbert_coeffs[i] = 0.0f;
        } else {
            float h = 2.0f / (M_PI * n);
            float w = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (HILBERT_NTAPS - 1));
            hilbert_coeffs[i] = h * w;
        }
    }
    memset(hilbert_history, 0, sizeof(hilbert_history));
}

/* Process one sample through the streaming Hilbert transform.
   history[0] = most recent sample, history[NTAPS-1] = oldest. */
static RADE_COMP hilbert_one(float sample) {
    memmove(&hilbert_history[1], &hilbert_history[0],
            (HILBERT_NTAPS - 1) * sizeof(float));
    hilbert_history[0] = sample;

    RADE_COMP out;
    out.real = hilbert_history[HILBERT_DELAY];

    float imag = 0.0f;
    for (int k = 0; k < HILBERT_NTAPS; k++)
        imag += hilbert_coeffs[k] * hilbert_history[k];
    out.imag = imag;

    return out;
}

/* ---- Usage ---- */

static void usage(void) {
    fprintf(stderr,
            "usage: rade_decode [options]\n\n"
            "  Reads 16-bit signed mono audio at %d Hz from stdin,\n"
            "  decodes RADAE, and writes 16-bit signed mono audio\n"
            "  at %d Hz to stdout.\n\n"
            "options:\n"
            "  -h, --help     Show this help\n"
            "  -v LEVEL       Verbosity: 0=quiet  1=normal (default)  2=verbose\n",
            RADE_FS, RADE_FS);
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    int verbose = 1;
    int opt;
    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {NULL,   0,           NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "hv:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': usage(); return 0;
            case 'v': verbose = atoi(optarg); break;
            default:  usage(); return 1;
        }
    }

    /* ---- init Hilbert transform ---- */
    init_hilbert();

    /* ---- init RADE receiver ---- */
    rade_initialize();

    int flags = (verbose < 2) ? RADE_VERBOSE_0 : 0;
    char *model_name = "model19_check3/checkpoints/checkpoint_epoch_100.pth";
    struct rade *r = rade_open(model_name, flags);
    if (!r) {
        fprintf(stderr, "rade_decode: rade_open failed\n");
        rade_finalize();
        return 1;
    }

    int nin_max        = rade_nin_max(r);
    int n_features_out = rade_n_features_in_out(r);
    int n_eoo_bits     = rade_n_eoo_bits(r);

    if (verbose >= 1)
        fprintf(stderr, "nin_max: %d  n_features_out: %d  n_eoo_bits: %d\n",
                nin_max, n_features_out, n_eoo_bits);

    int16_t   *pcm_in   = malloc((size_t)nin_max        * sizeof(int16_t));
    RADE_COMP *iq_buf    = malloc((size_t)nin_max        * sizeof(RADE_COMP));
    float     *feat_buf  = malloc((size_t)n_features_out * sizeof(float));
    float     *eoo_buf   = malloc((size_t)n_eoo_bits     * sizeof(float));
    if (!pcm_in || !iq_buf || !feat_buf || !eoo_buf) {
        fprintf(stderr, "rade_decode: malloc failed\n");
        free(pcm_in); free(iq_buf); free(feat_buf); free(eoo_buf);
        rade_close(r); rade_finalize();
        return 1;
    }

    /* ---- init FARGAN vocoder ---- */
    FARGANState fargan;
    fargan_init(&fargan);

    int   fargan_ready = 0;
    float cont_buf[5 * NB_TOTAL_FEATURES];
    int   cont_frames  = 0;
    int   was_synced   = 0;

    /* ---- main processing loop ---- */
    int mf_count  = 0;
    int vld_count = 0;

    while (1) {
        int nin = rade_nin(r);

        /* Read nin S16 samples from stdin */
        size_t n_read = fread(pcm_in, sizeof(int16_t), (size_t)nin, stdin);
        if (n_read != (size_t)nin)
            break;

        /* S16 → float → streaming Hilbert → IQ */
        for (int i = 0; i < nin; i++) {
            float sample = pcm_in[i] / 32768.0f;
            iq_buf[i] = hilbert_one(sample);
        }

        /* RADE RX */
        int has_eoo = 0;
        int n_out   = rade_rx(r, feat_buf, &has_eoo, eoo_buf, iq_buf);

        if (has_eoo && verbose >= 1)
            fprintf(stderr, "End-of-over at modem frame %d\n", mf_count);

        /* Re-init FARGAN when sync is newly acquired so we get a clean
           warm-up for each transmission. */
        int synced = rade_sync(r);
        if (synced && !was_synced) {
            fargan_init(&fargan);
            fargan_ready = 0;
            cont_frames  = 0;
        }
        was_synced = synced;

        if (n_out > 0) {
            vld_count++;
            int n_frames = n_out / RADE_NB_TOTAL_FEATURES;

            for (int fi = 0; fi < n_frames; fi++) {
                float *feat = &feat_buf[fi * RADE_NB_TOTAL_FEATURES];

                /* ---- FARGAN warm-up: buffer the first 5 frames ---- */
                if (!fargan_ready) {
                    memcpy(&cont_buf[cont_frames * NB_TOTAL_FEATURES],
                           feat, (size_t)NB_TOTAL_FEATURES * sizeof(float));
                    if (++cont_frames >= 5) {
                        /* fargan_cont expects features packed at stride
                           NB_FEATURES – copy only the first NB_FEATURES of
                           each buffered frame, matching lpcnet_demo. */
                        float packed[5 * NB_FEATURES];
                        for (int i = 0; i < 5; i++)
                            memcpy(&packed[i * NB_FEATURES],
                                   &cont_buf[i * NB_TOTAL_FEATURES],
                                   (size_t)NB_FEATURES * sizeof(float));

                        float zeros[FARGAN_CONT_SAMPLES];
                        memset(zeros, 0, sizeof(zeros));
                        fargan_cont(&fargan, zeros, packed);
                        fargan_ready = 1;
                    }
                    continue;   /* warm-up frames are not synthesised */
                }

                /* ---- synthesise one 10 ms speech frame (160 @ 16 kHz) ---- */
                float fpcm[LPCNET_FRAME_SIZE];
                fargan_synthesize(&fargan, fpcm, feat);

                /* ---- downsample 16 kHz → 8 kHz (2:1) and write S16 ---- */
                int n_8k = LPCNET_FRAME_SIZE / 2;   /* 80 */
                int16_t pcm_out[LPCNET_FRAME_SIZE / 2];
                for (int s = 0; s < n_8k; s++) {
                    float v = (fpcm[2 * s] + fpcm[2 * s + 1]) * 0.5f * 32768.0f;
                    if (v >  32767.0f) v =  32767.0f;
                    if (v < -32767.0f) v = -32767.0f;
                    pcm_out[s] = (int16_t)floor(0.5 + (double)v);
                }

                fwrite(pcm_out, sizeof(int16_t), (size_t)n_8k, stdout);
            }
        }
        mf_count++;
    }

    if (verbose >= 1)
        fprintf(stderr, "Modem frames: %d   valid: %d\n", mf_count, vld_count);

    /* ---- cleanup ---- */
    free(pcm_in);
    free(iq_buf);
    free(feat_buf);
    free(eoo_buf);
    rade_close(r);
    rade_finalize();
    return 0;
}
