/*
 * Unit tests for the Arm Ethos-U register command-stream decoder.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercises ethos_u_cmdstream_decode() (the pure parser) with hand-built
 * streams: cmd0/cmd1 framing, NPU_SET_* -> descriptor accumulation, absolute
 * address resolution from the region bases, and malformed-stream handling.
 */

#include "qemu/osdep.h"
#include "../../hw/npu/ethos_u_internal.h"
#include "../../hw/npu/ethos_u_addr.h"

/* Little-endian command-stream emitter. */
typedef struct {
    uint8_t buf[4096];
    uint32_t len;
} Stream;

static void emit_cmd0(Stream *s, uint16_t opcode, uint16_t imm)
{
    s->buf[s->len++] = opcode & 0xff;
    s->buf[s->len++] = (opcode >> 8) & 0xff;
    s->buf[s->len++] = imm & 0xff;
    s->buf[s->len++] = (imm >> 8) & 0xff;
}

static void emit_cmd1(Stream *s, uint16_t opcode, uint32_t data)
{
    uint16_t code = opcode | ETHOS_U_CMD_PAYLOAD_BIT;

    s->buf[s->len++] = code & 0xff;
    s->buf[s->len++] = (code >> 8) & 0xff;
    s->buf[s->len++] = 0;        /* cmd1 16-bit param (unused here) */
    s->buf[s->len++] = 0;
    s->buf[s->len++] = data & 0xff;
    s->buf[s->len++] = (data >> 8) & 0xff;
    s->buf[s->len++] = (data >> 16) & 0xff;
    s->buf[s->len++] = (data >> 24) & 0xff;
}

typedef struct {
    int n_ops;
    uint16_t last_opcode;
    EthosUOpDesc last;
} Record;

static void record_handler(void *ctx, uint16_t opcode, const EthosUOpDesc *op)
{
    Record *r = ctx;

    r->n_ops++;
    r->last_opcode = opcode;
    r->last = *op;
}

/* A single CONV op: check framing, descriptor accumulation, addr resolution. */
static void test_conv(void)
{
    Stream s = { 0 };
    Record r = { 0 };
    uint64_t basep[ETHOS_U_NUM_BASEP] = { 0 };
    bool ok;

    basep[1] = 0xc0040000;   /* arena / IFM+OFM region */
    basep[0] = 0xc0000000;   /* weights+scales region */

    /* IFM 16x16x1, region 1, base 0x1000. */
    emit_cmd0(&s, NPU_SET_IFM_REGION, 1);
    emit_cmd1(&s, NPU_SET_IFM_BASE0, 0x1000);
    emit_cmd0(&s, NPU_SET_IFM_WIDTH0_M1, 15);
    emit_cmd0(&s, NPU_SET_IFM_HEIGHT0_M1, 15);
    emit_cmd0(&s, NPU_SET_IFM_DEPTH_M1, 0);
    emit_cmd0(&s, NPU_SET_IFM_ZERO_POINT, (uint16_t)-128);
    /* OFM 16x16x8, region 1, base 0. */
    emit_cmd0(&s, NPU_SET_OFM_REGION, 1);
    emit_cmd1(&s, NPU_SET_OFM_BASE0, 0);
    emit_cmd0(&s, NPU_SET_OFM_WIDTH_M1, 15);
    emit_cmd0(&s, NPU_SET_OFM_HEIGHT_M1, 15);
    emit_cmd0(&s, NPU_SET_OFM_DEPTH_M1, 7);
    /* 3x3 kernel, stride 1. */
    emit_cmd0(&s, NPU_SET_KERNEL_WIDTH_M1, 2);
    emit_cmd0(&s, NPU_SET_KERNEL_HEIGHT_M1, 2);
    emit_cmd0(&s, NPU_SET_KERNEL_STRIDE, 0);
    /* weights region 0, base 0x2000; scales region 0, base 0x100. */
    emit_cmd0(&s, NPU_SET_WEIGHT_REGION, 0);
    emit_cmd1(&s, NPU_SET_WEIGHT_BASE, 0x2000);
    emit_cmd1(&s, NPU_SET_WEIGHT_LENGTH, 0x90);
    emit_cmd0(&s, NPU_SET_SCALE_REGION, 0);
    emit_cmd1(&s, NPU_SET_SCALE_BASE, 0x100);
    emit_cmd0(&s, NPU_SET_ACTIVATION_MIN, (uint16_t)-128);
    emit_cmd0(&s, NPU_SET_ACTIVATION_MAX, 127);
    emit_cmd0(&s, NPU_OP_CONV, 0);
    emit_cmd0(&s, NPU_OP_STOP, 0);

    ok = ethos_u_cmdstream_decode(s.buf, s.len, basep, record_handler, &r);
    g_assert_true(ok);
    g_assert_cmpint(r.n_ops, ==, 1);
    g_assert_cmpuint(r.last_opcode, ==, NPU_OP_CONV);

    g_assert_cmpint(r.last.ifm_w, ==, 16);
    g_assert_cmpint(r.last.ifm_h, ==, 16);
    g_assert_cmpint(r.last.ifm_c, ==, 1);
    g_assert_cmpint(r.last.ifm_zp, ==, -128);
    g_assert_cmpint(r.last.ofm_w, ==, 16);
    g_assert_cmpint(r.last.ofm_h, ==, 16);
    g_assert_cmpint(r.last.ofm_c, ==, 8);
    g_assert_cmpint(r.last.kw, ==, 3);
    g_assert_cmpint(r.last.kh, ==, 3);
    g_assert_cmpint(r.last.stride_x, ==, 1);
    g_assert_cmpint(r.last.stride_y, ==, 1);
    g_assert_cmpint(r.last.act_min, ==, -128);
    g_assert_cmpint(r.last.act_max, ==, 127);

    /* Absolute addresses resolved as basep[region] + offset. */
    g_assert_cmphex(r.last.ifm_addr, ==, 0xc0041000);
    g_assert_cmphex(r.last.ofm_addr, ==, 0xc0040000);
    g_assert_cmphex(r.last.weight_addr, ==, 0xc0002000);
    g_assert_cmphex(r.last.scale_addr, ==, 0xc0000100);
}

