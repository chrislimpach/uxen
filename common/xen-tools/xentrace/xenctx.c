/******************************************************************************
 * tools/xentrace/xenctx.c
 *
 * Tool for dumping the cpu context
 *
 * Copyright (C) 2005 by Intel Corp
 *
 * Author: Arun Sharma <arun.sharma@intel.com>
 * Date:   February 2005
 */
/*
 * uXen changes:
 *
 * Copyright 2012-2016, Bromium, Inc.
 * Author: Julian Pidancet <julian@pidancet.net>
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <err.h>
#include <time.h>
#include <stdlib.h>
//#include <sys/mman.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <string.h>
#include <inttypes.h>
#include <getopt.h>

#include "xenctrl.h"
#include <xen/foreign/x86_32.h>
#include <xen/foreign/x86_64.h>
#include <xen/hvm/save.h>

#include <uuid/uuid.h>
#include <uxenctllib.h>
#include <uxen_ioctl.h>
#include <vm-savefile-simple.h>

#if defined(_WIN32)
DECLARE_PROGNAME;
#endif	/* _WIN32 */

static struct xenctx {
    uint8_t uuid[16];
    xc_interface *xc_handle;
    int domid;
    int frame_ptrs;
    int stack_trace;
    int disp_all;
    int all_vcpus;
    int self_paused;
    int save;
    xc_dominfo_t dominfo;
} xenctx;

typedef unsigned long long guest_word_t;
#define FMT_32B_WORD "%08"PRIx64
#define FMT_64B_WORD "%016"PRIx64
/* Word-length of the guest's own data structures */
int guest_word_size = sizeof (unsigned long);
/* Word-length of the context record we get from xen */
int ctxt_word_size = sizeof (unsigned long);
int guest_protected_mode = 1;

struct symbol {
    guest_word_t address;
    char *name;
    struct symbol *next;
} *symbol_table = NULL;

guest_word_t kernel_stext, kernel_etext, kernel_sinittext, kernel_einittext, kernel_hypercallpage;

enum guest_os {
    GUEST_LINUX = 0,
    GUEST_WINDOWS,
};

static enum guest_os guest_os = GUEST_WINDOWS;

static uint64_t kernel_start_os[][2] = {
    /* linux */ { 0xc0000000, 0xffffffff80000000UL },
    /* windows */ { 0x80000000, 0xfffff80000000000UL }
};

static char *savefile = NULL;

static int kernel_start_set = 0;
static unsigned long long kernel_start = 0;

static int is_kernel_text(guest_word_t addr)
{
    if (symbol_table == NULL) {
        if (kernel_start_set)
            return (addr > kernel_start);
        return (addr > kernel_start_os[guest_os][guest_word_size == 4 ? 0 : 1]);
    }

    if (addr >= kernel_stext &&
        addr <= kernel_etext)
        return 1;
    if (addr >= kernel_hypercallpage &&
        addr <= kernel_hypercallpage + 4096)
        return 1;
    if (addr >= kernel_sinittext &&
        addr <= kernel_einittext)
        return 1;
    return 0;
}

#if 0
static void free_symbol(struct symbol *symbol)
{
    if (symbol == NULL)
        return;
    if (symbol->name)
        free(symbol->name);
    free(symbol);
}
#endif

static void insert_symbol(struct symbol *symbol)
{
    static struct symbol *prev = NULL;
    struct symbol *s = symbol_table;

    if (s == NULL) {
        symbol_table = symbol;
        symbol->next = NULL;
        return;
    }

    /* The System.map is usually already sorted... */
    if (prev
        && prev->address <= symbol->address
        && (!prev->next || prev->next->address > symbol->address)) {
        s = prev;
    } else {
        /* ... otherwise do crappy/slow search for the correct place */
        while (s->next && s->next->address <= symbol->address)
            s = s->next;
    }

    symbol->next = s->next;
    s->next = symbol;
    prev = symbol;
}

