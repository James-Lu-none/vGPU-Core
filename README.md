# PCIe FPGA GPU Liux Driver

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

## Unified Virtual Memory with Zero-Copy Direct DMA via XDMA Scatter-Gather Descriptors

Originally, a fixed DMA buffer was allocated at probe time and mapped to user space via `mmap`. 
Now, the driver dynamically binds and pins user-space virtual memory on demand (`ioctl`) without pre-allocating contiguous physical RAM during probe:

- On probe: Allocates a 64KB coherent DMA buffer (`dev->desc_ring`) to hold up to 8192 XDMA Descriptors, and a 64KB array (`pinned_pages`) to hold `struct page *` pointers (supporting up to 32MB payload limit).
- On ioctl: Dynamically pins user-space pages based on `payload_size` using `get_user_pages_fast()`. Then establishes DMA mappings using `dma_map_sg()` (transparently supporting both IOMMU and Non-IOMMU hosts).
- XDMA Descriptor Chain: Converts the mapped scatterlist (`dev->sgl`) into an XDMA Hardware Descriptor Chain in `dev->desc_ring`, linking non-contiguous physical pages via `next_desc` pointers. The driver submits the first Descriptor's DMA address to the FPGA XDMA IP via MMIO, allowing XDMA to automatically traverse and stream all payload pages.

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
sudo insmod driver/vgpu_core.ko queue_mode=0
sudo ./tests/test_ioctl

sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1
sudo ./tests/test_ioctl

# test working queue & interrupt
sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1
sudo ./tests/test_interrupt
```

## 

```bash
user@bastion:~/workspace$ sudo lspci -vvv -nn -d 10ee:7021
00:10.0 Serial controller [0700]: Xilinx Corporation Device [10ee:7021] (prog-if 01 [16450])
        Subsystem: Xilinx Corporation Device [10ee:0007]
        Physical Slot: 16
        Control: I/O+ Mem+ BusMaster- SpecCycle- MemWINV- VGASnoop- ParErr- Stepping- SERR+ FastB2B- DisINTx-
        Status: Cap+ 66MHz- UDF- FastB2B- ParErr- DEVSEL=fast >TAbort- <TAbort- <MAbort- >SERR- <PERR- INTx-
        Interrupt: pin A routed to IRQ 11
        Region 0: Memory at fea50000 (32-bit, non-prefetchable) [size=64K]
        Region 1: Memory at fea60000 (32-bit, non-prefetchable) [size=64K]
        Capabilities: [40] Power Management version 3
                Flags: PMEClk- DSI- D1- D2- AuxCurrent=0mA PME(D0-,D1-,D2-,D3hot-,D3cold-)
                Status: D0 NoSoftRst+ PME-Enable- DSel=0 DScale=0 PME-
        Capabilities: [48] MSI: Enable- Count=1/1 Maskable- 64bit+
                Address: 0000000000000000  Data: 0000
        Capabilities: [60] Express (v2) Endpoint, MSI 00
                DevCap: MaxPayload 512 bytes, PhantFunc 0, Latency L0s <64ns, L1 unlimited
                        ExtTag+ AttnBtn- AttnInd- PwrInd- RBE+ FLReset- SlotPowerLimit 75W
                DevCtl: CorrErr- NonFatalErr- FatalErr- UnsupReq-
                        RlxdOrd+ ExtTag+ PhantFunc- AuxPwr- NoSnoop+
                        MaxPayload 256 bytes, MaxReadReq 512 bytes
                DevSta: CorrErr+ NonFatalErr- FatalErr- UnsupReq- AuxPwr- TransPend-
                LnkCap: Port #0, Speed 5GT/s, Width x1, ASPM L0s, Exit Latency L0s unlimited
                        ClockPM- Surprise- LLActRep- BwNot- ASPMOptComp-
                LnkCtl: ASPM Disabled; RCB 64 bytes, Disabled- CommClk+
                        ExtSynch- ClockPM- AutWidDis- BWInt- AutBWInt-
                LnkSta: Speed 5GT/s, Width x1
                        TrErr- Train- SlotClk+ DLActive- BWMgmt- ABWMgmt-
                DevCap2: Completion Timeout: Range B, TimeoutDis- NROPrPrP- LTR-
                         10BitTagComp- 10BitTagReq- OBFF Not Supported, ExtFmt- EETLPPrefix-
                         EmergencyPowerReduction Not Supported, EmergencyPowerReductionInit-
                         FRS- TPHComp- ExtTPHComp-
                         AtomicOpsCap: 32bit- 64bit- 128bitCAS-
                DevCtl2: Completion Timeout: 50us to 50ms, TimeoutDis- LTR- 10BitTagReq- OBFF Disabled,
                         AtomicOpsCtl: ReqEn-
                LnkSta2: Current De-emphasis Level: -6dB, EqualizationComplete- EqualizationPhase1-
                         EqualizationPhase2- EqualizationPhase3- LinkEqualizationRequest-
                         Retimer- 2Retimers- CrosslinkRes: unsupported
```