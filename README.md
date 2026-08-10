# vGPU-Core: Virtual PCIe GPU/Accelerator Linux Driver

## 專案簡介 (Introduction)
vGPU-Core 是一個基於 Linux Kernel 的虛擬 PCIe 加速器驅動程式。本專案旨在模擬現代 GPU / AI 加速器在矽前驗證 (Pre-silicon Validation) 階段的軟硬體互動模型。透過實作字元裝置介面、記憶體映射與非同步工作佇列，提供了一個無需實體硬體即可驗證 User-to-Kernel 溝通開銷與排程機制的測試平台。


## 實體 Linux 系統到 PCIe 的軟硬體架構 (overview)

- 第一層：User Space (使用者空間)
    - Application / Library： 比如一個 AI 推論程式，或者 CUDA 函式庫。
    - System Calls (系統呼叫)： 當程式需要硬體運算時，不能直接碰硬體。它必須透過 open(), ioctl() (傳遞自訂指令), 或 mmap() (直接映射記憶體) 敲響 Kernel 的大門。

- 第二層：Kernel Space (核心空間)
    - VFS (虛擬檔案系統)： Linux 哲學是「萬物皆檔案」。你的驅動程式會在這裡註冊一個裝置節點（例如 /dev/vaccel0）。
    - Device Driver (你的驅動程式)： 這是核心大腦。它負責接收 ioctl，驗證參數，然後把工作轉換成硬體看得懂的格式。
    - Memory Management & DMA： GPU 運算需要大量資料。驅動程式要在這裡分配一塊實體連續的記憶體（DMA Buffer），讓 CPU 和 PCIe 設備可以互相讀寫，而不用浪費 CPU 資源去 Copy 資料。
    - Interrupt Handler (中斷處理)： 當 PCIe 設備算完資料後，會發出一個硬體信號（IRQ/MSI）。你的驅動程式必須註冊一個對應的函數來「接」這個信號，然後喚醒在 User Space 等待的程式。

- 第三層：Hardware (硬體層 - 專案中將被軟體模擬的部分)
    - PCIe Root Complex (RC) & Bus： CPU 透過這條高速公路跟設備溝通。
    - BARs (Base Address Registers)： 設備上的實體暫存器。CPU 透過讀寫這些暫存器（MMIO）來命令硬體：「開始運算」、「清除錯誤」。
    - Hardware Queues (硬體佇列)： 也就是我們說的 Ring Buffer。CPU 把任務塞進 Queue，硬體自己去 Queue 裡面拿出來算。

## 核心架構與功能 (Core Features & Architecture)

### 基礎模組與可配置實驗模式 (Configurable Experimental Modes)
為了深入探討不同架構的優缺點（Trade-offs），本專案透過 Kernel Module Parameters 或編譯巨集支援多種實作組合，可作為技術面試與架構驗證的實驗室：

- 模組名稱：字元裝置與控制介面
    - 實作內容：實作 `cdev` 註冊，提供 `open`, `release`, `ioctl` 系統呼叫。
    - 面試重點 (Key Topics)：User/Kernel Space 界線、`copy_from_user` 效能開銷與安全性驗證。

- 模組名稱：高效能 Ring Buffer (Command Queue)
    - 實作內容：實作 Circular Command Queue，探討不同鎖定機制對效能的影響。
    - **[Mode A] Global Shared Queue (Multi-Producer):** 所有 Context 共用單一 Ring Buffer，需實作 Spinlock 處理併發寫入競爭。
    - **[Mode B] Private Context Queue (SPSC, Lock-free):** 每個 Context 配置私有 Ring Buffer，實作 Single-Producer Single-Consumer 的無鎖（Lock-free）佇列邏輯。
    - 面試重點 (Key Topics)：鎖的選擇 (Spinlock vs. Mutex)、上下文切換、Memory Barrier、Lock-free 演算法。

- 模組名稱：零拷貝記憶體映射 (Zero-Copy Memory Mapping)
    - 實作內容：實作 `mmap`，讓 User Space 直連 Kernel 分配的緩衝區。
    - **[Mode A] 純軟體模擬分配:** 使用 `alloc_pages` 或 `kmalloc` 配上 `remap_pfn_range`，展示最輕量級的純 Character Device 記憶體映射。
    - **[Mode B] 虛擬硬體 DMA 分配:** 註冊虛擬的 `platform_device`，使用正規 DMA API (`dma_alloc_coherent`) 來模擬具有匯流排存取能力的實體加速器。
    - 面試重點 (Key Topics)：Page Table 運作、DMA API 的匯流排依賴性、Cache Coherency (快取一致性)。

- 模組名稱：虛擬硬體排程與中斷模擬
    - 實作內容：多個 Ring Buffer 的請求仲裁與硬體延遲模擬。
    - **Virtual Hardware Scheduler:** 當處於 Mode B (Private Context Queue) 時，實作 Round-Robin 或 FIFO 排程器，仲裁多個 Context 誰能優先被虛擬硬體 (Workqueue) 執行。
    - 面試重點 (Key Topics)：Top/Bottom Half 中斷處理、Tasklet vs Workqueue、硬體排程器 (Hardware Scheduler) 概念。

### 現代 GPU 驅動架構四大核心設計

- **Multi-Context Isolation (多重執行環境隔離):**
   - 支援多個 User Space Process 併發存取。
   - 每個 `open()` 系統呼叫皆會分配獨立的 Virtual Context ID，隔離各自的資源狀態。
- **Zero-Copy Mapping (零拷貝記憶體映射):**
   - 透過 `mmap` 將 Kernel 空間分配的連續記憶體直接暴露給 User Space。
   - 消除傳統 `read`/`write` 帶來的 Context Switch 與 Data Copy 延遲。
- **Asynchronous Command Submission & Doorbell (非同步指令派發與門鈴機制):**
   - User Space 透過 `ioctl` 或 MMIO 方式寫入 Command 後，觸發 "Doorbell" 通知虛擬硬體。
   - 底層透過 Kernel Scheduler 與 Workqueue 模擬硬體非同步執行，不阻塞 User Space 執行緒。
- **Event-driven Synchronization (事件驅動同步機制):**
   - 實作 Wait Queue 機制。當虛擬硬體處理完畢，透過喚醒機制通知等待中的 User Space 應用程式，模擬硬體中斷 (Hardware Interrupt)。

## 開發里程碑 (Roadmap)
* [ ] **Phase 1: Basic Scaffolding & Char Device** - 完成 LKM 註冊、`open`/`release` 機制與基礎 Context 隔離。
* [ ] **Phase 2: IOCTL Command Submission & Doorbell** - 實作基礎指令協定，透過 `ioctl` (`copy_from_user`) 傳遞任務，完成單一 Ring Buffer (Mode A) 邏輯與 Doorbell 觸發。
* [ ] **Phase 3: Multi-Context Isolation & Scheduling** - 實作私有 Ring Buffer (Mode B) 與 Lock-free SPSC，並導入 Virtual Hardware Scheduler 處理多 Context 排程。
* [ ] **Phase 4: Zero-Copy MMAP & Virtual Bus** - 將記憶體存取升級為 `mmap`。實作純軟體 Memory Mapping (Mode A)，並嘗試註冊虛擬 Platform Device 以支援正規 DMA API (Mode B)。
* [ ] **Phase 5: Hardware Simulation & Benchmarking** - 整合 Workqueue 模擬非同步運算，利用 Wait Queue 模擬中斷喚醒，並撰寫 User Space Benchmark 收集效能數據。