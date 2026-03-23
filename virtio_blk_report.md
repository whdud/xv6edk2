# xv6 Virtio-blk 드라이버 구현 보고서

## 1. 개요

### 1.1 배경
xv6 운영체제는 교육용으로 설계된 간단한 Unix 계열 OS로, 기본적으로 IDE 하드디스크 드라이버(`ide.c`)를 통해 디스크 I/O를 수행한다. 그러나 UEFI 부팅 환경에서는 기존 IDE 컨트롤러 접근 방식이 제한되어, `memide.c`를 사용한 메모리 기반 파일시스템(`kernelmemfs`)으로 우회하고 있었다. 이는 디스크에 실제로 쓰기가 불가능하며, 모든 파일시스템 데이터가 커널 바이너리에 내장되는 한계가 있다.

### 1.2 목표
QEMU의 virtio-blk 가상 디스크 장치를 통해 xv6에서 실제 디스크 I/O를 수행할 수 있는 드라이버를 구현한다.

### 1.3 개발 환경
| 항목 | 내용 |
|------|------|
| Host OS | Windows 11 Pro (WSL2 Ubuntu 24.04) |
| 에뮬레이터 | QEMU 8.x (qemu-system-x86_64) |
| 펌웨어 | OVMF (UEFI) |
| 대상 OS | xv6 (32-bit, i386) |
| 컴파일러 | GCC (-m32 -Werror) |
| virtio 모드 | Legacy (Transitional) PCI Interface |

---

## 2. Virtio 아키텍처 개요

### 2.1 Virtio란?
Virtio는 가상화 환경에서 게스트 OS와 하이퍼바이저 간 효율적인 I/O를 위한 표준 인터페이스이다. 하드웨어를 완전히 에뮬레이션하는 대신, 게스트가 가상화 환경임을 인지하고 최적화된 프로토콜로 통신한다.

### 2.2 Legacy PCI Interface
본 구현은 virtio legacy (0.9.5) PCI 인터페이스를 사용한다. BAR0에 매핑된 I/O 포트를 통해 디바이스와 통신하며, 주요 레지스터는 다음과 같다:

| 오프셋 | 크기 | 레지스터 | 설명 |
|--------|------|----------|------|
| 0x00 | 4B | HOST_FEATURES | 디바이스가 지원하는 기능 |
| 0x04 | 4B | GUEST_FEATURES | 드라이버가 수락한 기능 |
| 0x08 | 4B | QUEUE_PFN | Virtqueue 물리 페이지 번호 |
| 0x0C | 2B | QUEUE_SIZE | 큐의 디스크립터 수 (읽기 전용) |
| 0x0E | 2B | QUEUE_SEL | 큐 선택 |
| 0x10 | 2B | QUEUE_NOTIFY | 디바이스에 알림 |
| 0x12 | 1B | STATUS | 디바이스 상태 |
| 0x13 | 1B | ISR | 인터럽트 상태 |

### 2.3 Virtqueue 구조
Virtqueue는 드라이버와 디바이스 간 데이터를 주고받는 링 버퍼 구조로, 세 부분으로 구성된다:

```
+---------------------------+
| Descriptor Table          |  ← 각 16바이트, queue_size개
| (addr, len, flags, next)  |
+---------------------------+  ← 페이지 정렬
| Available Ring            |  ← 드라이버 → 디바이스
| (flags, idx, ring[])      |
+---------------------------+  ← 다음 페이지 경계로 정렬
| Used Ring                 |  ← 디바이스 → 드라이버
| (flags, idx, ring[])      |
+---------------------------+
```

**메모리 레이아웃 계산 (queue_size = 256 기준):**
- Descriptor Table: 256 * 16 = 4096 bytes
- Available Ring: 6 + 256 * 2 = 518 bytes
- desc + avail 합계: 4614 → 4096으로 페이지 정렬 → Used Ring 오프셋 = 8192
- Used Ring: 6 + 256 * 8 = 2054 bytes
- 총 필요 메모리: 8192 + 2054 ≈ 10246 bytes → 16384 bytes (4 pages) 할당

### 2.4 Virtio-blk 요청 구조
하나의 블록 I/O 요청은 3개의 디스크립터 체인으로 구성된다:

