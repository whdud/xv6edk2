#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "pci.h"
#include "virtio_blk.h"

// 32-bit port I/O (not in x86.h)
static inline uint
inl(ushort port)
{
  uint data;
  asm volatile("inl %1,%0" : "=a"(data) : "d"(port));
  return data;
}

static inline void
outl(ushort port, uint data)
{
  asm volatile("outl %0,%1" : : "a"(data), "d"(port));
}

static inline ushort
inw(ushort port)
{
  ushort data;
  asm volatile("inw %1,%0" : "=a"(data) : "d"(port));
  return data;
}

// Driver state
static ushort iobase;
static uchar virtio_irq;
static uint queue_size;
static int virtio_found = 0;

// Virtqueue pointers
static struct virtq_desc  *vq_desc;
static struct virtq_avail *vq_avail;
static struct virtq_used  *vq_used;

// Per-request tracking
static struct virtio_blk_req reqs[MAX_INFLIGHT];

// Free descriptor tracking (use 256 to match max device queue)
static int desc_free[VIRTIO_QUEUE_NUM_MAX];
static int nfree_desc;

// Synchronization
static struct spinlock virtio_lock;

// Last processed used ring index
static ushort last_used_idx;

// ---- Descriptor management ----

static int
alloc_descs(int n)
{
  int i, j;
  for(i = 0; i <= (int)queue_size - n; i++){
    int ok = 1;
    for(j = 0; j < n; j++){
      if(!desc_free[i+j]){
        ok = 0;
        break;
      }
    }
    if(ok){
      for(j = 0; j < n; j++)
        desc_free[i+j] = 0;
      nfree_desc -= n;
      return i;
    }
  }
  return -1;
}

static void
free_descs(int idx, int n)
{
  int j;
  for(j = 0; j < n; j++){
    desc_free[idx+j] = 1;
    memset(&vq_desc[idx+j], 0, sizeof(struct virtq_desc));
  }
  nfree_desc += n;
}

// ---- Device initialization ----

