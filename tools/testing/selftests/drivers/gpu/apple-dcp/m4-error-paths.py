#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production M4 display error handlers with mocked IRQ/DRM ownership.

These host tests check error isolation and callback ownership; they do not
validate live DART registers, firmware DMA or a successful display handoff.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def definition(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[6])
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = args.source
    pmp = (root / 'drivers/soc/apple/pmp-display.c').read_text()
    dart = (root / 'drivers/iommu/apple-dart.c').read_text()
    dcp = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
    common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t resource_size_t;
#define BIT(n) (1U << (n))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctz(mask))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define IRQ_NONE 0
#define IRQ_HANDLED 1
typedef int irqreturn_t;
#define dev_err_ratelimited(...) test_log(__VA_ARGS__)
static void test_log(void *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
#define lockdep_assert_held(p) assert(*(p))
'''
    votes = common + '\n#define PMP_ENDPOINT 0x20\n#define PMP_POWER_REPLY 0x0021000000000000ULL\n'
    votes += definition(pmp, 'enum pmp_power_kind {') + ';\n'
    votes += definition(pmp, 'struct pmp_power_vote {') + ';\n'
    for function in ('static void pmp_vote_fail(', 'static int pmp_vote_begin(',
                     'static int pmp_vote_check(', 'static int pmp_vote_finish(',
                     'static void pmp_vote_reply('):
        votes += definition(pmp, function) + '\n'
    votes += r'''
int main(void) {
    struct pmp_power_vote s = {0};
    assert(pmp_vote_begin(&s, PMP_POWER_DISP) == 1);
    pmp_vote_reply(&s, PMP_ENDPOINT, PMP_POWER_REPLY);
    assert(pmp_vote_finish(&s, PMP_POWER_DISP) == 0);
    assert(pmp_vote_begin(&s, PMP_POWER_AGX) == 1);
    pmp_vote_fail(&s, -EIO);
    assert(pmp_vote_check(&s, PMP_POWER_DISP) == 0);
    assert(pmp_vote_begin(&s, PMP_POWER_DISP) == 0);
    assert(pmp_vote_check(&s, PMP_POWER_AGX) == -EIO);
    assert(pmp_vote_begin(&s, PMP_POWER_AGX) == -EIO);
    s.provider_error = -EHOSTDOWN;
    assert(pmp_vote_check(&s, PMP_POWER_DISP) == -EHOSTDOWN);
    assert(pmp_vote_begin(&s, PMP_POWER_DISP) == -EHOSTDOWN);

    /* Both lock orderings of a reply racing timeout. */
    s = (struct pmp_power_vote){0};
    assert(pmp_vote_begin(&s, PMP_POWER_DISP) == 1);
    pmp_vote_reply(&s, PMP_ENDPOINT, PMP_POWER_REPLY);
    assert(pmp_vote_finish(&s, PMP_POWER_DISP) == 0);
    s = (struct pmp_power_vote){0};
    assert(pmp_vote_begin(&s, PMP_POWER_AGX) == 1);
    assert(pmp_vote_finish(&s, PMP_POWER_AGX) == -ETIMEDOUT);
    pmp_vote_reply(&s, PMP_ENDPOINT, PMP_POWER_REPLY);
    assert(pmp_vote_check(&s, PMP_POWER_AGX) == -ETIMEDOUT);
    assert(!s.held[PMP_POWER_AGX]);

    /* Malformed/unsolicited replies close admission without revoking DISP. */
    s = (struct pmp_power_vote){0};
    assert(pmp_vote_begin(&s, PMP_POWER_DISP) == 1);
    pmp_vote_reply(&s, PMP_ENDPOINT, PMP_POWER_REPLY);
    pmp_vote_reply(&s, PMP_ENDPOINT, PMP_POWER_REPLY);
    assert(pmp_vote_check(&s, PMP_POWER_DISP) == 0);
    assert(pmp_vote_begin(&s, PMP_POWER_AGX) == -EPROTO);
    return 0;
}
'''
    irq = common
    for line in dart.splitlines():
        if line.startswith('#define DART_T8110_ERROR'):
            irq += line.replace('GENMASK(27, 20)', '0x0ff00000U').replace(
                'GENMASK(14, 0)', '0x00007fffU') + '\n'
    irq += r'''
struct apple_dart_dcp { bool failed; };
struct apple_dart {
    void *dev;
    u8 *regs;
    struct apple_dart_dcp *dcp, *frontend[2];
};
static struct apple_dart_dcp *apple_dart_borrowed_sid(struct apple_dart *d, unsigned sid) {
    return sid == 23 ? d->dcp : sid == 0 ? d->frontend[0] : sid == 16 ? d->frontend[1] : NULL;
}
static unsigned writes;
static u32 written[2];
static uintptr_t written_addr[2];
static u32 readl(const void *p) { u32 v; memcpy(&v, p, sizeof(v)); return v; }
static void writel(u32 v, void *p) {
    assert(writes < 2); written[writes] = v; written_addr[writes++] = (uintptr_t)p;
}
'''
    irq += definition(dart, 'static irqreturn_t apple_dart_dcp_irq(') + '\n'
    irq += r'''
int main(void) {
    u8 regs[0x200] = {0};
    struct apple_dart_dcp dcp = {0}, fe = {0};
    struct apple_dart d = {.regs = regs, .dcp = &dcp, .frontend = {&fe, NULL}};
    u32 error = DART_T8110_ERROR_FLAG | (5U << 20) | DART_T8110_ERROR_NO_PTE;
    memcpy(regs + DART_T8110_ERROR, &error, 4);
    assert(apple_dart_dcp_irq(&d) == IRQ_HANDLED);
    assert(dcp.failed && fe.failed);
    assert(writes == 2 && written[0] == error && written[1] == BIT(5));
    assert(written_addr[0] == (uintptr_t)(regs + DART_T8110_ERROR));
    assert(written_addr[1] == (uintptr_t)(regs + DART_T8110_ERROR_STREAMS));
    writes = 0;
    error = DART_T8110_ERROR_FLAG | (255U << 20);
    memcpy(regs + DART_T8110_ERROR, &error, 4);
    assert(apple_dart_dcp_irq(&d) == IRQ_HANDLED);
    assert(writes == 1); /* Invalid SID must not cause an undefined BIT shift. */
    writes = 0; error = 0; memcpy(regs + DART_T8110_ERROR, &error, 4);
    assert(apple_dart_dcp_irq(&d) == IRQ_NONE && writes == 0);
    return 0;
}
'''
    lifetime = common + r'''
struct apple_dart { bool retained; };
static unsigned released, metadata_freed;
static void kfree(void *p) { metadata_freed++; free(p); }
static void release_payload(void *p, resource_size_t size) {
    assert(size == 4096); released++; free(p);
}
'''
    lifetime += definition(dart, 'struct apple_dart_retained_resource {') + ';\n'
    lifetime += definition(dart, 'static void apple_dart_release_retained_resource(') + '\n'
    lifetime += r'''
int main(void) {
    struct apple_dart d = {0};
    struct apple_dart_retained_resource *r = malloc(sizeof(*r));
    *r = (struct apple_dart_retained_resource){&d, release_payload, malloc(4096), 4096};
    apple_dart_release_retained_resource(r);
    assert(released == 1 && metadata_freed == 1);
    d.retained = true;
    r = malloc(sizeof(*r));
    void *backing = malloc(4096);
    *r = (struct apple_dart_retained_resource){&d, release_payload, backing, 4096};
    apple_dart_release_retained_resource(r);
    assert(released == 1 && metadata_freed == 2);
    memset(backing, 0x5a, 4096); /* Hardware-visible allocation still exists. */
    free(backing); /* Test process cleanup; driver does not release this. */
    return 0;
}
'''
    events = common + r'''
struct dma_fence { int error; unsigned signals; };
struct drm_pending_vblank_event { struct { struct dma_fence *fence; } base; unsigned deliveries; };
struct drm_device { int event_lock; };
struct drm_crtc { struct drm_device *dev; };
struct apple_crtc { struct drm_crtc base; struct drm_pending_vblank_event *event; };
struct drm_crtc_state { struct drm_pending_vblank_event *event; };
#define spin_lock_irqsave(p, flags) do { (flags)=0; assert(!*(p)); *(p)=1; } while (0)
#define spin_unlock_irqrestore(p, flags) do { (void)(flags); assert(*(p)); *(p)=0; } while (0)
static void dma_fence_set_error(struct dma_fence *f, int error) { f->error = error; }
static void drm_crtc_send_vblank_event(struct drm_crtc *c, struct drm_pending_vblank_event *e) {
    assert(c->dev->event_lock);
    assert(++e->deliveries == 1);
    if (e->base.fence) assert(++e->base.fence->signals == 1);
}
'''
    events += definition(dcp, 'static void dcp_failed_page_flip(') + '\n'
    events += definition(dcp, 'void dcp_drm_crtc_error(') + '\n'
    events += r'''
int main(void) {
    struct drm_device dev = {0};
    struct dma_fence f1 = {0}, f2 = {0};
    struct drm_pending_vblank_event e1 = {.base.fence = &f1}, e2 = {.base.fence = &f2};
    struct apple_crtc c = {.base.dev = &dev, .event = &e1};
    struct drm_crtc_state s = {.event = &e2};
    dcp_drm_crtc_error(&c, &s, -EIO);
    assert(!c.event && !s.event && !dev.event_lock);
    assert(e1.deliveries == 1 && e2.deliveries == 1);
    assert(f1.error == -EIO && f2.error == -EIO);
    struct drm_pending_vblank_event e3 = {0};
    c.event = &e3; s.event = &e3;
    dcp_drm_crtc_error(&c, &s, -EIO);
    assert(e3.deliveries == 1 && !c.event && !s.event);
    dcp_drm_crtc_error(&c, NULL, -EIO);
    return 0;
}
'''
    iommu = (root / 'drivers/iommu/of_iommu.c').read_text()
    parser_unit = common + r"""
typedef u32 __be32;
typedef u64 phys_addr_t;
struct device_node { int cells; bool fail_translation; };
struct device { struct device_node *of_node; };
struct resource { u64 start; };
struct list_head { unsigned count; };
struct iommu_resv_region { struct list_head list; };
enum iommu_resv_type { IOMMU_RESV_DIRECT, IOMMU_RESV_TRANSLATED };
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IOMMU_CACHE 4
#define GFP_KERNEL 0
#define dev_warn(...) do {} while (0)
#define dev_dbg(...) do {} while (0)
#define __free(type) __attribute__((cleanup(free_##type)))
static struct device_node own = {4, false}, foreign = {6, true};
static unsigned translations, node_puts;
static u32 be32_to_cpup(const __be32 *p) { return *p; }
static struct device_node *of_find_node_by_phandle(u32 id) {
    return id == 1 ? &own : id == 2 ? &foreign : NULL;
}
static int of_dma_region_cells(struct device_node *n) { return n->cells; }
static void of_node_put(struct device_node *n) { assert(n); node_puts++; }
static void free_device_node(struct device_node **n) { if (*n) of_node_put(*n); }
static const __be32 *of_translate_dma_region_checked(struct device_node *n, const __be32 *p,
        size_t cells, phys_addr_t *iova, size_t *length) {
    assert(cells == (size_t)n->cells);
    translations++;
    if (n->fail_translation) return NULL;
    *iova = 0x100000; *length = 0x4000;
    return p + n->cells;
}
static bool of_dma_is_coherent(struct device_node *n) { (void)n; return false; }
static enum iommu_resv_type iommu_resv_region_get_type(struct device *d,
        struct resource *r, phys_addr_t iova, size_t len) {
    (void)d; (void)r; assert(iova == 0x100000 && len == 0x4000);
    return IOMMU_RESV_DIRECT;
}
static struct iommu_resv_region reservation;
static struct iommu_resv_region *iommu_alloc_resv_region(phys_addr_t iova,
        size_t len, int prot, enum iommu_resv_type type, int flags) {
    (void)prot; (void)type; (void)flags;
    assert(iova == 0x100000 && len == 0x4000); return &reservation;
}
static struct iommu_resv_region *iommu_alloc_resv_region_tr(u64 phys,
        phys_addr_t iova, size_t len, int prot, enum iommu_resv_type type, int flags) {
    (void)phys; return iommu_alloc_resv_region(iova, len, prot, type, flags);
}
static void list_add_tail(struct list_head *item, struct list_head *head) {
    (void)item; head->count++;
}
static unsigned parse(const __be32 *maps, size_t cells) {
    const __be32 *end = maps + cells;
    struct device d = {.of_node = &own}, *dev = &d;
    struct resource phys = {0};
    struct list_head regions = {0}, *list = &regions;
"""
    parser_unit += definition(iommu, '\t\twhile (maps < end) {')
    parser_unit += r"""
    return regions.count;
}
int main(void) {
    const __be32 mixed[] = {2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    assert(parse(mixed, 12) == 1 && translations == 1 && node_puts == 2);
    const __be32 truncated[] = {1, 0, 0, 0};
    translations = node_puts = 0;
    assert(parse(truncated, 4) == 0 && translations == 0 && node_puts == 1);
    own.fail_translation = true;
    const __be32 fail_then_foreign[] = {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0};
    translations = node_puts = 0;
    assert(parse(fail_then_foreign, 12) == 0 && translations == 1 && node_puts == 2);
    const __be32 invalid[] = {0, 0, 0, 0, 0};
    translations = node_puts = 0;
    assert(parse(invalid, 5) == 0 && translations == 0 && node_puts == 0);
    foreign.cells = -EINVAL;
    const __be32 bad_owner[] = {2, 0, 0, 0, 0};
    assert(parse(bad_owner, 5) == 0 && translations == 0);
    return 0;
}
"""
    report = (root / 'drivers/pmdomain/apple/pmp-report.c').read_text()
    readiness = common + r"""
#define EPROBE_DEFER 517
#define PMP_REPORT_READY 1
struct device_node { const char *compatible; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
struct apple_pmp_report_offsets { unsigned status; };
struct apple_pmp_report { unsigned char *base; const struct apple_pmp_report_offsets *offsets; };
static struct device_node j713={"apple,t8132-pmp-v2"}, j613={"apple,t8122-pmp-v2"};
static struct device_node m4_report={"apple,t8132-pmp-v2-report"}, m3_report={"apple,j613-25g83-pmp-report"};
static struct device_node *linked;
static struct platform_device provider;
static u64 words[3];
static const struct apple_pmp_report_offsets offsets={16};
static struct apple_pmp_report rep={(unsigned char *)words,&offsets};
static bool board_m4, present=true, lockable=true, bound=true;
static unsigned reads, drops, unlocks;
static bool of_machine_is_compatible(const char *s) { assert(!strcmp(s,"apple,j713")); return board_m4; }
static bool of_device_is_compatible(struct device_node *n,const char *s) { return n && !strcmp(n->compatible,s); }
static struct device_node *of_parse_phandle(struct device_node *n,const char *s,int i) {
    assert(n && !strcmp(s,"apple,pmp-report") && i==0); return linked;
}
static void of_node_put(struct device_node *n) { (void)n; }
static struct platform_device *of_find_device_by_node(struct device_node *n) { assert(n==linked);return present?&provider:NULL; }
static bool device_trylock(struct device *d) { assert(d==&provider.dev);return lockable; }
static bool device_is_bound(struct device *d) { assert(d==&provider.dev);return bound; }
static void device_unlock(struct device *d) { assert(d==&provider.dev);unlocks++; }
static void put_device(struct device *d) { assert(d==&provider.dev);drops++; }
static struct apple_pmp_report *platform_get_drvdata(struct platform_device *d) { assert(d==&provider);return &rep; }
#define readq_poll_timeout(addr,value,condition,delay,timeout) ({ \
    (void)(delay);assert((timeout)==5000000);reads++;memcpy(&(value),(addr),8); \
    (condition)?0:-ETIMEDOUT; })
"""
    readiness += definition(report, 'int apple_pmp_report_wait_supplier_ready(')
    readiness += r"""
int main(void) {
    struct device supplier={&j713};
    board_m4=true;linked=&m4_report;words[2]=1;
    assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==0);
    words[2]=3;assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==0);
    words[2]=0;assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==-ETIMEDOUT);
    supplier.of_node=&j613;board_m4=false;linked=&m3_report;words[2]=1;
    assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==0);
    words[2]=3;assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==-ETIMEDOUT);
    unsigned before=reads;
    linked=&m4_report;assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==-ENODEV);
    linked=NULL;assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==-ENODEV);
    supplier.of_node=&j713;board_m4=true;linked=&m4_report;present=false;
    assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==-EPROBE_DEFER);
    present=true;lockable=false;assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==-EPROBE_DEFER);
    lockable=true;bound=false;assert(apple_pmp_report_wait_supplier_ready(&supplier,5000)==-EPROBE_DEFER);
    assert(reads==before && drops==7 && unlocks==6);
    return 0;
}
"""
    tests = [('pmp-vote-isolation', votes), ('borrowed-dart-fault', irq),
             ('retained-dart-resource', lifetime), ('failed-flip-events', events),
             ('iommu-address-tuples', parser_unit), ('pmp-supplier-readiness', readiness)]
    print('TAP version 13', flush=True)
    print(f'1..{len(tests)}', flush=True)
    with tempfile.TemporaryDirectory(prefix='m4-display-error-paths-') as directory:
        for index, (name, source) in enumerate(tests, 1):
            c = Path(directory) / (name + '.c')
            binary = Path(directory) / name
            c.write_text(source)
            subprocess.run([args.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-variable', '-Wno-unused-function', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', str(c), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(f'ok {index} - {name}', flush=True)


if __name__ == '__main__':
    main()
