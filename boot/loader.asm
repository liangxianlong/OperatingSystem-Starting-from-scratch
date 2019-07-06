; ------------------------------------------------------------------------
; boot/loader.asm
; ------------------------------------------------------------------------
org 0100h
	jmp LABEL_START 	; start

; 下面是 FAT12 磁盘头，之所以包含这个是因为它下面用到了磁盘的一些信息
%include "fat12hdr.inc"
%include "load.inc"
%include "pm.inc"

; ------------------------------------------------------------------------
; GDT
; ------------------------------------------------------------------------
LABEL_GDT:	Descriptor	0,	0,	0	; 空描述符
LABEL_DESC_FLAT_C:	Descriptor	0, 0fffffh,	DA_CR | DA_32 | DA_LIMIT_4K	; 0 ~ 4G
LABEL_DESC_FLAT_RW:	Descriptor	0, 0fffffh, DA_DRW | DA_32 | DA_LIMIT_4K	; 0 ~ 4G
LABEL_DESC_VIDEO: 	Descriptor	0B8000h, 0ffffh, DA_DRW | DA_DPL3	; 显存首地址

; ------------------------------------------------------------------------


GdtLen equ $ - LABEL_GDT
GdtPtr dw GdtLen - 1		; 段界限
dd LOADER_PHY_ADDR + LABEL_GDT	; 基地址（此处可优化，将基地址8字节对齐将起到速度优化效果）
; The GDT isn't a segment itself; instead, it is a data structure in linear address space.
; The base linear address and limit of the GDT must be loaded into the GDTR register. -- IA-32 Software Developer's Manual, Vol.3A


; GDT 选择子--------------------------------------------------------------
SelectorFlatC equ LABEL_DESC_FLAT_C - LABEL_GDT
SelectorFlatRW equ LABEL_DESC_FLAT_RW - LABEL_GDT
SelectorVideo equ LABEL_DESC_VIDEO - LABEL_GDT + SA_RPL3
; ------------------------------------------------------------------------


BaseOfStack equ 0100h

LABEL_START:			; start loading
	mov ax, cs
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, BaseOfStack

	mov dh, 0		; "Loading  "
	call DispStrRealMode		; 显示字符串

	; 得到内存数
	mov ebx, 0		; ebx = 后续值，开始时需要为0
	mov di, _MemChkBuf	; es:di 指向一个地址范围描述符结构（Address Rang Descriptor Structure）

.MemChkLoop:
	mov eax, 0E820h		; eax = 0000E820h
	mov ecx, 20			; ecx = 地址范围描述符结构的大小
	mov edx, 0534D4150h	; edx = 'SMAP'
	int 15h				; int 15h
	jc .MemChkFail
	add di, 20
	inc dword [_dwMCRNumber]	; dwMCRNumber = ARDS 的个数
	cmp ebx, 0
	jne .MemChkLoop
	jmp .MemChkOK
.MemChkFail:
	mov dword [_dwMCRNumber], 0
.MemChkOK:
	; 下面在 A盘根目录找 KERNEL.BIN
	mov word [wSectorNo], SectorNoOfRootDirectory
	xor ah, ah			; ┓
	xor dl, dl 			; ┣ 软驱复位
	int 13h				; ┛

LABEL_SEARCH_IN_ROOT_DIR_BEGIN:
	cmp word [wRootDirSizeForLoop], 0	; ┓
	jz LABEL_NO_KERNEL_BIN				; ┣ 判断根目录是否已经读完，如果读完，表示没有找到
	dec word [wRootDirSizeForLoop]		; ┛
	mov ax, KERNEL_FILE_SEG
	mov es, ax			; es <- KERNEL_FILE_SEG
	mov bx, KERNEL_FILE_OFF		; bx <- KERNEL_FILE_OFF 于是，es:bx = KERBEL_FILE_SEG:KERNEL_FILE_OFF = KERNEL_FILE_SEG * 10h + KERNEL_FILE_OFF
	mov ax, [wSectorNo]			; ax <- Root Director 中的某个 Sector 号
	mov cl, 1
	call ReadSector

	mov si, KernelFileName		; ds:si -> "KERNEL  BIN"
	mov di, KERNEL_FILE_OFF		; es:di -> KERNEL_FILE_SEG:???? = KERNEL_FILE_SEG * 10h + ????
	cld
	mov dx, 10h

LABEL_SEARCH_FOR_KERNELBIN:
	cmp dx, 0								; ┓
	jz LABEL_GOTO_NEXT_SECTOR_IN_ROOT_DIR	; ┣ 循环次数控制，如果已经读完一个 Sector, 那么跳到下一个 Sector
	dec dx									; ┛
	mov cx, 11

LABEL_CMP_FILENAME:
	cmp cx, 0				; ┓
	jz LABEL_FILENAME_FOUND	; ┣ 循环次数控制，如果比较了11个字符全都相等。
	dec cx					; ┛
	lodsb			; ds:si -> al
	cmp al, byte [es:di]	; if al == es:di
	jz LABEL_GO_ON
	jmp LABEL_DIFFERENT

LABEL_GO_ON:
	inc di
	jmp LABEL_CMP_FILENAME	; 继续循环

LABEL_DIFFERENT:
	and di, 0FFE0h					; else┓ 这时 di 的值不知道是什么，di &= e0 为了让它是 20h 的整数倍
	add di, 20h						;     ┃
	mov si, KernelFileName			;     ┣ di += 20h 下一个目录条目
	jmp LABEL_SEARCH_FOR_KERNELBIN	; 	  ┛

LABEL_GOTO_NEXT_SECTOR_IN_ROOT_DIR:
	add word [wSectorNo], 1
	jmp LABEL_SEARCH_IN_ROOT_DIR_BEGIN

LABEL_NO_KERNELBIN:
	mov dh, 3				; "No KERNEL."
	call DispStrRealMode	; 显示字符串
	jmp $					; 没有找到 KERNEL.BIN, 进入死循环
