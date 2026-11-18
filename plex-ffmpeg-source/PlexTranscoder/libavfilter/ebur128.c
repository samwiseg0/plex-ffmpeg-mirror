/*
 * Copyright (c) 2011 Jan Kokemüller
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * This file is based on libebur128 which is available at
 * https://github.com/jiixyj/libebur128/
 *
 * Libebur128 has the following copyright:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
*/

#include "ebur128.h"

#include <float.h>
#include <limits.h>
#include <math.h>               /* You may have to define _USE_MATH_DEFINES if you use MSVC */

#include "libavutil/error.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"
#include "libavutil/thread.h"

#define CHECK_ERROR(condition, errorcode, goto_point)                          \
    if ((condition)) {                                                         \
        errcode = (errorcode);                                                 \
        goto goto_point;                                                       \
    }

#define ALMOST_ZERO 0.000001

#define RELATIVE_GATE         (-10.0)
#define RELATIVE_GATE_FACTOR  pow(10.0, RELATIVE_GATE / 10.0)
#define MINUS_20DB            pow(10.0, -20.0 / 10.0)

struct FFEBUR128StateInternal {
    /** Filtered audio data (used as ring buffer). */
    double *audio_data;
    /** Size of audio_data array. */
    size_t audio_data_frames;
    /** Current index for audio_data. */
    size_t audio_data_index;
    /** How many frames are needed for a gating block. Will correspond to 400ms
     *  of audio at initialization, and 100ms after the first block (75% overlap
     *  as specified in the 2011 revision of BS1770). */
    unsigned long needed_frames;
    /** The channel map. Has as many elements as there are channels. */
    int *channel_map;
    /** How many samples fit in 100ms (rounded). */
    unsigned long samples_in_100ms;
    /** BS.1770 filter coefficients (nominator). */
    double b[5];
    /** BS.1770 filter coefficients (denominator). */
    double a[5];
    /** BS.1770 filter state. */
    double v[5][5];
    /** Histograms, used to calculate LRA. */
    unsigned long *block_energy_histogram;
    unsigned long *short_term_block_energy_histogram;
    /** Keeps track of when a new short term block is needed. */
    size_t short_term_frame_counter;
    /** Maximum sample peak, one per channel */
    double *sample_peak;
    /** The maximum window duration in ms. */
    unsigned long window;
    /** Data pointer array for interleaved data */
    void **data_ptrs;
    /** Channel-weighted energy of each 100ms slice of audio_data, so that
     *  gating blocks and short-term windows ending on a slice boundary can be
     *  summed from slices instead of from every sample. */
    double *slice_energy;
    /** Number of 100ms slices in audio_data. */
    size_t nb_slices;
};

static AVOnce histogram_init = AV_ONCE_INIT;
static DECLARE_ALIGNED(32, double, histogram_energies)[1000];
static DECLARE_ALIGNED(32, double, histogram_energy_boundaries)[1001];

static void ebur128_init_filter(FFEBUR128State * st)
{
    int i, j;

    double f0 = 1681.974450955533;
    double G = 3.999843853973347;
    double Q = 0.7071752369554196;

    double K = tan(M_PI * f0 / (double) st->samplerate);
    double Vh = pow(10.0, G / 20.0);
    double Vb = pow(Vh, 0.4996667741545416);

    double pb[3] = { 0.0, 0.0, 0.0 };
    double pa[3] = { 1.0, 0.0, 0.0 };
    double rb[3] = { 1.0, -2.0, 1.0 };
    double ra[3] = { 1.0, 0.0, 0.0 };

    double a0 = 1.0 + K / Q + K * K;
    pb[0] = (Vh + Vb * K / Q + K * K) / a0;
    pb[1] = 2.0 * (K * K - Vh) / a0;
    pb[2] = (Vh - Vb * K / Q + K * K) / a0;
    pa[1] = 2.0 * (K * K - 1.0) / a0;
    pa[2] = (1.0 - K / Q + K * K) / a0;

    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = tan(M_PI * f0 / (double) st->samplerate);

    ra[1] = 2.0 * (K * K - 1.0) / (1.0 + K / Q + K * K);
    ra[2] = (1.0 - K / Q + K * K) / (1.0 + K / Q + K * K);

    st->d->b[0] = pb[0] * rb[0];
    st->d->b[1] = pb[0] * rb[1] + pb[1] * rb[0];
    st->d->b[2] = pb[0] * rb[2] + pb[1] * rb[1] + pb[2] * rb[0];
    st->d->b[3] = pb[1] * rb[2] + pb[2] * rb[1];
    st->d->b[4] = pb[2] * rb[2];

    st->d->a[0] = pa[0] * ra[0];
    st->d->a[1] = pa[0] * ra[1] + pa[1] * ra[0];
    st->d->a[2] = pa[0] * ra[2] + pa[1] * ra[1] + pa[2] * ra[0];
    st->d->a[3] = pa[1] * ra[2] + pa[2] * ra[1];
    st->d->a[4] = pa[2] * ra[2];

    for (i = 0; i < 5; ++i) {
        for (j = 0; j < 5; ++j) {
            st->d->v[i][j] = 0.0;
        }
    }
}

