#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/cpu.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/cpufeature.h>
#include <asm/io.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arquiteto");
MODULE_DESCRIPTION("Metatron VMM Foundation - Raw ASM Edition");
MODULE_VERSION("1.1");

#define IA32_VMX_BASIC_MSR          0x00000480
#define IA32_FEATURE_CONTROL_MSR    0x0000003A
#define FEATURE_CONTROL_LOCK_BIT        (1ULL << 0)
#define FEATURE_CONTROL_VMX_ENABLE      (1ULL << 2)
#define X86_CR4_VMXE_BIT                (1ULL << 13)

struct core_context {
    void *vmxon_region;
    u64 vmxon_phys;
    u64 original_cr4;
    bool is_virtualized;
};

static struct core_context *g_ctx = NULL;
static int g_nr_cpus = 0;

/* --- PRIMITIVAS RAW (IGNORANDO O LINKER DO KERNEL) --- */

static inline unsigned long read_cr4_raw(void) {
    unsigned long val;
    asm volatile("mov %%cr4, %0" : "=r"(val) : : "memory");
    return val;
}

static inline void write_cr4_raw(unsigned long val) {
    asm volatile("mov %0, %%cr4" : : "r"(val) : "memory");
}

static inline int vmx_on_raw(u64 phys_addr) {
    u8 error;
    // Tenta ativar o modo VMX. setna captura se houve erro no Carry Flag ou Zero Flag.
    asm volatile ("vmxon %1; setna %0" : "=q"(error) : "m"(phys_addr) : "cc", "memory");
    return error;
}

static inline void vmx_off_raw(void) {
    asm volatile ("vmxoff" : : : "cc", "memory");
}

/* --- RITO DE ASCENSÃO --- */
static void metatron_ascension(void *info) {
    int cpu = smp_processor_id();
    u64 feature_control, vmx_basic;
    u32 revision_id;

    rdmsrl(IA32_FEATURE_CONTROL_MSR, feature_control);
    if (!(feature_control & FEATURE_CONTROL_LOCK_BIT)) {
        wrmsrl(IA32_FEATURE_CONTROL_MSR, feature_control | FEATURE_CONTROL_VMX_ENABLE | FEATURE_CONTROL_LOCK_BIT);
    } else if (!(feature_control & FEATURE_CONTROL_VMX_ENABLE)) {
        pr_err("[-] Metatron: VMX bloqueado na CPU %d\n", cpu);
        return;
    }

    // Usando nossas primitivas raw para contornar o erro 'native_write_cr4'
    g_ctx[cpu].original_cr4 = read_cr4_raw();
    write_cr4_raw(g_ctx[cpu].original_cr4 | X86_CR4_VMXE_BIT);

    rdmsrl(IA32_VMX_BASIC_MSR, vmx_basic);
    revision_id = (u32)(vmx_basic & 0x7FFFFFFF);
    *(u32 *)g_ctx[cpu].vmxon_region = revision_id;

    if (vmx_on_raw(g_ctx[cpu].vmxon_phys) == 0) {
        g_ctx[cpu].is_virtualized = true;
        pr_info("[+] Metatron: CPU %d entrou no Ring -1\n", cpu);
    } else {
        write_cr4_raw(g_ctx[cpu].original_cr4);
        pr_err("[-] Metatron: VMXON falhou na CPU %d\n", cpu);
    }
}

static void metatron_descension(void *info) {
    int cpu = smp_processor_id();
    if (g_ctx[cpu].is_virtualized) {
        vmx_off_raw();
        write_cr4_raw(g_ctx[cpu].original_cr4);
        g_ctx[cpu].is_virtualized = false;
    }
}

static int __init metatron_init(void) {
    int i;
    if (!boot_cpu_has(X86_FEATURE_VMX)) return -ENODEV;

    cpus_read_lock();
    g_nr_cpus = num_online_cpus();
    g_ctx = kzalloc(sizeof(*g_ctx) * g_nr_cpus, GFP_KERNEL);
    if (!g_ctx) { cpus_read_unlock(); return -ENOMEM; }

    for_each_online_cpu(i) {
        g_ctx[i].vmxon_region = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 0);
        if (g_ctx[i].vmxon_region) g_ctx[i].vmxon_phys = virt_to_phys(g_ctx[i].vmxon_region);
    }

    on_each_cpu(metatron_ascension, NULL, 1);
    cpus_read_unlock();
    return 0;
}

static void __exit metatron_exit(void) {
    int i;
    cpus_read_lock();
    on_each_cpu(metatron_descension, NULL, 1);
    cpus_read_unlock();
    for (i = 0; i < g_nr_cpus; i++) 
        if (g_ctx[i].vmxon_region) free_pages((unsigned long)g_ctx[i].vmxon_region, 0);
    kfree(g_ctx);
}

module_init(metatron_init);
module_exit(metatron_exit);
