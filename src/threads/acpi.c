/*
 * Minimal support for reading basic ACPI tables.
 *
 * Based on Advanced Configuration and Power Interface (ACPI) Specification
 * Version 6.3 January 2019
 *
 * Written by Godmar Back <godmar@gmail.com>
 */

#include "threads/acpi.h"
#include "threads/vaddr.h"
#include "threads/pte.h"
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/cpu.h"
#include "devices/serial.h"
#include "devices/ioapic.h"
#include "devices/lapic.h"
#include "lib/kernel/x86.h"
#include "lib/round.h"
#include <stdio.h>
#include <string.h>
#include "lib/debug.h"

/*
 * The tables are located outside the memory that the BIOS
 * reports as usable, so they are not already mapped in the
 * kernel-virtual address space.
 *
 * We map them temporarily at the following two addresses.
 * Adding a mapping makes physical address paddr accessible 
 * at ACPI_TEMPORARY_ADDRESS
 */
#define ACPI_TEMPORARY_ADDRESS  (void *)0xA0000000
/* Chosen such that it uses a different PDE entry */
#define ACPI_TEMPORARY_ADDRESS2  (ACPI_TEMPORARY_ADDRESS + PTSPAN)
static void * map_acpi_table (void *vaddr, uintptr_t paddr);
static void unmap_acpi_table (void *vaddr);

/* printf/putchar is not operational yet, output directly to serial console. */
static void
output_string(char *m)
{
  for (; *m; m++)
    serial_putc (*m);
}

/*
 * ACPI Data Structures
 * 
 * [5.2.5.3] Root System Description Pointer (RSDP) Structure
 */
struct RSDP 
  {
    char Signature[8];      /* "RSD PTR " */
    uint8_t Checksum;
    char OEMID[6];          /* OEM-Supplied identified. */ 
    uint8_t Revision;       /* 0 for ACPI 1.0, 2 for later */
    uint32_t RsdtAddress;   /* Physical address of RSDT */
  } __attribute__ ((packed));

/* 
 * [5.2.5.1] Finding the RSDP on IA-PC Systems.
 * Scans for "RSD PTR " signature and correct checksum,
 * as per ACPI 1.0.  Returns the 32-bit physical address.
 */
static uint32_t
find_rsdp (uint32_t base, uint32_t len)
{
  void *addr = ptov (base);
  void *end = addr + len;
  for (void *start = addr; start < end; start += 16)
    if (memcmp(start, "RSD PTR ", 8) == 0)
      {
        uint8_t sum = 0;
        for (uint8_t *u = start; u < (uint8_t *)start + 20; u++)
          sum += *u;
        
        if (sum == 0)
          {
            struct RSDP *rsdp = start;
            return rsdp->RsdtAddress;
          }
      }
  return 0;
}

/*
 * All ACPI Description Tables share the same header.
 */
struct DTHeader 
  {
    char Signature[4];    /* 4 byte signature */
    uint32_t Length;      /* Length, in bytes, of the entire descriptor table. */
    uint8_t Revision;
    uint8_t Checksum;
    char OEMID[6];
    char OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
  } __attribute__ ((packed));

/*
 * [5.2.7] Root System Description Table (RSDT)
 */
struct RSDT 
  {
    struct DTHeader Header;   /* Signature is "RSDT" */
    /* header.Length is the length, in bytes, of the entire RSDT.  The length 
     * implies the number of Entry fields (n) at the end of the table. */
    uint32_t Entry[0];
  } __attribute__ ((packed));

/*
 * [5.2.12] Multiple APIC Description Table (MADT)
 * Contains per-processor information and IO APIC info.
 */
struct MADT 
  {
    struct DTHeader Header;   /* Signature is "APIC" */
    uint32_t LocalInterruptControllerAddress; /* LAPIC base address */
    uint32_t Flags;
    char Entry[0];
  } __attribute__ ((packed));

/* Interrupt Control Structure Type.
 * Shared header for entries in the MADT for processors, IOAPIC, etc. */
struct ICST 
  {
    uint8_t type;
    uint8_t length;
    char  body[0];
  } __attribute__ ((packed));

/* Table 5-46 Processor Local APIC Structure */
struct ProcessorLocalAPIC
  {
    struct ICST header;
    uint8_t ACPIProcessorUID;
    uint8_t ApicID;
    uint32_t Flags;   /* see PROCESSOR_LOCAL_APIC_FLAG_* */
  } __attribute__ ((packed));

#define PROCESSOR_LOCAL_APIC_FLAG_ENABLED        0x1
#define PROCESSOR_LOCAL_APIC_FLAG_ONLINE_CAPABLE 0x2

/* Table 5-48. I/O APIC Structure */
struct IOAPIC 
  {
    struct ICST header;
    uint8_t IOApicId;   /* The I/O APIC's ID. */
    uint8_t reserved;
    uint32_t IOApicAddress;   /* The 32-bit physical address to access this 
                      I/O APIC. Each I/O APIC resides at a unique address. */
    uint32_t GlobalSystemInterruptBase; /* The global system interrupt number 
                      where this I/O APIC’s interrupt inputs start. */
  } __attribute__ ((packed));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

/*
 * Scan through RSDT and look for a table named `name`
 * Caller must call unmap_page () on return value.
 */
