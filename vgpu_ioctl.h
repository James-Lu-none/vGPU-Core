#ifndef _VGPU_IOCTL_H
#define _VGPU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * 定義 IOCTL 的 Magic Number。
 * Kernel 用這個字母來區分是哪個驅動程式的 IOCTL，避免衝突。
 * 我們這裡挑選 'V' 代表 vGPU。
 */
#define VGPU_IOC_MAGIC  'V'

/* 
 * 虛擬 GPU 的運算指令結構 (Command)
 * 這是 User Space 和 Kernel Space 溝通的「封包格式」。
 * 為了跨平台(32/64 bit)相容，我們使用 __u32 等固定大小的型別。
 */
struct vgpu_command {
    __u32 opcode;    // 運算類型 (例如：1=相加, 2=相乘)
    __u32 operand1;  // 操作數 1
    __u32 operand2;  // 操作數 2
    __u32 result;    // 運算結果 (未來虛擬硬體算完後填入)
};


// _IOW, _IOR, _IOWR, _IO 都是用來產生唯一 ioctl 指令碼的巨集
// 這些巨集會把讀寫方向、資料大小、裝置型態和序號打包成一個 32-bit 的整數型態指令
// bit 30-31: dir
// bit 16-29: size
// bit 8-15: type
// bit 0-7: nr (sequence number)
// 所以針對同一個魔術數字 type，最多只能定義 256 個不同的指令序號

/* 
 * 定義 IOCTL 系統呼叫指令：
 * 
 * _IOW(magic, seq, type): 代表 User Space 要「寫入(Write)」資料到 Kernel。
 * 我們用這個指令把一個 vgpu_command 結構送到 Kernel 的 Ring Buffer 中。
 */
#define VGPU_IOC_SUBMIT_CMD _IOW(VGPU_IOC_MAGIC, 1, struct vgpu_command)

/*
 * _IO(magic, seq): 代表沒有資料要傳遞，只是一個「觸發訊號」。
 * 我們用這個指令來敲響門鈴，告訴虛擬硬體「有新任務囉，快去 Ring Buffer 拿！」。
 */
#define VGPU_IOC_DOORBELL   _IO(VGPU_IOC_MAGIC,  2)

#endif /* _VGPU_IOCTL_H */