static struct symbol *lookup_symbol(guest_word_t address)
{
    struct symbol *s = symbol_table;

    if (!s)
        return NULL;

    while (s->next && s->next->address < address)
        s = s->next;

    return s->next && s->next->address <= address ? s->next : s;
}

static void print_symbol(guest_word_t addr)
{
    struct symbol *s;

    if (!is_kernel_text(addr))
        return;

    s = lookup_symbol(addr);

    if (s==NULL)
        return;

    if (addr==s->address)
        printf("%s ", s->name);
    else
        printf("%s+%#x ", s->name, (unsigned int)(addr - s->address));
}

static void read_symbol_table(const char *symtab)
{
    char type, line[256];
    char *p;
    struct symbol *symbol;
    FILE *f;

    f = fopen(symtab, "r");
    if(f == NULL) {
        fprintf(stderr, "failed to open symbol table %s\n", symtab);
        exit(-1);
    }

    while(!feof(f)) {
        if(fgets(line,256,f)==NULL)
            break;

        symbol = malloc(sizeof(*symbol));

        /* need more checks for syntax here... */
        symbol->address = strtoull(line, &p, 16);
        if (!isspace((uint8_t)*p++))
            continue;
        type = *p++;
        if (!isalpha((uint8_t)type) && type != '?')
            continue;
        if (!isspace((uint8_t)*p++))
            continue;

        /* in the future we should handle the module name
         * being appended here, this would allow us to use
         * /proc/kallsyms as our symbol table
         */
        if (p[strlen(p)-1] == '\n')
            p[strlen(p)-1] = '\0';
        symbol->name = strdup(p);

        switch (type) {
        case 'A': /* global absolute */
        case 'a': /* local absolute */
            break;
        case 'U': /* undefined */
        case 'v': /* undefined weak object */
        case 'w': /* undefined weak function */
            continue;
        default:
            insert_symbol(symbol);
            break;
        }

        if (strcmp(symbol->name, "_stext") == 0)
            kernel_stext = symbol->address;
        else if (strcmp(symbol->name, "_etext") == 0)
            kernel_etext = symbol->address;
        else if (strcmp(symbol->name, "_sinittext") == 0)
            kernel_sinittext = symbol->address;
        else if (strcmp(symbol->name, "_einittext") == 0)
            kernel_einittext = symbol->address;
        else if (strcmp(symbol->name, "hypercall_page") == 0)
            kernel_hypercallpage = symbol->address;
    }

    fclose(f);
}

#define CR0_PE  0x1
char *flag_values[22][2] =
{/*  clear,     set,       bit# */
    { NULL,     "c"    }, // 0        Carry
    { NULL,     NULL   }, // 1
    { NULL,     "p"    }, // 2        Parity
    { NULL,     NULL   }, // 3
    { NULL,     "a"    }, // 4        Adjust
    { NULL,     NULL   }, // 5
    { "nz",     "z"    }, // 6        Zero
    { NULL,     "s"    }, // 7        Sign
    { NULL,     "tf"   }, // 8        Trap
    { NULL,     "i"    }, // 9        Interrupt (enabled)
    { NULL,     "d=b"  }, // 10       Direction
    { NULL,     "o"    }, // 11       Overflow
    { NULL,     NULL   }, // 12       12+13 == IOPL
    { NULL,     NULL   }, // 13
    { NULL,     "nt"   }, // 14       Nested Task
    { NULL,     NULL   }, // 15
    { NULL,     "rf"   }, // 16       Resume Flag
    { NULL,     "v86"  }, // 17       Virtual 8086 mode
    { NULL,     "ac"   }, // 18       Alignment Check (enabled)
    { NULL,     "vif"  }, // 19       Virtual Interrupt (enabled)
    { NULL,     "vip"  }, // 20       Virtual Interrupt Pending
    { NULL,     "cid"  }  // 21       Cpuid Identification Flag
};