static struct DTHeader *
find_table_in_rsdt(uint32_t rsdtAddress, const char *name)
{
  struct RSDT *rsdt = map_acpi_table (ACPI_TEMPORARY_ADDRESS, rsdtAddress);

  struct DTHeader *dth = NULL;
  for (uint32_t *entry = rsdt->Entry; 
       (void *)entry <= (void *)rsdt + rsdt->Header.Length; 
       entry++)
    {
      dth = map_acpi_table (ACPI_TEMPORARY_ADDRESS2, *entry);
      if (!strncmp (dth->Signature, name, 4))
        break;

      unmap_acpi_table (dth);
    }
  unmap_acpi_table (rsdt);
  return dth;
}
#pragma GCC diagnostic pop

void
acpi_init (void)
{
  uint32_t rsdt = 0;

  /* [5.2.5.1] Finding the RSDP on IA-PC Systems.
   * The RSDP may be in two places:
   * 1) The first 1 KB of the Extended BIOS Data Area (EBDA). For EISA or MCA 
   * systems, the EBDA can be found in the two-byte location 40:0Eh on the 
   * BIOS data area. */
  uint8_t *bda = (uint8_t *) ptov (0x400);
  uint32_t ebda = ((bda[0x0F] << 8) | bda[0x0E]) << 4;
  if (ebda != 0)
    rsdt = find_rsdp (ebda, 1024);     /* First KB of the EBDA */
 
  /* 2) The BIOS read-only memory space between 0E0000h and 0FFFFFh. */
  if (rsdt == 0) 
    rsdt = find_rsdp (0xE0000, 0x20000);

  struct MADT *madt = NULL;
  struct IOAPIC *ioapic = NULL;
  if (rsdt == 0)
    goto out;

  madt = (struct MADT *) find_table_in_rsdt (rsdt, "APIC");
  if (madt == NULL)
    goto out;

  lapic_base_addr = (void *)madt->LocalInterruptControllerAddress;
  if (lapic_base_addr == NULL)
    goto out;

  /* ACPI guarantees that the Bootstrap Processor is listed first. */
  bcpu = &cpus[0];      /* Set boot CPU. */

  void *madt_end = (void *)madt + madt->Header.Length;
  for (void *entry = madt->Entry; (void *)entry < madt_end; )
    {
      struct ICST *icst = entry;
      switch (icst->type)
        {
          case 0:       /* Process Local APIC */
            struct ProcessorLocalAPIC *p = entry;
            if (p->Flags & PROCESSOR_LOCAL_APIC_FLAG_ENABLED)
              {
                memset(&cpus[ncpu], 0, sizeof(cpus[0]));
                cpus[ncpu].id = p->ApicID;
                ncpu++;
              }
            break;

          case 1:       /* I/O APIC */
            ioapic = entry;
            ioapic_set_base_address ((void *) ioapic->IOApicAddress);
            ioapic_set_id (ioapic->IOApicId);
            break;

          default:
            /* We are not interested in any other types at the moment. */
            break;
        }
      entry += icst->length;
    }
out:
  if (madt != NULL)
    unmap_acpi_table (madt);

  if (rsdt == 0)
    output_string ("Could not find rsdt\n");

  if (madt == NULL)
    output_string ("Could not find madt\n");

  if (ioapic == NULL)
    output_string ("Could not find I/O APIC\n");

  if (ncpu == 0)
    output_string ("Did not find any CPUs\n");
}

/* Helper function to create a single page mapping in the initial
 * page directory, allocating a page table as necessary. */
static void
map_page(void *vaddr, void *paddr)
{
  uint32_t *pd = init_page_dir, *pt;
  size_t pde_idx = pd_no (vaddr);
  size_t pte_idx = pt_no (vaddr);
  if (pd[pde_idx] == 0)
    {
      pt = palloc_get_page (PAL_ASSERT | PAL_ZERO);
      pd[pde_idx] = pde_create_kernel (pt);
    }
  else
    {
      pt = pde_get_pt(pd[pde_idx]);
    }
  uint32_t mapping = pte_create_kernel_identity (paddr, true);
  if (pt[pte_idx] == 0)
    pt[pte_idx] = mapping;
}

/* Map memory for an ACPI table, which may comprise multiple pages. */
static void *
map_acpi_table (void *vaddr, uintptr_t paddr)
{
  uint32_t pg_offset = paddr & (PGSIZE - 1);
  ASSERT (vaddr < (void *)PCI_ADDR_ZONE_BEGIN);
 
  paddr -= pg_offset; 

  /* Map first page to access header. */
  map_page (vaddr, (void *) paddr);
  void *raddr = vaddr + pg_offset;
  struct DTHeader *dt = raddr;

  /* Map additional pages as needed for the table. */
  void *end = (void *) ROUND_UP ((uintptr_t) (raddr + dt->Length), PGSIZE);
  void *next = (void *) ROUND_UP ((uintptr_t) raddr, PGSIZE);
  for (void *p = next; p < end; p += PGSIZE)
    {
      paddr += PGSIZE;
      map_page (p, (void *) paddr);
    }
  return raddr;
}

/* Unmap previously mapped page and deallocate page table. */
static void
unmap_acpi_table (void *vaddr)
{
  vaddr = ((void *) ((uintptr_t) vaddr & ~(PGSIZE-1)));

  /* This function unmaps the entire 4MB area corresponding to the PDE. */
  uint32_t *pd = init_page_dir;
  size_t pde_idx = pd_no (vaddr);
  uint32_t *pt = pde_get_pt (pd[pde_idx]);
  palloc_free_page (pt);
  pd[pde_idx] = 0;
  lcr3 (vtop (init_page_dir));    /* Flush TLB */
}