static int ebur128_init_channel_map(FFEBUR128State * st)
{
    size_t i;
    st->d->channel_map =
        (int *) av_malloc_array(st->channels, sizeof(*st->d->channel_map));
    if (!st->d->channel_map)
        return AVERROR(ENOMEM);
    if (st->channels == 4) {
        st->d->channel_map[0] = FF_EBUR128_LEFT;
        st->d->channel_map[1] = FF_EBUR128_RIGHT;
        st->d->channel_map[2] = FF_EBUR128_LEFT_SURROUND;
        st->d->channel_map[3] = FF_EBUR128_RIGHT_SURROUND;
    } else if (st->channels == 5) {
        st->d->channel_map[0] = FF_EBUR128_LEFT;
        st->d->channel_map[1] = FF_EBUR128_RIGHT;
        st->d->channel_map[2] = FF_EBUR128_CENTER;
        st->d->channel_map[3] = FF_EBUR128_LEFT_SURROUND;
        st->d->channel_map[4] = FF_EBUR128_RIGHT_SURROUND;
    } else {
        for (i = 0; i < st->channels; ++i) {
            switch (i) {
            case 0:
                st->d->channel_map[i] = FF_EBUR128_LEFT;
                break;
            case 1:
                st->d->channel_map[i] = FF_EBUR128_RIGHT;
                break;
            case 2:
                st->d->channel_map[i] = FF_EBUR128_CENTER;
                break;
            case 3:
                st->d->channel_map[i] = FF_EBUR128_UNUSED;
                break;
            case 4:
                st->d->channel_map[i] = FF_EBUR128_LEFT_SURROUND;
                break;
            case 5:
                st->d->channel_map[i] = FF_EBUR128_RIGHT_SURROUND;
                break;
            default:
                st->d->channel_map[i] = FF_EBUR128_UNUSED;
                break;
            }
        }
    }
    return 0;
}

static inline void init_histogram(void)
{
    int i;
    /* initialize static constants */
    histogram_energy_boundaries[0] = pow(10.0, (-70.0 + 0.691) / 10.0);
    for (i = 0; i < 1000; ++i) {
        histogram_energies[i] =
            pow(10.0, ((double) i / 10.0 - 69.95 + 0.691) / 10.0);
    }
    for (i = 1; i < 1001; ++i) {
        histogram_energy_boundaries[i] =
            pow(10.0, ((double) i / 10.0 - 70.0 + 0.691) / 10.0);
    }
}