static void print_flags(uint64_t flags)
{
    int i;

    printf("\nflags: %08" PRIx64, flags);
    for (i = 21; i >= 0; i--) {
        char *s = flag_values[i][(flags >> i) & 1];
        if (s != NULL)
            printf(" %s", s);
    }
    printf("\n");
}

static void print_special(void *regs, const char *name, unsigned int mask, int width)
{
    unsigned int i;

    printf("\n");
    for (i = 0; mask; mask >>= 1, ++i)
        if (mask & 1) {
            if (width == 4)
                printf("%s%u: %08"PRIx32"\n", name, i, ((uint32_t *) regs)[i]);
            else
                printf("%s%u: %016"PRIx64"\n", name, i, ((uint64_t *) regs)[i]);
        }
}

static void print_ctx_32(vcpu_guest_context_x86_32_t *ctx)
{
    struct cpu_user_regs_x86_32 *regs = &ctx->user_regs;

    printf("cs:eip: %04x:%08x ", regs->cs, regs->eip);
    print_symbol(regs->eip);
    print_flags(regs->eflags);
    printf("ss:esp: %04x:%08x\n", regs->ss, regs->esp);

    printf("eax: %08x\t", regs->eax);
    printf("ebx: %08x\t", regs->ebx);
    printf("ecx: %08x\t", regs->ecx);
    printf("edx: %08x\n", regs->edx);

    printf("esi: %08x\t", regs->esi);
    printf("edi: %08x\t", regs->edi);
    printf("ebp: %08x\n", regs->ebp);

    printf(" ds:     %04x\t", regs->ds);
    printf(" es:     %04x\t", regs->es);
    printf(" fs:     %04x\t", regs->fs);
    printf(" gs:     %04x\n", regs->gs);

    if (xenctx.disp_all) {
        print_special(ctx->ctrlreg, "cr", 0x1d, 4);
        print_special(ctx->debugreg, "dr", 0xcf, 4);
    }
}

static void print_ctx_32on64(vcpu_guest_context_x86_64_t *ctx)
{
    struct cpu_user_regs_x86_64 *regs = &ctx->user_regs;

    printf("cs:eip: %04x:%08x ", regs->cs, (uint32_t)regs->eip);
    print_symbol((uint32_t)regs->eip);
    print_flags((uint32_t)regs->eflags);
    printf("ss:esp: %04x:%08x\n", regs->ss, (uint32_t)regs->esp);

    printf("eax: %08x\t", (uint32_t)regs->eax);
    printf("ebx: %08x\t", (uint32_t)regs->ebx);
    printf("ecx: %08x\t", (uint32_t)regs->ecx);
    printf("edx: %08x\n", (uint32_t)regs->edx);

    printf("esi: %08x\t", (uint32_t)regs->esi);
    printf("edi: %08x\t", (uint32_t)regs->edi);
    printf("ebp: %08x\n", (uint32_t)regs->ebp);

    printf(" ds:     %04x\t", regs->ds);
    printf(" es:     %04x\t", regs->es);
    printf(" fs:     %04x\t", regs->fs);
    printf(" gs:     %04x\n", regs->gs);

    if (xenctx.disp_all) {
        print_special(ctx->ctrlreg, "cr", 0x1d, 4);
        print_special(ctx->debugreg, "dr", 0xcf, 4);
    }
}

