/* mpp_dec_yuv.c — decode an Annex-B HEVC file with Rockchip MPP and write
 * cropped I420, for the §6.3b Phase C acceptance readout.
 *
 * Why this exists rather than `--decoder gst:<mpp element>`: the shipped
 * ground image (OpenIPC SBC GS, rk3566) carries librockchip_mpp but NO
 * gstreamer rockchip plugin, no mpi_dec_test and no compiler — the README's
 * gst recipe has nothing to bind to there.
 *
 * Why not stock mpi_dec_test even where it exists: it decodes with MPP
 * DEFAULTS. waybeam-hub does not. The four settings below are copied from
 * `waybeam-hub/src/pixelpilot/video_decoder.c` `set_mpp_decoding_parameters()`
 * and are the whole point — an acceptance test against a configuration we do
 * not ship proves nothing about what the operator sees.
 *
 * Prints one line per frame: index, errinfo, discard. MPP reporting
 * `errinfo == 0` is NOT an acceptance signal (it does that on an
 * intra-picture slice gap while the picture is wrong) — compare the written
 * pictures with decode_compare.py.
 *
 * Build (aarch64, buildroot sysroot from sbc-groundstations):
 *   aarch64-none-linux-gnu-gcc -O2 -Wall -Wextra mpp_dec_yuv.c \
 *     --sysroot=$SYSROOT -I$SYSROOT/usr/include/rockchip \
 *     -lrockchip_mpp -o mpp_dec_yuv
 *
 * Usage: mpp_dec_yuv <in.265> <out.yuv> [max_frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rk_mpi.h>

#define READ_CHUNK (256 * 1024)

/* MPP hands back NV12 over an aligned buffer; the comparison wants packed
 * I420 at the display size, so crop and deinterleave here rather than
 * teaching every consumer about hor_stride. */