FFEBUR128State *ff_ebur128_init(unsigned int channels,
                                unsigned long samplerate,
                                unsigned long window, int mode)
{
    int errcode;
    FFEBUR128State *st;

    st = (FFEBUR128State *) av_malloc(sizeof(*st));
    CHECK_ERROR(!st, 0, exit)
    st->d = (struct FFEBUR128StateInternal *)
        av_malloc(sizeof(*st->d));
    CHECK_ERROR(!st->d, 0, free_state)
    st->channels = channels;
    errcode = ebur128_init_channel_map(st);
    CHECK_ERROR(errcode, 0, free_internal)

    st->d->sample_peak =
        (double *) av_calloc(channels, sizeof(*st->d->sample_peak));
    CHECK_ERROR(!st->d->sample_peak, 0, free_channel_map)

    st->samplerate = samplerate;
    st->d->samples_in_100ms = (st->samplerate + 5) / 10;
    st->mode = mode;
    if ((mode & FF_EBUR128_MODE_S) == FF_EBUR128_MODE_S) {
        st->d->window = FFMAX(window, 3000);
    } else if ((mode & FF_EBUR128_MODE_M) == FF_EBUR128_MODE_M) {
        st->d->window = FFMAX(window, 400);
    } else {
        goto free_sample_peak;
    }
    st->d->audio_data_frames = st->samplerate * st->d->window / 1000;
    if (st->d->audio_data_frames % st->d->samples_in_100ms) {
        /* round up to multiple of samples_in_100ms */
        st->d->audio_data_frames = st->d->audio_data_frames
            + st->d->samples_in_100ms
            - (st->d->audio_data_frames % st->d->samples_in_100ms);
    }
    st->d->audio_data =
        (double *) av_calloc(st->d->audio_data_frames,
                             st->channels * sizeof(*st->d->audio_data));
    CHECK_ERROR(!st->d->audio_data, 0, free_sample_peak)

    ebur128_init_filter(st);

    st->d->block_energy_histogram =
        av_mallocz(1000 * sizeof(*st->d->block_energy_histogram));
    CHECK_ERROR(!st->d->block_energy_histogram, 0, free_audio_data)
    st->d->short_term_block_energy_histogram =
        av_mallocz(1000 * sizeof(*st->d->short_term_block_energy_histogram));
    CHECK_ERROR(!st->d->short_term_block_energy_histogram, 0,
                free_block_energy_histogram)
    st->d->short_term_frame_counter = 0;

    /* the first block needs 400ms of audio data */
    st->d->needed_frames = st->d->samples_in_100ms * 4;
    /* start at the beginning of the buffer */
    st->d->audio_data_index = 0;

    if (ff_thread_once(&histogram_init, &init_histogram) != 0)
        goto free_short_term_block_energy_histogram;

    st->d->data_ptrs = av_malloc_array(channels, sizeof(*st->d->data_ptrs));
    CHECK_ERROR(!st->d->data_ptrs, 0,
                free_short_term_block_energy_histogram);

    /* audio_data_frames is a multiple of samples_in_100ms (see above) */
    st->d->nb_slices = st->d->audio_data_frames / st->d->samples_in_100ms;
    st->d->slice_energy =
        av_calloc(st->d->nb_slices, sizeof(*st->d->slice_energy));
    CHECK_ERROR(!st->d->slice_energy, 0, free_data_ptrs);

    return st;

free_data_ptrs:
    av_free(st->d->data_ptrs);
free_short_term_block_energy_histogram:
    av_free(st->d->short_term_block_energy_histogram);
free_block_energy_histogram:
    av_free(st->d->block_energy_histogram);
free_audio_data:
    av_free(st->d->audio_data);
free_sample_peak:
    av_free(st->d->sample_peak);
free_channel_map:
    av_free(st->d->channel_map);
free_internal:
    av_free(st->d);
free_state:
    av_free(st);
exit:
    return NULL;
}

void ff_ebur128_destroy(FFEBUR128State ** st)
{
    av_free((*st)->d->block_energy_histogram);
    av_free((*st)->d->short_term_block_energy_histogram);
    av_free((*st)->d->audio_data);
    av_free((*st)->d->channel_map);
    av_free((*st)->d->sample_peak);
    av_free((*st)->d->data_ptrs);
    av_free((*st)->d->slice_energy);
    av_free((*st)->d);
    av_free(*st);
    *st = NULL;
}

/* Most channels to filter together. Channels are independent, so running
 * several through the same loop lets the compiler keep all of their filter
 * states in registers and lets the CPU overlap their dependency chains, which
 * a channel-at-a-time loop cannot. */
#define EBUR128_FILTER_LANES 8

/* Apply the BS.1770 filter to nb_lanes channels at once. Each channel does
 * exactly the arithmetic of the one-channel loop, in the same order, so the
 * results are identical. nb_lanes is a compile-time constant at every call
 * site, so the lane loops are unrolled. */