static void print_ctx_64(vcpu_guest_context_x86_64_t *ctx)
{
    struct cpu_user_regs_x86_64 *regs = &ctx->user_regs;

    printf("rip: %016"PRIx64" ", regs->rip);
    print_symbol(regs->rip);
    print_flags(regs->rflags);
    printf("rsp: %016"PRIx64"\n", regs->rsp);

    printf("rax: %016"PRIx64"\t", regs->rax);
    printf("rcx: %016"PRIx64"\t", regs->rcx);
    printf("rdx: %016"PRIx64"\n", regs->rdx);

    printf("rbx: %016"PRIx64"\t", regs->rbx);
    printf("rsi: %016"PRIx64"\t", regs->rsi);
    printf("rdi: %016"PRIx64"\n", regs->rdi);

    printf("rbp: %016"PRIx64"\t", regs->rbp);
    printf(" r8: %016"PRIx64"\t", regs->r8);
    printf(" r9: %016"PRIx64"\n", regs->r9);

    printf("r10: %016"PRIx64"\t", regs->r10);
    printf("r11: %016"PRIx64"\t", regs->r11);
    printf("r12: %016"PRIx64"\n", regs->r12);

    printf("r13: %016"PRIx64"\t", regs->r13);
    printf("r14: %016"PRIx64"\t", regs->r14);
    printf("r15: %016"PRIx64"\n", regs->r15);

    printf(" cs: %04x\t", regs->cs);
    printf(" ss: %04x\t", regs->ss);
    printf(" ds: %04x\t", regs->ds);
    printf(" es: %04x\n", regs->es);

    printf(" fs: %04x @ %016"PRIx64"\n", regs->fs, ctx->fs_base);
    printf(" gs: %04x @ %016"PRIx64"/%016"PRIx64"\n", regs->gs,
           ctx->gs_base_kernel, ctx->gs_base_user);

    if (xenctx.disp_all) {
        print_special(ctx->ctrlreg, "cr", 0x1d, 8);
        print_special(ctx->debugreg, "dr", 0xcf, 8);
    }
}

static void print_ctx(vcpu_guest_context_any_t *ctx)
{
    if (ctxt_word_size == 4) 
        print_ctx_32(&ctx->x32);
    else if (guest_word_size == 4)
        print_ctx_32on64(&ctx->x64);
    else 
        print_ctx_64(&ctx->x64);
}

#define NONPROT_MODE_SEGMENT_SHIFT 4

static guest_word_t instr_pointer(vcpu_guest_context_any_t *ctx)
{
    guest_word_t r;

    r = ctx->x64.user_regs.rip;

    if ( !guest_protected_mode )
        r += ctx->x64.user_regs.cs << NONPROT_MODE_SEGMENT_SHIFT;

    return r;
}

static guest_word_t stack_pointer(vcpu_guest_context_any_t *ctx)
{
    guest_word_t r;

    r = ctx->x64.user_regs.rsp;

    if ( !guest_protected_mode )
        r += ctx->x64.user_regs.ss << NONPROT_MODE_SEGMENT_SHIFT;

    return r;
}

static guest_word_t frame_pointer(vcpu_guest_context_any_t *ctx)
{
    guest_word_t r;

    r = ctx->x64.user_regs.rbp;

    if ( !guest_protected_mode )
        r += ctx->x64.user_regs.ss << NONPROT_MODE_SEGMENT_SHIFT;

    return r;
}

static void *map_page(vcpu_guest_context_any_t *ctx, int vcpu, guest_word_t virt)
{
    static unsigned long previous_mfn = 0;
    static void *mapped = NULL;
    unsigned long mfn;
    unsigned long offset;

    if (!guest_protected_mode)
        mfn = virt >> XC_PAGE_SHIFT;
    else
        mfn = xc_translate_foreign_address(xenctx.xc_handle, xenctx.domid, vcpu, virt);

    offset = virt & ~XC_PAGE_MASK;

    if (mapped && mfn == previous_mfn)
        goto out;

    if (mapped)
        xc_munmap(xenctx.xc_handle, xenctx.domid, mapped, XC_PAGE_SIZE);

    previous_mfn = mfn;

    mapped = xc_map_foreign_range(xenctx.xc_handle, xenctx.domid, XC_PAGE_SIZE, PROT_READ, mfn);

    if (mapped == NULL) {
        fprintf(stderr, "failed to map page.\n");
        return NULL;
    }

 out:
    return (void *)(mapped + offset);
}

static guest_word_t read_stack_word(guest_word_t *src, int width)
{
    guest_word_t word = 0;
    /* Little-endian only */
    memcpy(&word, src, width);
    return word;
}