/* Registers are sticky across ops, like the hardware. */
static void test_sticky_regs(void)
{
    Stream s = { 0 };
    Record r = { 0 };
    uint64_t basep[ETHOS_U_NUM_BASEP] = { 0 };

    emit_cmd0(&s, NPU_SET_OFM_REGION, 2);
    emit_cmd0(&s, NPU_SET_OFM_DEPTH_M1, 3);
    emit_cmd0(&s, NPU_OP_POOL, 0);
    emit_cmd0(&s, NPU_OP_POOL, 0);     /* second op inherits the same regs */
    emit_cmd0(&s, NPU_OP_STOP, 0);

    g_assert_true(ethos_u_cmdstream_decode(s.buf, s.len, basep,
                                           record_handler, &r));
    g_assert_cmpint(r.n_ops, ==, 2);
    g_assert_cmpint(r.last.ofm_c, ==, 4);
}

/* A truncated cmd1 (payload runs off the end) must be rejected. */
static void test_truncated_cmd1(void)
{
    Stream s = { 0 };
    Record r = { 0 };
    uint64_t basep[ETHOS_U_NUM_BASEP] = { 0 };
    uint16_t code = NPU_SET_OFM_BASE0 | ETHOS_U_CMD_PAYLOAD_BIT;

    /* code word present but the 4-byte payload is missing. */
    s.buf[s.len++] = code & 0xff;
    s.buf[s.len++] = (code >> 8) & 0xff;
    s.buf[s.len++] = 0;
    s.buf[s.len++] = 0;

    g_assert_false(ethos_u_cmdstream_decode(s.buf, s.len, basep,
                                            record_handler, &r));
    g_assert_cmpint(r.n_ops, ==, 0);
}

/* Bad sizes are rejected. */
static void test_bad_size(void)
{
    uint8_t buf[8] = { 0 };
    uint64_t basep[ETHOS_U_NUM_BASEP] = { 0 };

    g_assert_false(ethos_u_cmdstream_decode(buf, 0, basep, NULL, NULL));
    g_assert_false(ethos_u_cmdstream_decode(buf, 3, basep, NULL, NULL));
}

/* NHWC addressing: off = y*sy + x*sx + c*sc. */
static void test_addr_nhwc(void)
{
    /* 16x16x8 int8: stride_c=1, stride_x=8, stride_y=128. */
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHWC, 0, 0, 0,
                                       128, 8, 1, 1), ==, 0);
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHWC, 0, 0, 5,
                                       128, 8, 1, 1), ==, 5);
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHWC, 0, 1, 0,
                                       128, 8, 1, 1), ==, 8);
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHWC, 2, 3, 4,
                                       128, 8, 1, 1), ==, 2 * 128 + 3 * 8 + 4);
}

/* NHCWB16: channels in bricks of 16. */
static void test_addr_nhcwb16(void)
{
    /* width=16: stride_x=16, stride_c(brick)=16*16=256, stride_y=16*16=256. */
    int sy = 256, sx = 16, sc = 256, elem = 1;

    /* c < 16 stays in brick 0 at (c%16)*elem. */
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHCWB16, 0, 0, 0,
                                       sy, sx, sc, elem), ==, 0);
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHCWB16, 0, 0, 7,
                                       sy, sx, sc, elem), ==, 7);
    /* c = 16 -> next brick (c/16 = 1) -> stride_c. */
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHCWB16, 0, 0, 16,
                                       sy, sx, sc, elem), ==, 256);
    /* c = 17 -> brick 1, within-brick 1. */
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHCWB16, 0, 0, 17,
                                       sy, sx, sc, elem), ==, 256 + 1);
    /* x advances by stride_x within a brick. */
    g_assert_cmpuint(ethos_u_fm_offset(ETHOS_U_LAYOUT_NHCWB16, 0, 1, 0,
                                       sy, sx, sc, elem), ==, 16);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ethos-u/cmdstream/conv", test_conv);
    g_test_add_func("/ethos-u/addr/nhwc", test_addr_nhwc);
    g_test_add_func("/ethos-u/addr/nhcwb16", test_addr_nhcwb16);
    g_test_add_func("/ethos-u/cmdstream/sticky-regs", test_sticky_regs);
    g_test_add_func("/ethos-u/cmdstream/truncated-cmd1", test_truncated_cmd1);
    g_test_add_func("/ethos-u/cmdstream/bad-size", test_bad_size);
    return g_test_run();
}