static av_always_inline void ebur128_filter_lanes(FFEBUR128State *st, const double **srcs,
                                                  size_t src_index, size_t frames, int stride,
                                                  double *audio_data,
                                                  const size_t *lane_channel, const int *lane_state,
                                                  const int nb_lanes)
{
    const double a1 = st->d->a[1], a2 = st->d->a[2];
    const double a3 = st->d->a[3], a4 = st->d->a[4];
    const double b0 = st->d->b[0], b1 = st->d->b[1], b2 = st->d->b[2];
    const double b3 = st->d->b[3], b4 = st->d->b[4];
    const size_t channels = st->channels;
    double v1[EBUR128_FILTER_LANES], v2[EBUR128_FILTER_LANES];
    double v3[EBUR128_FILTER_LANES], v4[EBUR128_FILTER_LANES];
    size_t i;
    int k;

    for (k = 0; k < nb_lanes; ++k) {
        v1[k] = st->d->v[lane_state[k]][1];
        v2[k] = st->d->v[lane_state[k]][2];
        v3[k] = st->d->v[lane_state[k]][3];
        v4[k] = st->d->v[lane_state[k]][4];
    }

    for (i = 0; i < frames; ++i) {
        for (k = 0; k < nb_lanes; ++k) {
            const size_t c = lane_channel[k];
            const double v0 = srcs[c][src_index + i * stride]
                            - a1 * v1[k]
                            - a2 * v2[k]
                            - a3 * v3[k]
                            - a4 * v4[k];
            audio_data[i * channels + c] =
                              b0 * v0
                            + b1 * v1[k]
                            + b2 * v2[k]
                            + b3 * v3[k]
                            + b4 * v4[k];
            v4[k] = v3[k];
            v3[k] = v2[k];
            v2[k] = v1[k];
            v1[k] = v0;
        }
    }

    for (k = 0; k < nb_lanes; ++k) {
        double *v = st->d->v[lane_state[k]];
        if (frames)
            v[0] = v1[k];
        v[4] = fabs(v4[k]) < DBL_MIN ? 0.0 : v4[k];
        v[3] = fabs(v3[k]) < DBL_MIN ? 0.0 : v3[k];
        v[2] = fabs(v2[k]) < DBL_MIN ? 0.0 : v2[k];
        v[1] = fabs(v1[k]) < DBL_MIN ? 0.0 : v1[k];
    }
}

static void ebur128_filter_group(FFEBUR128State *st, const double **srcs,
                                 size_t src_index, size_t frames, int stride,
                                 double *audio_data,
                                 const size_t *lane_channel, const int *lane_state,
                                 int nb_lanes)
{
    int j, k;

    /* Two channels sharing one filter state (only possible through a custom
     * channel map) must run one after the other, as the one-channel loop did. */
    for (j = 0; j < nb_lanes; ++j) {
        for (k = j + 1; k < nb_lanes; ++k) {
            if (lane_state[j] == lane_state[k]) {
                for (k = 0; k < nb_lanes; ++k)
                    ebur128_filter_lanes(st, srcs, src_index, frames, stride, audio_data,
                                         lane_channel + k, lane_state + k, 1);
                return;
            }
        }
    }

#define FILTER_LANES_CASE(n)                                                            \
    case n:                                                                             \
        ebur128_filter_lanes(st, srcs, src_index, frames, stride, audio_data,           \
                             lane_channel, lane_state, n);                              \
        break;
    switch (nb_lanes) {
    FILTER_LANES_CASE(1)
    FILTER_LANES_CASE(2)
    FILTER_LANES_CASE(3)
    FILTER_LANES_CASE(4)
    FILTER_LANES_CASE(5)
    FILTER_LANES_CASE(6)
    FILTER_LANES_CASE(7)
    FILTER_LANES_CASE(8)
    }
#undef FILTER_LANES_CASE
}

static void ebur128_filter_channels(FFEBUR128State *st, const double **srcs,
                                    size_t src_index, size_t frames, int stride,
                                    double *audio_data)
{
    size_t lane_channel[EBUR128_FILTER_LANES];
    int lane_state[EBUR128_FILTER_LANES];
    int nb_lanes = 0;
    size_t c;

    for (c = 0; c < st->channels; ++c) {
        int ci = st->d->channel_map[c] - 1;
        if (ci < 0)
            continue;
        else if (ci == FF_EBUR128_DUAL_MONO - 1)
            ci = 0; /* dual mono */
        lane_channel[nb_lanes] = c;
        lane_state[nb_lanes]   = ci;
        if (++nb_lanes == EBUR128_FILTER_LANES) {
            ebur128_filter_group(st, srcs, src_index, frames, stride, audio_data,
                                 lane_channel, lane_state, nb_lanes);
            nb_lanes = 0;
        }
    }
    if (nb_lanes)
        ebur128_filter_group(st, srcs, src_index, frames, stride, audio_data,
                             lane_channel, lane_state, nb_lanes);
}

/* Track the sample peak of nb_lanes consecutive channels starting at first in
 * one pass over the interleaved frames. fabs(v) > max is the same test and the
 * same result as the original v > max / -v > max pair, NaN and -0.0 included. */