static void print_stack_word(guest_word_t word, int width)
{
    if (width == 4)
        printf(FMT_32B_WORD, word);
    else
        printf(FMT_64B_WORD, word);
}

static int print_code(vcpu_guest_context_any_t *ctx, int vcpu)
{
    guest_word_t instr;
    int i;

    instr = instr_pointer(ctx);
    printf("Code (instr addr "FMT_64B_WORD")\n", instr);
    instr -= 21;
    for(i=0; i<32; i++) {
        unsigned char *c = map_page(ctx, vcpu, instr+i);
        if (!c)
            return -1;
        if (instr+i == instr_pointer(ctx))
            printf("<%02x> ", *c);
        else
            printf("%02x ", *c);
    }
    printf("\n");

    printf("\n");
    return 0;
}

static int print_stack(vcpu_guest_context_any_t *ctx, int vcpu, int width)
{
    guest_word_t stack = stack_pointer(ctx);
    guest_word_t stack_limit;
    guest_word_t frame;
    guest_word_t word;
    guest_word_t *p;
    int i;

    stack_limit = ((stack_pointer(ctx) + XC_PAGE_SIZE)
                   & ~((guest_word_t) XC_PAGE_SIZE - 1)); 
    printf("\n");
    printf("Stack:\n");
    for (i=1; i<5 && stack < stack_limit; i++) {
        while(stack < stack_limit && stack < stack_pointer(ctx) + i*32) {
            p = map_page(ctx, vcpu, stack);
            if (!p)
                return -1;
            word = read_stack_word(p, width);
            printf(" ");
            print_stack_word(word, width);
            stack += width;
        }
        printf("\n");
    }
    printf("\n");

    if(xenctx.stack_trace)
        printf("Stack Trace:\n");
    else
        printf("Call Trace:\n");
    printf("%c [<", xenctx.stack_trace ? '*' : ' ');
    print_stack_word(instr_pointer(ctx), width);
    printf(">] ");

    print_symbol(instr_pointer(ctx));
    printf(" <--\n");
    if (xenctx.frame_ptrs) {
        stack = stack_pointer(ctx);
        frame = frame_pointer(ctx);
        while(frame && stack < stack_limit) {
            if (xenctx.stack_trace) {
                while (stack < frame) {
                    p = map_page(ctx, vcpu, stack);
                    if (!p)
                        return -1;
                    printf("|   ");
                    print_stack_word(read_stack_word(p, width), width);
                    printf("   \n");
                    stack += width;
                }
            } else {
                stack = frame;
            }

            p = map_page(ctx, vcpu, stack);
            if (!p)
                return -1;
            frame = read_stack_word(p, width);
            if (xenctx.stack_trace) {
                printf("|-- ");
                print_stack_word(read_stack_word(p, width), width);
                printf("\n");
            }
            stack += width;

            if (frame) {
                p = map_page(ctx, vcpu, stack);
                if (!p)
                    return -1;
                word = read_stack_word(p, width);
                printf("%c [<", xenctx.stack_trace ? '|' : ' ');
                print_stack_word(word, width);
                printf(">] ");
                print_symbol(word);
                printf("\n");
                stack += width;
            }
        }
    } else {
        stack = stack_pointer(ctx);
        while(stack < stack_limit) {
            p = map_page(ctx, vcpu, stack);
            if (!p)
                return -1;
            word = read_stack_word(p, width);
            if (is_kernel_text(word)) {
                printf("  [<");
                print_stack_word(word, width);
                printf(">] ");
                print_symbol(word);
                printf("\n");
            } else if (xenctx.stack_trace) {
                printf("    ");
                print_stack_word(word, width);
                printf("\n");
            }
            stack += width;
        }
    }
    return 0;
}