void
virtio_blk_init(struct pci_dev *pdev)
{
  uint cmd_reg, data;

  initlock(&virtio_lock, "virtio_blk");

  // Enable PCI bus mastering and I/O space access
  pci_access_config(pdev->bus_num, pdev->device_num, pdev->function_num, 0x04, &cmd_reg);
  cmd_reg |= PCI_CMD_BUS_MASTER | 0x01;
  pci_write_config_register(pdev->bus_num, pdev->device_num, pdev->function_num, 0x04, cmd_reg);

  // Extract I/O port base from BAR0 (bit 0 = I/O space indicator)
  iobase = (ushort)(pdev->bar0 & ~0x3);

  // Read IRQ from PCI config
  pci_access_config(pdev->bus_num, pdev->device_num, pdev->function_num, 0x3C, &data);
  virtio_irq = data & 0xFF;

  // 1. Reset device
  outb(iobase + VIRTIO_PCI_STATUS, 0);

  // 2. Acknowledge
  outb(iobase + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

  // 3. Driver loaded
  outb(iobase + VIRTIO_PCI_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

  // 4. Feature negotiation - accept no special features
  outl(iobase + VIRTIO_PCI_GUEST_FEATURES, 0);

  // 5. Set up virtqueue 0 (the request queue)
  outw(iobase + VIRTIO_PCI_QUEUE_SEL, 0);
  queue_size = inw(iobase + VIRTIO_PCI_QUEUE_SIZE);
  if(queue_size == 0)
    panic("virtio-blk: queue size is 0");
  if(queue_size > VIRTIO_QUEUE_NUM_MAX)
    queue_size = VIRTIO_QUEUE_NUM_MAX;

  // 6. Use fixed physical address for virtqueue (guaranteed contiguous)
  // Physical 0x800000 (8MB) - well within RAM, above kernel, page-aligned
  uint desc_size = 16 * queue_size;
  uint avail_size = 6 + 2 * queue_size;
  uint used_offset = (desc_size + avail_size + 4095) & ~4095u;

  uint phys_base = 0x800000;
  char *base = P2V(phys_base);
  memset(base, 0, 16384);  // 4 pages for up to 256 descriptors

  // Set up pointers
  vq_desc = (struct virtq_desc *)base;
  vq_avail = (struct virtq_avail *)(base + desc_size);
  vq_used = (struct virtq_used *)(base + used_offset);

  // Init free descriptor list
  uint i;
  for(i = 0; i < queue_size; i++)
    desc_free[i] = 1;
  nfree_desc = queue_size;

  vq_avail->flags = 0;
  vq_avail->idx = 0;
  last_used_idx = 0;

  // 7. Tell device where virtqueue is (PFN)
  uint pfn = phys_base >> VIRTIO_PCI_QUEUE_ADDR_SHIFT;
  outl(iobase + VIRTIO_PCI_QUEUE_PFN, pfn);

  // 8. Mark driver ready
  outb(iobase + VIRTIO_PCI_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

  // 9. Enable interrupt on IOAPIC (level-triggered, active-low for PCI)
  // ioapicenable sets edge-triggered which doesn't work for PCI interrupts.
  // We write the IOAPIC redirection entry directly.
  {
    volatile uint *ioapic_reg = (volatile uint *)0xFEC00000;
    // Write low 32 bits: vector, level-triggered, active-low, enabled
    ioapic_reg[0] = 0x10 + 2 * virtio_irq;       // select low register
    ioapic_reg[4] = (T_IRQ0 + virtio_irq) | 0x00008000 | 0x00002000;  // level + active-low
    // Write high 32 bits: route to CPU 0
    ioapic_reg[0] = 0x10 + 2 * virtio_irq + 1;   // select high register
    ioapic_reg[4] = 0 << 24;                      // CPU 0
  }

  virtio_found = 1;
  cprintf("virtio-blk: initialized\n");
}

// ---- Submit a request ----

static void
virtio_blk_submit(struct buf *b)
{
  int idx = alloc_descs(DESCS_PER_REQ);
  if(idx < 0)
    panic("virtio-blk: out of descriptors");

  int req_idx = idx / DESCS_PER_REQ;
  struct virtio_blk_req *req = &reqs[req_idx];

  // Set up request header
  req->hdr.type = (b->flags & B_DIRTY) ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  req->hdr.reserved = 0;
  req->hdr.sector_lo = b->blockno * (BSIZE / 512);
  req->hdr.sector_hi = 0;
  req->status = 0xFF;
  req->b = b;

  // Descriptor 0: header (device reads)
  vq_desc[idx].addr_lo = V2P(&req->hdr);
  vq_desc[idx].addr_hi = 0;
  vq_desc[idx].len = sizeof(struct virtio_blk_req_hdr);
  vq_desc[idx].flags = VIRTQ_DESC_F_NEXT;
  vq_desc[idx].next = idx + 1;

  // Descriptor 1: data buffer
  vq_desc[idx+1].addr_lo = V2P(b->data);
  vq_desc[idx+1].addr_hi = 0;
  vq_desc[idx+1].len = BSIZE;
  if(b->flags & B_DIRTY){
    // Write: device reads data
    vq_desc[idx+1].flags = VIRTQ_DESC_F_NEXT;
  } else {
    // Read: device writes data
    vq_desc[idx+1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
  }
  vq_desc[idx+1].next = idx + 2;

  // Descriptor 2: status byte (device writes)
  vq_desc[idx+2].addr_lo = V2P(&req->status);
  vq_desc[idx+2].addr_hi = 0;
  vq_desc[idx+2].len = 1;
  vq_desc[idx+2].flags = VIRTQ_DESC_F_WRITE;
  vq_desc[idx+2].next = 0;

  // Add to available ring
  __sync_synchronize();
  vq_avail->ring[vq_avail->idx % queue_size] = idx;
  __sync_synchronize();
  vq_avail->idx++;
  __sync_synchronize();

  // Notify device
  outw(iobase + VIRTIO_PCI_QUEUE_NOTIFY, 0);
}

// ---- Interrupt handler ----

void
ideintr(void)
{
  if(!virtio_found)
    return;

  acquire(&virtio_lock);

  // Read ISR status to acknowledge interrupt
  inb(iobase + VIRTIO_PCI_ISR);

  // Process completed requests
  __sync_synchronize();
  while(last_used_idx != vq_used->idx){
    __sync_synchronize();
    uint desc_idx = vq_used->ring[last_used_idx % queue_size].id;

    int req_idx = desc_idx / DESCS_PER_REQ;
    struct virtio_blk_req *req = &reqs[req_idx];
    struct buf *b = req->b;

    if(req->status != VIRTIO_BLK_S_OK){
      cprintf("virtio-blk: I/O error, status=%d\n", req->status);
    }

    b->flags |= B_VALID;
    b->flags &= ~B_DIRTY;

    free_descs(desc_idx, DESCS_PER_REQ);
    wakeup(b);

    last_used_idx++;
  }

  release(&virtio_lock);
}

// ---- ideinit: called from main.c ----

void
ideinit(void)
{
  // Actual initialization happens in virtio_blk_init() called from pci_init_device()
  // which runs after this. Nothing to do here.
}

// ---- iderw: called from bio.c ----

void
iderw(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("iderw: buf not locked");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");

  if(!virtio_found)
    panic("iderw: virtio-blk not initialized");

  acquire(&virtio_lock);

  virtio_blk_submit(b);

  // Poll for completion
  while((b->flags & (B_VALID|B_DIRTY)) != B_VALID){
    __sync_synchronize();
    if(last_used_idx != ((volatile struct virtq_used *)vq_used)->idx){
      __sync_synchronize();
      uint desc_idx = vq_used->ring[last_used_idx % queue_size].id;
      int req_idx = desc_idx / DESCS_PER_REQ;
      struct virtio_blk_req *req = &reqs[req_idx];
      struct buf *done = req->b;

      done->flags |= B_VALID;
      done->flags &= ~B_DIRTY;
      free_descs(desc_idx, DESCS_PER_REQ);
      last_used_idx++;

      if(done != b)
        wakeup(done);
    }
  }

  release(&virtio_lock);
}