```
Descriptor 0 (Header)     → Descriptor 1 (Data)      → Descriptor 2 (Status)
┌─────────────────────┐    ┌──────────────────────┐    ┌──────────────────┐
│ type (IN/OUT)       │    │ 512B 데이터 버퍼      │    │ 1B 상태 코드     │
│ reserved            │    │ (읽기: 디바이스 쓰기)  │    │ (디바이스 쓰기)  │
│ sector (LBA)        │    │ (쓰기: 디바이스 읽기)  │    │ 0=OK, 1=Error   │
│ flags: NEXT         │    │ flags: WRITE|NEXT     │    │ flags: WRITE     │
└─────────────────────┘    └──────────────────────┘    └──────────────────┘
  디바이스가 읽음              방향에 따라 다름             디바이스가 씀
```

---

## 3. 구현 상세

### 3.1 파일 구성

| 파일 | 설명 | 변경 유형 |
|------|------|----------|
| `virtio_blk.h` | 레지스터 정의, 구조체 선언 | 신규 |
| `virtio_blk.c` | 드라이버 본체 (초기화, I/O, 인터럽트) | 신규 |
| `pci.c` | PCI 디바이스 탐색 시 virtio-blk 감지 추가 | 수정 |
| `trap.c` | IRQ 11 핸들러에 ideintr() 호출 추가 | 수정 |
| `main.c` | pci_init()을 ideinit() 앞으로 이동 | 수정 |
| `Makefile` | kernelvirtio 빌드 타겟 추가 | 수정 |
| `run_virtio.sh` | QEMU 실행 스크립트 | 신규 |

### 3.2 디바이스 초기화 과정 (`virtio_blk_init`)

Virtio 스펙에 정의된 초기화 시퀀스를 따른다:

```
1. Reset          → STATUS = 0
2. Acknowledge    → STATUS |= ACKNOWLEDGE (1)
3. Driver         → STATUS |= DRIVER (2)
4. Feature 협상   → GUEST_FEATURES = 0 (추가 기능 미사용)
5. Virtqueue 설정 → 큐 선택, 크기 읽기, 메모리 배치, PFN 전달
6. Driver OK      → STATUS |= DRIVER_OK (4)
7. IOAPIC 설정    → Level-triggered, Active-low 인터럽트
```

### 3.3 PCI 디바이스 감지

기존 PCI 스캔 코드(`pci.c`)에 virtio-blk 감지 로직을 추가하였다:

```c
// pci.c - pci_init_device() 내부
if(vendor_id == 0x1AF4 && device_id == 0x1001){
    cprintf("Virtio Block Device Found\n");
    virtio_blk_init(&dev);
}
```

- Vendor ID `0x1AF4`: Red Hat (virtio 표준)
- Device ID `0x1001`: virtio-blk legacy (0x1000 + device_type)

### 3.4 I/O 경로

xv6의 기존 디스크 I/O 인터페이스(`ideinit`, `iderw`, `ideintr`)를 그대로 유지하여, 상위 계층(bio.c, fs.c, log.c)의 수정 없이 드라이버를 교체하였다.

```
bio.c: bread()/bwrite()
    ↓
virtio_blk.c: iderw(buf)
    ↓
virtio_blk_submit(buf)     ← 디스크립터 체인 구성, Available Ring에 추가, Notify
    ↓
Polling loop               ← Used Ring 확인, 완료 시 buf 플래그 갱신
```

### 3.5 QEMU 실행 구성

```bash
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
    -drive if=ide,file=fat:rw:image,index=0,media=disk \
    -device virtio-blk-pci,drive=hd1,disable-modern=on \
    -drive id=hd1,file=xv6/fs.img,format=raw,if=none \
    -m 2048 -smp 4 -serial mon:stdio -vga std
```

- `disable-modern=on`: Modern (1.0+) 인터페이스를 비활성화하여 Legacy 모드 강제
- `fs.img`: xv6의 파일시스템 이미지를 virtio-blk 디스크로 연결
- IDE 드라이브: UEFI 부트로더 + 커널 이미지 (FAT 파티션)

---

## 4. 트러블슈팅

### 4.1 문제 1: "Guest says index 257 is available" 오류