static void dump_ctx(int vcpu)
{
    vcpu_guest_context_any_t ctx;

    if (xc_vcpu_getcontext(xenctx.xc_handle, xenctx.domid, vcpu, &ctx) < 0) {
        perror("xc_vcpu_getcontext");
        return;
    }

    {
        if (xenctx.dominfo.hvm) {
            struct hvm_hw_cpu cpuctx;
            xen_capabilities_info_t xen_caps = "";
            if (xc_domain_hvm_getcontext_partial(
                    xenctx.xc_handle, xenctx.domid, HVM_SAVE_CODE(CPU),
                    vcpu, &cpuctx, sizeof cpuctx) != 0) {
                perror("xc_domain_hvm_getcontext_partial");
                return;
            }
            guest_word_size = (cpuctx.msr_efer & 0x400) ? 8 : 4;
            guest_protected_mode = (cpuctx.cr0 & CR0_PE);
            /* HVM guest context records are always host-sized */
            if (xc_version(xenctx.xc_handle, XENVER_capabilities, &xen_caps) != 0) {
                perror("xc_version");
                return;
            }
            ctxt_word_size = (strstr(xen_caps, "xen-3.0-x86_64")) ? 8 : 4;
        } else {
            struct xen_domctl domctl;
            memset(&domctl, 0, sizeof domctl);
            domctl.domain = xenctx.domid;
            domctl.cmd = XEN_DOMCTL_get_address_size;
            if (xc_domctl(xenctx.xc_handle, &domctl) == 0)
                ctxt_word_size = guest_word_size = domctl.u.address_size.size / 8;
        }
    }

    printf("\nVCPU: %d\n", vcpu);
    print_ctx(&ctx);
    if (print_code(&ctx, vcpu))
        return;
    if (is_kernel_text(instr_pointer(&ctx)))
        if (print_stack(&ctx, vcpu, guest_word_size))
            return;
}

static void dump_all_vcpus(void)
{
    xc_vcpuinfo_t vinfo;
    int vcpu;
    for (vcpu = 0; vcpu <= xenctx.dominfo.max_vcpu_id; vcpu++)
    {
        if ( xc_vcpu_getinfo(xenctx.xc_handle, xenctx.domid, vcpu, &vinfo) )
            continue;
        if ( vinfo.online )
            dump_ctx(vcpu);
    }
}

static void usage(void)
{
    printf("usage:\n\n");

    printf("  xenctx [options] <DOMAIN> [VCPU]\n\n");

    printf("options:\n");
    printf("  -f, --frame-pointers\n");
    printf("                    assume the kernel was compiled with\n");
    printf("                    frame pointers.\n");
    printf("  -s SYMTAB, --symbol-table=SYMTAB\n");
    printf("                    read symbol table from SYMTAB.\n");
    printf("  -S --stack-trace  print a complete stack trace.\n");
    printf("  -k, --kernel-start\n");
    printf("                    set user/kernel split. (default per os)\n");
    printf("  -o, --os          set guest os. (default windows)\n");
    printf("  -a --all          display more registers\n");
    printf("  -C --all-vcpus    print info for all vcpus\n");

    printf("\n");
    printf("  xenctx [--trap vector [--error-code code] [--cr2 address]] <domain> <vcpu>\n");
}