static int write_i420(FILE *out, const unsigned char *base, int hs, int vs,
                      int w, int h) {
    for (int y = 0; y < h; y++) {
        if (fwrite(base + (size_t)y * hs, 1, w, out) != (size_t)w) return -1;
    }
    const unsigned char *uv = base + (size_t)hs * vs;
    int cw = w / 2, ch = h / 2;
    unsigned char *line = malloc(cw);
    if (!line) return -1;
    for (int p = 0; p < 2; p++) {          /* U then V out of interleaved UV */
        for (int y = 0; y < ch; y++) {
            const unsigned char *src = uv + (size_t)y * hs;
            for (int x = 0; x < cw; x++) line[x] = src[2 * x + p];
            if (fwrite(line, 1, cw, out) != (size_t)cw) { free(line); return -1; }
        }
    }
    free(line);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.265> <out.yuv> [max_frames]\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1], *out_path = argv[2];
    int max_frames = (argc > 3) ? atoi(argv[3]) : 0;

    FILE *in = fopen(in_path, "rb");
    if (!in) { perror("open input"); return 2; }
    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("open output"); fclose(in); return 2; }

    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    if (mpp_create(&ctx, &mpi) != MPP_OK) { fprintf(stderr, "mpp_create failed\n"); return 2; }
    if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC) != MPP_OK) {
        fprintf(stderr, "mpp_init failed\n"); return 2;
    }

    /* --- waybeam-hub's shipped decoder configuration, verbatim --- */
    MppDecCfg cfg = NULL;
    if (mpp_dec_cfg_init(&cfg) == MPP_OK) {
        if (mpi->control(ctx, MPP_DEC_GET_CFG, cfg) == MPP_OK) {
            mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
            mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
        }
        mpp_dec_cfg_deinit(cfg);
    }
    RK_U32 on = 0xffff;
    mpi->control(ctx, MPP_DEC_SET_DISABLE_ERROR, &on);
    mpi->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &on);
    mpi->control(ctx, MPP_DEC_SET_ENABLE_FAST_PLAY, &on);
    /* Block in decode_get_frame instead of spinning; the loop below has no
     * other pacing. */
    RK_S64 timeout = 50;
    mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);

    unsigned char *buf = malloc(READ_CHUNK);
    MppPacket packet = NULL;
    if (!buf || mpp_packet_init(&packet, buf, READ_CHUNK) != MPP_OK) {
        fprintf(stderr, "packet init failed\n"); return 2;
    }

    MppBufferGroup grp = NULL;
    int frames = 0, err_frames = 0;
    int pkt_done = 1, eos_sent = 0, frm_eos = 0, idle = 0;
    while (!frm_eos) {
        if (pkt_done) {                     /* previous packet fully accepted */
            size_t n = eos_sent ? 0 : fread(buf, 1, READ_CHUNK, in);
            if (n > 0) {
                mpp_packet_set_pos(packet, buf);
                mpp_packet_set_length(packet, n);
                pkt_done = 0;
            } else if (!eos_sent) {
                mpp_packet_set_pos(packet, buf);
                mpp_packet_set_length(packet, 0);
                mpp_packet_set_eos(packet);
                eos_sent = 1;
                pkt_done = 0;
            }
        }
        if (!pkt_done && mpi->decode_put_packet(ctx, packet) == MPP_OK) {
            pkt_done = 1;
        }

        for (;;) {                          /* drain everything available */
            MppFrame frame = NULL;
            if (mpi->decode_get_frame(ctx, &frame) != MPP_OK || !frame) break;

            if (mpp_frame_get_info_change(frame)) {
                /* MPP's internal pool is too small for this stream's
                 * reference structure and deadlocks the feed loop partway
                 * through; the decoder must be given a group. 24 buffers is
                 * both mpi_dec_test's count and the hub's
                 * DECODER_MAX_FRAMES. */
                printf("info change: %dx%d stride %dx%d fmt %d buf_size %zu\n",
                       mpp_frame_get_width(frame), mpp_frame_get_height(frame),
                       mpp_frame_get_hor_stride(frame),
                       mpp_frame_get_ver_stride(frame),
                       mpp_frame_get_fmt(frame),
                       mpp_frame_get_buf_size(frame));
                if (!grp)
                    mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
                if (grp) {
                    mpp_buffer_group_limit_config(
                        grp, mpp_frame_get_buf_size(frame), 24);
                    mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
                }
                mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                mpp_frame_deinit(&frame);
                continue;
            }

            RK_U32 errinfo = mpp_frame_get_errinfo(frame);
            RK_U32 discard = mpp_frame_get_discard(frame);
            if (errinfo || discard) err_frames++;
            printf("frame %d errinfo %u discard %u\n", frames, errinfo, discard);

            MppBuffer b = mpp_frame_get_buffer(frame);
            if (b) {
                if (write_i420(out, mpp_buffer_get_ptr(b),
                               mpp_frame_get_hor_stride(frame),
                               mpp_frame_get_ver_stride(frame),
                               mpp_frame_get_width(frame),
                               mpp_frame_get_height(frame)) != 0) {
                    fprintf(stderr, "write failed at frame %d\n", frames);
                    mpp_frame_deinit(&frame);
                    frm_eos = 1;
                    break;
                }
                frames++;
            }
            if (mpp_frame_get_eos(frame)) frm_eos = 1;
            mpp_frame_deinit(&frame);
            idle = 0;
            if (frm_eos || (max_frames && frames >= max_frames)) {
                frm_eos = 1;
                break;
            }
        }
        /* EOS fed and nothing coming back for ~3 s: the decoder is done and
         * simply never raised the flag. Bounded, so a wedge exits nonzero
         * rather than hanging a test run. */
        if (eos_sent && pkt_done && ++idle > 60) break;
    }

    printf("decoded %d frames, %d with errinfo/discard\n", frames, err_frames);
    mpi->reset(ctx);
    mpp_packet_deinit(&packet);
    mpp_destroy(ctx);
    if (grp) mpp_buffer_group_put(grp);
    free(buf);
    fclose(in);
    fclose(out);
    return frames > 0 ? 0 : 2;
}
