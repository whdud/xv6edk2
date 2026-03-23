#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "types.h"
#include "pci.h"

// Virtio PCI vendor/device IDs (legacy/transitional)
#define VIRTIO_PCI_VENDOR_ID       0x1AF4
#define VIRTIO_PCI_DEVICE_ID_BLK   0x1001  // legacy: 0x1000 + type - 1

// Virtio PCI legacy I/O port register offsets (BAR0)
#define VIRTIO_PCI_HOST_FEATURES   0x00  // 4 bytes, R
#define VIRTIO_PCI_GUEST_FEATURES  0x04  // 4 bytes, R/W
#define VIRTIO_PCI_QUEUE_PFN       0x08  // 4 bytes, R/W
#define VIRTIO_PCI_QUEUE_SIZE      0x0C  // 2 bytes, R
#define VIRTIO_PCI_QUEUE_SEL       0x0E  // 2 bytes, R/W
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10  // 2 bytes, R/W
#define VIRTIO_PCI_STATUS          0x12  // 1 byte, R/W
#define VIRTIO_PCI_ISR             0x13  // 1 byte, R

// Device status bits
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

// Virtqueue descriptor flags
#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2

// Virtio-blk request types
#define VIRTIO_BLK_T_IN            0  // read
#define VIRTIO_BLK_T_OUT           1  // write

// Virtio-blk status values
#define VIRTIO_BLK_S_OK            0
#define VIRTIO_BLK_S_IOERR         1
#define VIRTIO_BLK_S_UNSUPP        2

// Queue PFN shift (4096 byte pages)
#define VIRTIO_PCI_QUEUE_ADDR_SHIFT 12

// Max descriptors we support
#define VIRTIO_QUEUE_NUM_MAX       256

// Each I/O request uses 3 descriptors: header, data, status
#define DESCS_PER_REQ              3
#define MAX_INFLIGHT               (VIRTIO_QUEUE_NUM_MAX / DESCS_PER_REQ)

// Virtqueue descriptor (16 bytes)
struct virtq_desc {
  uint addr_lo;
  uint addr_hi;
  uint len;
  ushort flags;
  ushort next;
};

// Available ring (driver -> device)
struct virtq_avail {
  ushort flags;
  ushort idx;
  ushort ring[];
};

// Used ring element
struct virtq_used_elem {
  uint id;
  uint len;
};

// Used ring (device -> driver)
struct virtq_used {
  ushort flags;
  ushort idx;
  struct virtq_used_elem ring[];
};

// Virtio-blk request header (16 bytes)
struct virtio_blk_req_hdr {
  uint type;
  uint reserved;
  uint sector_lo;
  uint sector_hi;
};

// Per-request tracking
struct virtio_blk_req {
  struct virtio_blk_req_hdr hdr;
  uchar status;
  struct buf *b;
};

void virtio_blk_init(struct pci_dev *dev);

#endif