int main(int argc, char **argv)
{
    int ch;
    int ret;
    static const char *sopts = "fs:hak:o:SCt:F:";
    static int long_index;
    enum { LI_ERROR_CODE, LI_CR2 };
    static const struct option lopts[] = {
        {"stack-trace", 0, NULL, 'S'},
        {"symbol-table", 1, NULL, 's'},
        {"frame-pointers", 0, NULL, 'f'},
        {"kernel-start", 1, NULL, 'k'},
        {"os", 1, NULL, 'o'},
        {"all", 0, NULL, 'a'},
        {"all-vcpus", 0, NULL, 'C'},
        {"help", 0, NULL, 'h'},
        {"trap", 1, NULL, 't'},
        {"error-code", 1, &long_index, LI_ERROR_CODE},
        {"cr2", 1, &long_index, LI_CR2},
        {"save", 1, NULL, 'F'},
        {0, 0, 0, 0}
    };
    const char *symbol_table = NULL;
    int trap_no = -1;
    int error_code = -1;
    uint64_t cr2 = 0;

    int vcpu = 0;

    while ((ch = getopt_long(argc, argv, sopts, lopts, NULL)) != -1) {
        switch(ch) {
        case 'f':
            xenctx.frame_ptrs = 1;
            break;
        case 's':
            symbol_table = optarg;
            break;
        case 'S':
            xenctx.stack_trace = 1;
            break;
        case 'a':
            xenctx.disp_all = 1;
            break;
        case 'C':
            xenctx.all_vcpus = 1;
            break;
        case 'k':
            kernel_start = strtoull(optarg, NULL, 0);
            kernel_start_set = 1;
            break;
        case 'o':
            if (!strcmp(optarg, "windows"))
                guest_os = GUEST_WINDOWS;
            else if (!strcmp(optarg, "linux"))
                guest_os = GUEST_LINUX;
            break;
        case 't':
            trap_no = atoi(optarg);
            break;
        case 'F':
            xenctx.save = 1;
            savefile = optarg;
            break;
        case 0:
            switch (long_index) {
            case LI_ERROR_CODE:
                error_code = atoi(optarg);
                break;
            case LI_CR2:
                cr2 = strtoull(optarg, NULL, 0);
                break;
            }
            break;
        case 'h':
            usage();
            exit(-1);
        case '?':
            fprintf(stderr, "%s --help for more options\n", argv[0]);
            exit(-1);
        }
    }

    argv += optind; argc -= optind;

    if (argc < 1 || argc > 2) {
        printf("usage: xenctx [options] <uuid> <optional vcpu>\n");
        exit(-1);
    }

    ret = uuid_parse(argv[0], xenctx.uuid);
    if (ret) {
            fprintf(stderr, "invalid uuid: %s\n", argv[0]);
            exit(-1);
    }

    if (argc == 2)
        vcpu = atoi(argv[1]);

    if (symbol_table)
        read_symbol_table(symbol_table);

    xenctx.xc_handle = xc_interface_open(0, 0, 0, NULL);
    if (xenctx.xc_handle == NULL) {
        fprintf(stderr, "error: xc_interface_open\n");
        exit(-1);
    }

    xenctx.domid = uxen_target_vm((UXEN_HANDLE_T)xc_interface_handle(xenctx.xc_handle),
                                  xenctx.uuid);
    if (xenctx.domid < 0) {
        perror("uxen_target_vm");
        exit(-1);
    }

    printf("xenctx: UUID %s, domid %d\n", argv[0], xenctx.domid);

    if (trap_no != -1) {
        ret = xc_hvm_inject_trap(xenctx.xc_handle, xenctx.domid, vcpu,
                                 trap_no, error_code, cr2);
        if (ret) {
            perror("xc_hvm_inject_trap");
            exit(-1);
        }
        goto finish;
    }

    ret = xc_domain_getinfo(xenctx.xc_handle, xenctx.domid, 1, &xenctx.dominfo);
    if (ret < 0) {
        perror("xc_domain_getinfo");
        exit(-1);
    }

    if (!xenctx.dominfo.paused) {
        ret = xc_domain_pause(xenctx.xc_handle, xenctx.domid);
        if (ret < 0) {
            perror("xc_domain_pause");
            exit(-1);
        }
        xenctx.self_paused = 1;
    }

    if (xenctx.save)
        vmsavefile_save_simple(xenctx.xc_handle, savefile, xenctx.uuid, xenctx.domid);

    if (xenctx.all_vcpus)
        dump_all_vcpus();
    else if (!xenctx.save)
        dump_ctx(vcpu);

    if (xenctx.self_paused) {
        ret = xc_domain_unpause(xenctx.xc_handle, xenctx.domid);
        if (ret < 0) {
            perror("xc_domain_unpause");
            exit(-1);
        }
    }

  finish:
    ret = xc_interface_close(xenctx.xc_handle);
    if (ret < 0) {
        perror("xc_interface_close");
        exit(-1);
    }
    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