**증상:** QEMU가 `qemu: Guest says index 257 is available` 에러를 출력하며 I/O 실패.

**원인:** 초기 구현에서 8KB 정적 버퍼(`vq_mem[8192]`)를 사용했으나, 이 버퍼의 가상 주소를 `V2P()` 매크로로 변환한 물리 주소가 연속적이지 않을 수 있었다. 또한 queue_size를 32로 임의로 제한하면서 디바이스가 기대하는 것과 다른 메모리 레이아웃이 만들어졌다.

**해결:** 고정 물리 주소(0x800000, 8MB 지점)를 사용하도록 변경. 이 주소는 커널보다 위에 있고 RAM 범위 내에 있으며, 물리적으로 연속된 것이 보장된다.

```c
uint phys_base = 0x800000;
char *base = P2V(phys_base);
memset(base, 0, 16384);
```

### 4.2 문제 2: 인터럽트 미전달 (Used Ring 갱신 안 됨)

**증상:** 요청 제출 후 `used->idx`가 0에서 변하지 않음. ISR 레지스터도 0.

**원인:** xv6의 `ioapicenable()` 함수는 Edge-triggered 모드로 인터럽트를 설정한다. 그러나 PCI 디바이스의 인터럽트는 Level-triggered, Active-low여야 한다.

**해결:** IOAPIC 리다이렉션 엔트리를 직접 작성하여 Level-triggered + Active-low로 설정:

```c
volatile uint *ioapic_reg = (volatile uint *)0xFEC00000;
// Low 32비트: 벡터 + level-triggered(bit 15) + active-low(bit 13)
ioapic_reg[0] = 0x10 + 2 * virtio_irq;
ioapic_reg[4] = (T_IRQ0 + virtio_irq) | 0x00008000 | 0x00002000;
// High 32비트: CPU 0으로 라우팅
ioapic_reg[0] = 0x10 + 2 * virtio_irq + 1;
ioapic_reg[4] = 0 << 24;
```

### 4.3 문제 3: 연속 물리 페이지 할당 실패

**증상:** `kalloc()`으로 여러 페이지를 할당받아 연속성을 확인하려 했으나, free list가 단편화되어 연속 페이지를 찾지 못함. `panic("virtio-blk: cannot find contiguous pages")` 발생.

**원인:** xv6의 `kalloc()`은 단일 페이지 할당기로, 연속된 물리 페이지 할당을 보장하지 않는다.

**해결:** `kalloc()`을 사용하지 않고, 고정 물리 주소 0x800000을 직접 사용. 이 영역은 커널 로드 영역(~0x19C000) 위에 있어 충돌하지 않으며, `kinit2()`에서 free list에 등록되기 전에 사용하므로 안전하다.

### 4.4 문제 4: Queue Size 불일치 (핵심 버그)

**증상:** 인터럽트 설정, 물리 주소 문제를 모두 해결한 후에도 `used->idx`가 여전히 0. 디바이스가 요청을 전혀 처리하지 않음.

**디버깅 과정:**
1. QEMU 모니터에서 `info qtree` 명령으로 디바이스 상태 확인
2. 디바이스의 `vring.num = 256` 확인 — 디바이스는 256개 디스크립터 기준으로 동작

**원인:** 드라이버의 `VIRTIO_QUEUE_NUM_MAX`가 128로 정의되어 있었다. 디바이스는 queue_size=256을 보고하지만 드라이버가 128로 캡을 걸었다. 문제는 **디바이스는 항상 자신의 queue_size(256)를 기준으로 메모리 레이아웃을 계산**한다는 점이다.

```
드라이버 기준 (queue_size=128):          디바이스 기준 (queue_size=256):
Desc:   0 ~ 2047   (128*16)            Desc:   0 ~ 4095   (256*16)
Avail:  2048 ~ 2309                     Avail:  4096 ~ 4613
Used:   4096 ~     (페이지 정렬)         Used:   8192 ~     (페이지 정렬)
```

드라이버가 Available Ring을 오프셋 2048에 쓰지만, 디바이스는 오프셋 4096에서 Available Ring을 읽으려 한다. 서로 다른 위치를 보고 있으므로 디바이스는 새 요청을 감지하지 못한다.

