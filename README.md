# vGPU-Core: Virtual PCIe GPU/Accelerator Linux Driver

## 運作模式

- **[Data Path]** User space 透過 `mmap()` 向驅動程式請求記憶體映射，驅動分配一塊 DMA Buffer 並直接映射給 User space
- **[Data Path]** User space 將要運算的資料直接寫入該映射位址
- **[Control Path]** 驅動程式在記憶體中維護 Ring Buffer，並將其實體位址告知 GPU 硬體
- **[Control Path]** User space 透過 `ioctl` 將運算指令寫入 Ring Buffer
- **[Control Path]** 驅動程式更新 Ring Buffer 的 Tail Pointer，並寫入 GPU 的 Doorbell 暫存器通知硬體
- **[Hardware]** GPU 的 Command Processor 讀取 Ring Buffer 裡的指令
- **[Hardware]** GPU 根據指令，透過 PCIe DMA 直接讀取 Data Buffer 進行運算，並將結果直接寫回 Data Buffer
- **[Hardware]** GPU 運算完畢，發出硬體中斷 (IRQ / MSI-X)
- **[Synchronization]** Kernel Driver 攔截中斷，喚醒在 Wait Queue 中等待的 User space 執行緒
- **[Result]** User space 被喚醒後從 mmap 的位址讀取運算結果

## 核心架構與功能 (Core Features & Architecture)

- **Basic Scaffolding & Char Device**: 完成 LKM 註冊、`open`/`release` 機制與基礎 Context 隔離。
- **IOCTL Command Submission & Doorbell**: 實作基礎指令協定，透過 `ioctl` (`copy_from_user`) 傳遞任務，完成單一 Ring Buffer 邏輯與 Doorbell 觸發。
- **Multi-Context Isolation & Scheduling**: 實作私有 Ring Buffer 與 Lock-free SPSC，並導入 Virtual Hardware Scheduler 處理多 Context 排程。
- **Zero-Copy MMAP & Virtual Bus**: 將記憶體存取升級為 `mmap`。實作純軟體 Memory Mapping，並嘗試註冊虛擬 Platform Device 以支援正規 DMA API 。
- **Hardware Simulation & Benchmarking**: 整合 Workqueue 模擬非同步運算，利用 Wait Queue 模擬中斷喚醒，並撰寫 User Space Benchmark 收集效能數據。

## build, install and check

```bash
make clean
make

# global queue + spin_lock vs private queue per context
sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=0 dma_mode=0
sudo ./tests/test_ioctl

sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1 dma_mode=0
sudo ./tests/test_ioctl

# remap_pfn_range vs dma
sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1 dma_mode=0
sudo ./tests/test_mmap

sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1 dma_mode=1
sudo ./tests/test_mmap

# test working queue & interrupt
sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1 dma_mode=1
sudo ./tests/test_interrupt
```