/*
 * QTest: i.MX93 CCM audio-clock gating survives migration (post_load).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The CCM's audio root clocks (pdm_root, spdif_root) are derived from its
 * registers and propagated to their consumers (MICFIL, XCVR), which read the
 * rate live and do NOT migrate it themselves. The CCM registers migrate but the
 * derived clock outputs do not, so without imx93_ccm_post_load a destination
 * re-establishes the RESET-era rate rather than the migrated one. MICFIL is the
 * observable consumer: with its PDM LPCG cleared there is no clock and a running
 * capture's DATACH0 reads 0; with the clock on it pops the synthesised ramp.
 *
 * Proof: on the source, start a capture and then GATE the PDM clock off, so
 * DATACH0 reads 0. Migrate. On the destination the PDM LPCG is still clear in
 * the migrated registers, so post_load must re-derive pdm_root = 0 and DATACH0
 * must stay 0. Without post_load the destination reverts to the reset default
 * (PDM LPCG on) and DATACH0 pops the ramp again.
 *
 * Mutation-proven: drop imx93_ccm_post_load and the destination DATACH0 reads
 * non-zero (the reset-era clock, not the migrated gated-off state).
 *
 * Part of the joint #5 (migration post_load) pass; sibling of
 * imx93-edma-migration-test.c and 91's imx91-lpuart-migration-test.c.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define CCM_BASE        0x44450000ULL
#define CCM_PDM_DIRECT  (CCM_BASE + 0x9ac0ULL)  /* PDM LPCG (LPCG107) */
#define GATE_ON         0x1u

#define MICFIL_BASE     0x44520000ULL
#define MICFIL_CTRL1    0x00
#define MICFIL_DATACH0  0x24
#define CTRL1_PDMIEN    (1u << 29)              /* enable PDM capture */

static void wait_for_migration_completed(QTestState *who)
{
    while (true) {
        QDict *rsp = qtest_qmp(who, "{ 'execute': 'query-migrate' }");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = qdict_get_str(ret, "status");
        bool done = g_str_equal(status, "completed");
        bool failed = g_str_equal(status, "failed");

        qobject_unref(rsp);
        if (failed) {
            g_test_message("migration failed");
            g_assert_not_reached();
        }
        if (done) {
            return;
        }
        g_usleep(1000);
    }
}

/* Count non-zero DATACH0 reads: non-zero => clocked, zero => gated/drained. */
static int micfil_nonzero_of(QTestState *qts, int n)
{
    int nz = 0, k;

    for (k = 0; k < n; k++) {
        if (qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0) != 0) {
            nz++;
        }
    }
    return nz;
}

static void test_ccm_audio_gate_survives_migration(void)
{
    g_autofree char *tmp = g_dir_make_tmp("imx93-ccm-mig.XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *sock = g_strdup_printf("%s/mig.sock", tmp);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    /*
     * Source: start a PDM capture (DATACH0 pops the ramp), then clear the PDM
     * LPCG so the clock drops to 0 and the capture goes silent. This is the
     * migrated state: MICFIL running, its clock gated off.
     */
    QTestState *src = qtest_init("-machine imx93-11x11-evk -display none");
    g_assert_cmphex(qtest_readl(src, CCM_PDM_DIRECT) & GATE_ON, ==, GATE_ON);
    qtest_writel(src, MICFIL_BASE + MICFIL_CTRL1, CTRL1_PDMIEN);
    g_assert_cmpint(micfil_nonzero_of(src, 6), >, 0);   /* clocked: ramp */
    qtest_writel(src, CCM_PDM_DIRECT, 0);               /* gate the PDM clock */
    g_assert_cmpint(micfil_nonzero_of(src, 6), ==, 0);  /* gated: silent */

    /* Destination: -incoming defer, then load the source's (gated) state. */
    QTestState *dst = qtest_init("-machine imx93-11x11-evk -display none "
                                 "-incoming defer");

    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                                  "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                                  "  'arguments': { 'uri': %s } }", uri);
    wait_for_migration_completed(src);

    /*
     * post_load must re-derive pdm_root from the migrated (cleared) PDM LPCG, so
     * the capture stays gated off on the destination. Without it the destination
     * reverts to the reset default (clock on) and DATACH0 pops the ramp.
     */
    g_assert_cmphex(qtest_readl(dst, CCM_PDM_DIRECT) & GATE_ON, ==, 0);
    g_assert_cmpint(micfil_nonzero_of(dst, 6), ==, 0);

    qtest_quit(dst);
    qtest_quit(src);
    unlink(sock);
    rmdir(tmp);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx93/ccm/audio-gate-survives-migration",
                   test_ccm_audio_gate_survives_migration);
    return g_test_run();
}