**해결:** `VIRTIO_QUEUE_NUM_MAX`를 256으로 변경하고, 메모리 할당을 16384 bytes (4 pages)로 확대:

```c
// virtio_blk.h
#define VIRTIO_QUEUE_NUM_MAX  256  // 기존: 128

// virtio_blk.c
memset(base, 0, 16384);  // 기존: 8192
```

### 4.5 문제 5: IRQ 11 공유

**증상:** virtio-blk(IRQ 11)과 E1000 NIC(IRQ 11)이 같은 인터럽트 라인을 공유.

**해결:** `trap.c`의 IRQ 11 핸들러에서 두 드라이버의 인터럽트 핸들러를 모두 호출. `ideintr()`는 내부에서 ISR 레지스터를 확인하여 자신에게 해당하지 않는 인터럽트를 필터링한다:

```c
case T_IRQ0 + 0xB:
    i8254_intr();     // E1000 NIC
    ideintr();        // virtio-blk (ISR 체크로 spurious 필터링)
    lapiceoi();
    break;
```

### 4.6 문제 요약 및 해결 타임라인

| 순서 | 문제 | 근본 원인 | 해결 방법 |
|------|------|----------|----------|
| 1 | index 257 오류 | 비연속 물리 메모리 | 고정 물리 주소 사용 |
| 2 | 인터럽트 미전달 | Edge-triggered 설정 | IOAPIC 직접 설정 (Level-triggered) |
| 3 | 연속 페이지 할당 실패 | kalloc 단편화 | 고정 물리 주소 0x800000 |
| 4 | **디바이스 무응답** | **Queue size 불일치 (128 vs 256)** | **VIRTIO_QUEUE_NUM_MAX=256** |
| 5 | IRQ 공유 충돌 | E1000과 IRQ 11 공유 | 두 핸들러 동시 호출 + ISR 필터링 |

---

## 5. 최종 결과

### 5.1 부팅 로그 (디버그 출력 제거 후)

```
PCI Device Found Bus:0x0 Device:0x4 Function:0
  Device ID:0x1001  Vendor ID:0x1af4
  Base Class:0x1  Sub Class:0x0  Interface:0x0  Revision ID:0x0
Virtio Block Device Found
virtio-blk: initialized
cpu0: starting 0
sb: size 1000 nblocks 941 ninodes 200 nlog 30 logstart 2 inodestart 32 bmap start 58
init: starting sh
$
```

### 5.2 동작 확인

- xv6 셸 정상 기동
- `ls` 명령으로 파일시스템 전체 내용 읽기 성공
- 프로그램 실행 (cat, echo, hello 등) 정상 동작
- 파일시스템 superblock 정상 파싱 (size=1000, nblocks=941, ninodes=200)

### 5.3 빌드 방법

```bash
# virtio-blk 커널 빌드
cd /mnt/c/Users/whdud/xv6edk2/xv6
make kernelvirtio

# image 폴더에 복사 후 실행
cp kernelvirtio ../image/kernel
cd .. && bash run_virtio.sh
```

기존 `kernelmemfs` 빌드는 그대로 유지되어 영향을 받지 않는다:
```bash
make kernelmemfs   # 기존 메모리 기반 파일시스템 (변경 없음)
```

---

## 6. 결론

xv6에 virtio-blk 드라이버를 구현하여 UEFI 부팅 환경에서 실제 디스크 I/O를 가능하게 하였다. 핵심 난관은 virtqueue 메모리 레이아웃에서 드라이버와 디바이스 간의 queue size 불일치였으며, QEMU 모니터(`info qtree`)를 활용한 디버깅으로 근본 원인을 파악할 수 있었다.

이 구현은 기존 xv6의 블록 디바이스 인터페이스(`ideinit`, `iderw`, `ideintr`)를 그대로 유지하여 파일시스템 계층(bio.c, fs.c, log.c)의 수정 없이 투명하게 동작한다.

### 향후 개선 가능 사항
- Polling 방식에서 인터럽트 기반 Sleep/Wakeup 방식으로 전환하여 CPU 사용률 개선
- 여러 요청의 동시 제출(batching)을 통한 I/O 처리량 향상
- Virtio Modern (1.0+) 인터페이스 지원
