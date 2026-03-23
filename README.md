# xv6 for booting with UEFI (EDK2)

## Virtio-blk 드라이버 구현

xv6 UEFI 부팅 환경에서 기존 메모리 기반 파일시스템(memide)의 한계를 해결하기 위해 **virtio-blk 디스크 드라이버**를 구현하였습니다.

### 기존 문제점
- `kernelmemfs`는 fs.img를 커널 바이너리에 내장하여 메모리에서만 동작
- 파일 생성/수정 후 재부팅하면 모든 변경사항 소실
- 실제 디스크 I/O 불가

### 구현 내용
- QEMU virtio-blk-pci 디바이스를 통한 실제 디스크 읽기/쓰기
- Virtio Legacy PCI Interface (BAR0 I/O 포트 방식)
- 기존 xv6 블록 디바이스 인터페이스(`iderw`, `ideintr`) 유지 → 상위 계층 수정 없이 동작
- 재부팅 후에도 파일 영구 보존 확인

### 추가/수정된 파일
| 파일 | 설명 |
|------|------|
| `xv6/virtio_blk.h` | Virtio 레지스터 정의, Virtqueue 구조체 |
| `xv6/virtio_blk.c` | 드라이버 본체 (초기화, I/O 제출, 인터럽트 처리) |
| `xv6/pci.c` | PCI 스캔 시 virtio-blk 디바이스 감지 추가 |
| `xv6/trap.c` | IRQ 11 핸들러에 virtio-blk 인터럽트 추가 |
| `xv6/main.c` | 초기화 순서 조정 (pci_init → ideinit) |
| `xv6/Makefile` | `kernelvirtio` 빌드 타겟 추가 |
| `run_virtio.sh` | QEMU 실행 스크립트 |

### 빌드 및 실행 (WSL)

```bash
# virtio-blk 커널 빌드
cd xv6
make kernelvirtio

# 실행
cp kernelvirtio ../image/kernel
cd ..
bash run_virtio.sh
```

기존 `kernelmemfs` 빌드는 영향 없이 그대로 유지됩니다:
```bash
make kernelmemfs
```

### 트러블슈팅 요약

| 문제 | 원인 | 해결 |
|------|------|------|
| QEMU "index 257" 오류 | 비연속 물리 메모리 | 고정 물리 주소 0x800000 사용 |
| 인터럽트 미전달 | IOAPIC Edge-triggered 설정 | Level-triggered + Active-low 직접 설정 |
| kalloc 연속 페이지 실패 | xv6 단일 페이지 할당기 한계 | 고정 물리 주소로 우회 |
| **디바이스 무응답 (핵심)** | **Queue size 불일치 (128 vs 256)** | **VIRTIO_QUEUE_NUM_MAX=256으로 수정** |
| IRQ 11 공유 | E1000 NIC과 동일 IRQ | 두 핸들러 동시 호출 + ISR 필터링 |

상세 보고서: [`virtio_blk_report.md`](virtio_blk_report.md)