static av_always_inline void ebur128_peak_lanes(FFEBUR128State *st, const double **srcs,
                                                size_t src_index, size_t frames, int stride,
                                                size_t first, const int nb_lanes)
{
    double max[EBUR128_FILTER_LANES];
    size_t i;
    int k;

    for (k = 0; k < nb_lanes; ++k)
        max[k] = 0.0;

    for (i = 0; i < frames; ++i) {
        for (k = 0; k < nb_lanes; ++k) {
            const double a = fabs(srcs[first + k][src_index + i * stride]);
            if (a > max[k])
                max[k] = a;
        }
    }

    for (k = 0; k < nb_lanes; ++k)
        if (max[k] > st->d->sample_peak[first + k])
            st->d->sample_peak[first + k] = max[k];
}

static void ebur128_peak_channels(FFEBUR128State *st, const double **srcs,
                                  size_t src_index, size_t frames, int stride)
{
    size_t first;

    for (first = 0; first < st->channels; first += EBUR128_FILTER_LANES) {
        const size_t left = st->channels - first;

#define PEAK_LANES_CASE(n)                                                              \
        case n:                                                                         \
            ebur128_peak_lanes(st, srcs, src_index, frames, stride, first, n);          \
            break;
        switch (left < EBUR128_FILTER_LANES ? left : EBUR128_FILTER_LANES) {
        PEAK_LANES_CASE(1)
        PEAK_LANES_CASE(2)
        PEAK_LANES_CASE(3)
        PEAK_LANES_CASE(4)
        PEAK_LANES_CASE(5)
        PEAK_LANES_CASE(6)
        PEAK_LANES_CASE(7)
        PEAK_LANES_CASE(8)
        }
#undef PEAK_LANES_CASE
    }
}

#define EBUR128_FILTER(type, scaling_factor)                                       \
static void ebur128_filter_##type(FFEBUR128State* st, const type** srcs,           \
                                  size_t src_index, size_t frames,                 \
                                  int stride) {                                    \
    double* audio_data = st->d->audio_data + st->d->audio_data_index;              \
                                                                                   \
    /* Only instantiated for double with a scaling factor of 1.0. */             \
    if ((st->mode & FF_EBUR128_MODE_SAMPLE_PEAK) == FF_EBUR128_MODE_SAMPLE_PEAK)   \
        ebur128_peak_channels(st, srcs, src_index, frames, stride);               \
    ebur128_filter_channels(st, srcs, src_index, frames, stride, audio_data);      \
}
EBUR128_FILTER(double, 1.0)

static double ebur128_energy_to_loudness(double energy)
{
    return 10 * log10(energy) - 0.691;
}

static size_t find_histogram_index(double energy)
{
    size_t index_min = 0;
    size_t index_max = 1000;
    size_t index_mid;

    do {
        index_mid = (index_min + index_max) / 2;
        if (energy >= histogram_energy_boundaries[index_mid]) {
            index_min = index_mid;
        } else {
            index_max = index_mid;
        }
    } while (index_max - index_min != 1);

    return index_min;
}

static double ebur128_channel_weight(int channel)
{
    if (channel == FF_EBUR128_Mp110 ||
        channel == FF_EBUR128_Mm110 ||
        channel == FF_EBUR128_Mp060 ||
        channel == FF_EBUR128_Mm060 ||
        channel == FF_EBUR128_Mp090 ||
        channel == FF_EBUR128_Mm090) {
        return 1.41;
    } else if (channel == FF_EBUR128_DUAL_MONO) {
        return 2.0;
    }
    return 1.0;
}

/* Compute the energy of every 100ms slice that is completed by the frames
 * between start_frame and end_frame of audio_data. Additions never wrap
 * around the end of audio_data, which holds a whole number of slices. */
static void ebur128_update_slice_energy(FFEBUR128State *st,
                                        size_t start_frame, size_t end_frame)
{
    const size_t frames = st->d->samples_in_100ms;
    size_t slice, i, c;

    for (slice = start_frame / frames; (slice + 1) * frames <= end_frame; ++slice) {
        const double *data = st->d->audio_data + slice * frames * st->channels;
        double sum = 0.0;
        for (c = 0; c < st->channels; ++c) {
            double channel_sum = 0.0;
            if (st->d->channel_map[c] == FF_EBUR128_UNUSED)
                continue;
            for (i = 0; i < frames; ++i)
                channel_sum += data[i * st->channels + c] * data[i * st->channels + c];
            sum += channel_sum * ebur128_channel_weight(st->d->channel_map[c]);
        }
        st->d->slice_energy[slice] = sum;
    }
}

static void ebur128_calc_gating_block(FFEBUR128State * st,
                                      size_t frames_per_block,
                                      double *optional_output)
{
    size_t i, c;
    double sum = 0.0;
    double channel_sum;
    const size_t frames_in_slice = st->d->samples_in_100ms;
    const size_t end_frame = st->d->audio_data_index / st->channels;

    /* A block that ends on a slice boundary and spans whole slices is the sum
     * of the slice energies, which avoids summing every sample of the block
     * again. That is the case for every call except after a partial frame. */
    if (end_frame % frames_in_slice == 0 && frames_per_block % frames_in_slice == 0 &&
        frames_per_block / frames_in_slice <= st->d->nb_slices) {
        const size_t nb = frames_per_block / frames_in_slice;
        const size_t end_slice = end_frame / frames_in_slice;
        for (i = 0; i < nb; ++i)
            sum += st->d->slice_energy[(end_slice + 2 * st->d->nb_slices - 1 - i) % st->d->nb_slices];
        goto done;
    }

    for (c = 0; c < st->channels; ++c) {
        if (st->d->channel_map[c] == FF_EBUR128_UNUSED)
            continue;
        channel_sum = 0.0;
        if (st->d->audio_data_index < frames_per_block * st->channels) {
            for (i = 0; i < st->d->audio_data_index / st->channels; ++i) {
                channel_sum += st->d->audio_data[i * st->channels + c] *
                    st->d->audio_data[i * st->channels + c];
            }
            for (i = st->d->audio_data_frames -
                 (frames_per_block -
                  st->d->audio_data_index / st->channels);
                 i < st->d->audio_data_frames; ++i) {
                channel_sum += st->d->audio_data[i * st->channels + c] *
                    st->d->audio_data[i * st->channels + c];
            }
        } else {
            for (i =
                 st->d->audio_data_index / st->channels - frames_per_block;
                 i < st->d->audio_data_index / st->channels; ++i) {
                channel_sum +=
                    st->d->audio_data[i * st->channels +
                                      c] * st->d->audio_data[i *
                                                             st->channels +
                                                             c];
            }
        }
        sum += channel_sum * ebur128_channel_weight(st->d->channel_map[c]);
    }
done:
    sum /= (double) frames_per_block;
    if (optional_output) {
        *optional_output = sum;
    } else if (sum >= histogram_energy_boundaries[0]) {
        ++st->d->block_energy_histogram[find_histogram_index(sum)];
    }
}

int ff_ebur128_set_channel(FFEBUR128State * st,
                           unsigned int channel_number, int value)
{
    if (channel_number >= st->channels) {
        return 1;
    }
    if (value == FF_EBUR128_DUAL_MONO &&
        (st->channels != 1 || channel_number != 0)) {
        return 1;
    }
    st->d->channel_map[channel_number] = value;
    return 0;
}

static int ebur128_energy_shortterm(FFEBUR128State * st, double *out);
#define EBUR128_ADD_FRAMES_PLANAR(type)                                                \
static void ebur128_add_frames_planar_##type(FFEBUR128State* st, const type** srcs,    \
                                 size_t frames, int stride) {                          \
    size_t src_index = 0;                                                              \
    while (frames > 0) {                                                               \
        if (frames >= st->d->needed_frames) {                                          \
            ebur128_filter_##type(st, srcs, src_index, st->d->needed_frames, stride);  \
            src_index += st->d->needed_frames * stride;                                \
            frames -= st->d->needed_frames;                                            \
            st->d->audio_data_index += st->d->needed_frames * st->channels;            \
            ebur128_update_slice_energy(st,                                            \
                st->d->audio_data_index / st->channels - st->d->needed_frames,         \
                st->d->audio_data_index / st->channels);                               \
            /* calculate the new gating block */                                       \
            if ((st->mode & FF_EBUR128_MODE_I) == FF_EBUR128_MODE_I) {                 \
                ebur128_calc_gating_block(st, st->d->samples_in_100ms * 4, NULL);      \
            }                                                                          \
            if ((st->mode & FF_EBUR128_MODE_LRA) == FF_EBUR128_MODE_LRA) {             \
                st->d->short_term_frame_counter += st->d->needed_frames;               \
                if (st->d->short_term_frame_counter == st->d->samples_in_100ms * 30) { \
                    double st_energy;                                                  \
                    ebur128_energy_shortterm(st, &st_energy);                          \
                    if (st_energy >= histogram_energy_boundaries[0]) {                 \
                        ++st->d->short_term_block_energy_histogram[                    \
                                                    find_histogram_index(st_energy)];  \
                    }                                                                  \
                    st->d->short_term_frame_counter = st->d->samples_in_100ms * 20;    \
                }                                                                      \
            }                                                                          \
            /* 100ms are needed for all blocks besides the first one */                \
            st->d->needed_frames = st->d->samples_in_100ms;                            \
            /* reset audio_data_index when buffer full */                              \
            if (st->d->audio_data_index == st->d->audio_data_frames * st->channels) {  \
                st->d->audio_data_index = 0;                                           \
            }                                                                          \
        } else {                                                                       \
            ebur128_filter_##type(st, srcs, src_index, frames, stride);                \
            st->d->audio_data_index += frames * st->channels;                          \
            ebur128_update_slice_energy(st,                                            \
                st->d->audio_data_index / st->channels - frames,                       \
                st->d->audio_data_index / st->channels);                               \
            if ((st->mode & FF_EBUR128_MODE_LRA) == FF_EBUR128_MODE_LRA) {             \
                st->d->short_term_frame_counter += frames;                             \
            }                                                                          \
            st->d->needed_frames -= frames;                                            \
            frames = 0;                                                                \
        }                                                                              \
    }                                                                                  \
}
EBUR128_ADD_FRAMES_PLANAR(double)
#define FF_EBUR128_ADD_FRAMES(type)                                            \
void ff_ebur128_add_frames_##type(FFEBUR128State* st, const type* src,         \
                                    size_t frames) {                           \
  int i;                                                                       \
  const type **buf = (const type**)st->d->data_ptrs;                           \
  for (i = 0; i < st->channels; i++)                                           \
    buf[i] = src + i;                                                          \
  ebur128_add_frames_planar_##type(st, buf, frames, st->channels);             \
}
FF_EBUR128_ADD_FRAMES(double)

static int ebur128_calc_relative_threshold(FFEBUR128State **sts, size_t size,
                                           double *relative_threshold)
{
    size_t i, j;
    int above_thresh_counter = 0;
    *relative_threshold = 0.0;

    for (i = 0; i < size; i++) {
        unsigned long *block_energy_histogram = sts[i]->d->block_energy_histogram;
        for (j = 0; j < 1000; ++j) {
            *relative_threshold += block_energy_histogram[j] * histogram_energies[j];
            above_thresh_counter += block_energy_histogram[j];
        }
    }

    if (above_thresh_counter != 0) {
        *relative_threshold /= (double)above_thresh_counter;
        *relative_threshold *= RELATIVE_GATE_FACTOR;
    }

    return above_thresh_counter;
}

static int ebur128_gated_loudness(FFEBUR128State ** sts, size_t size,
                                  double *out)
{
    double gated_loudness = 0.0;
    double relative_threshold;
    size_t above_thresh_counter;
    size_t i, j, start_index;

    for (i = 0; i < size; i++)
        if ((sts[i]->mode & FF_EBUR128_MODE_I) != FF_EBUR128_MODE_I)
            return AVERROR(EINVAL);

    if (!ebur128_calc_relative_threshold(sts, size, &relative_threshold)) {
        *out = -HUGE_VAL;
        return 0;
    }

    above_thresh_counter = 0;
    if (relative_threshold < histogram_energy_boundaries[0]) {
        start_index = 0;
    } else {
        start_index = find_histogram_index(relative_threshold);
        if (relative_threshold > histogram_energies[start_index]) {
            ++start_index;
        }
    }
    for (i = 0; i < size; i++) {
        for (j = start_index; j < 1000; ++j) {
            gated_loudness += sts[i]->d->block_energy_histogram[j] *
                histogram_energies[j];
            above_thresh_counter += sts[i]->d->block_energy_histogram[j];
        }
    }
    if (!above_thresh_counter) {
        *out = -HUGE_VAL;
        return 0;
    }
    gated_loudness /= (double) above_thresh_counter;
    *out = ebur128_energy_to_loudness(gated_loudness);
    return 0;
}

int ff_ebur128_relative_threshold(FFEBUR128State * st, double *out)
{
    double relative_threshold;

    if ((st->mode & FF_EBUR128_MODE_I) != FF_EBUR128_MODE_I)
        return AVERROR(EINVAL);

    if (!ebur128_calc_relative_threshold(&st, 1, &relative_threshold)) {
        *out = -70.0;
        return 0;
    }

    *out = ebur128_energy_to_loudness(relative_threshold);
    return 0;
}

int ff_ebur128_loudness_global(FFEBUR128State * st, double *out)
{
    return ebur128_gated_loudness(&st, 1, out);
}

static int ebur128_energy_in_interval(FFEBUR128State * st,
                                      size_t interval_frames, double *out)
{
    if (interval_frames > st->d->audio_data_frames) {
        return AVERROR(EINVAL);
    }
    ebur128_calc_gating_block(st, interval_frames, out);
    return 0;
}

static int ebur128_energy_shortterm(FFEBUR128State * st, double *out)
{
    return ebur128_energy_in_interval(st, st->d->samples_in_100ms * 30,
                                      out);
}

int ff_ebur128_loudness_shortterm(FFEBUR128State * st, double *out)
{
    double energy;
    int error = ebur128_energy_shortterm(st, &energy);
    if (error) {
        return error;
    } else if (energy <= 0.0) {
        *out = -HUGE_VAL;
        return 0;
    }
    *out = ebur128_energy_to_loudness(energy);
    return 0;
}

/* EBU - TECH 3342 */
int ff_ebur128_loudness_range_multiple(FFEBUR128State ** sts, size_t size,
                                       double *out)
{
    size_t i, j;
    size_t stl_size;
    double stl_power, stl_integrated;
    /* High and low percentile energy */
    double h_en, l_en;
    unsigned long hist[1000] = { 0 };
    size_t percentile_low, percentile_high;
    size_t index;

    for (i = 0; i < size; ++i) {
        if (sts[i]) {
            if ((sts[i]->mode & FF_EBUR128_MODE_LRA) !=
                FF_EBUR128_MODE_LRA) {
                return AVERROR(EINVAL);
            }
        }
    }

    stl_size = 0;
    stl_power = 0.0;
    for (i = 0; i < size; ++i) {
        if (!sts[i])
            continue;
        for (j = 0; j < 1000; ++j) {
            hist[j] += sts[i]->d->short_term_block_energy_histogram[j];
            stl_size += sts[i]->d->short_term_block_energy_histogram[j];
            stl_power += sts[i]->d->short_term_block_energy_histogram[j]
                * histogram_energies[j];
        }
    }
    if (!stl_size) {
        *out = 0.0;
        return 0;
    }

    stl_power /= stl_size;
    stl_integrated = MINUS_20DB * stl_power;

    if (stl_integrated < histogram_energy_boundaries[0]) {
        index = 0;
    } else {
        index = find_histogram_index(stl_integrated);
        if (stl_integrated > histogram_energies[index]) {
            ++index;
        }
    }
    stl_size = 0;
    for (j = index; j < 1000; ++j) {
        stl_size += hist[j];
    }
    if (!stl_size) {
        *out = 0.0;
        return 0;
    }

    percentile_low = (size_t) ((stl_size - 1) * 0.1 + 0.5);
    percentile_high = (size_t) ((stl_size - 1) * 0.95 + 0.5);

    stl_size = 0;
    j = index;
    while (stl_size <= percentile_low) {
        stl_size += hist[j++];
    }
    l_en = histogram_energies[j - 1];
    while (stl_size <= percentile_high) {
        stl_size += hist[j++];
    }
    h_en = histogram_energies[j - 1];
    *out =
        ebur128_energy_to_loudness(h_en) -
        ebur128_energy_to_loudness(l_en);
    return 0;
}

int ff_ebur128_loudness_range(FFEBUR128State * st, double *out)
{
    return ff_ebur128_loudness_range_multiple(&st, 1, out);
}

int ff_ebur128_sample_peak(FFEBUR128State * st,
                           unsigned int channel_number, double *out)
{
    if ((st->mode & FF_EBUR128_MODE_SAMPLE_PEAK) !=
        FF_EBUR128_MODE_SAMPLE_PEAK) {
        return AVERROR(EINVAL);
    } else if (channel_number >= st->channels) {
        return AVERROR(EINVAL);
    }
    *out = st->d->sample_peak[channel_number];
    return 0;
}
