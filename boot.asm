[org 0x7c00]      ; Bootloader is loaded into memory starting at 0x7c00

KERNEL_OFFSET equ 0x1000  ; This is the memory offset into which we will load our kernel.
KERNEL_START_ADDRESS equ 0x9000 ; This is the location to which the kernel will be loaded.
CODE_SEG equ 0x08  ; GDT selector for the code segment (descriptor index 1); used in the far jump into 32-bit mode.
DATA_SEG equ 0x10  ; GDT selector for the data segment (descriptor index 2); loaded into DS, SS, ES, FS, and GS.

[bits 16]

global start

start:
  ; Start up code: disabling all interrupts, setting register values, setting stack pointer, etc.
  ; After that, enable interrupts, allowing them to occur again, as we'll use INT10h and INT13h
  cli                           ; Disable interrupts
  mov ax, 0x00                  ; Zero for segment registers in real mode.
  mov ds, ax                    ; DS = 0 (same segment as the boot code).
  mov es, ax                    ; ES = 0 for BIOS and string ops.
  mov ss, ax                    ; SS = 0; stack shares CS's segment.
  mov sp, 0x7c00                ; Stack below the boot sector (grows down).
  sti                           ; Re-enable interrupts

  ; BIOS stores index of boot drive it discovered in dl. Hang on to this as we will need to read
  ; from this drive later. Note that this variable is allocated in the globals section below.
  mov [BOOT_DRIVE], dl

  mov bp, KERNEL_START_ADDRESS  ; Set up the stack to load the kernel
  mov sp, bp

  mov bx, MSG_REAL_MODE         ; Print message indicating we are in real mode
  call print_string             ; This helper function is located in the print_string.asm file

  call load_kernel_from_disk    ; Uses BIOS interrupt to read kernel from disk into memory

  call switch_to_pm             ; Switch to protected mode, from which control will not return.
                                ; After the switch, enter 32-bit code at the offset BEGIN_PM.

  jmp $


;
; Part 1
;
; Code for loading kernel binary from disk into memory.
; Implement your load_kernel_from_disk code below, if disk reading fails, 
; your function should give an error message.
; Note that you must read the correct number of sectors.
;

[bits 16]

load_kernel_from_disk:

  mov bx, MSG_LOAD_KERNEL ; Print message for kernel load.
  call print_string

  mov dl, [BOOT_DRIVE]          ; Drive number (same one BIOS booted from).
  mov ah, 0x02                  ; INT 13h: read sectors into RAM.
  mov al, 25                    ; Sector count - must be at least the kernel size.
  mov ch, 0x00                  ; Cylinder 0.
  mov cl, 0x02                  ; Start at sector 2 (sector 1 is this boot sector).
  mov dh, 0x00                  ; Head 0.
  mov bx, KERNEL_OFFSET         ; ES:BX buffer (ES still 0) -> linear 0x1000.
  int 0x13
  jc disk_error                 ; CF set if the BIOS reports a read error.

  ret

disk_error:
  mov bx, MSG_DISK_ERROR        ; Report failure, then stop.
  call print_string
  jmp $                         ; Idle forever.

MSG_DISK_ERROR:
  db "disk read error: read failed", 0x0  ; NUL-terminated string for print_string.


;
; Part 2
;
; Code for setting up the global descriptor table
; Implement your GDT set up code below
;

gdt_start:
gdt_null:
  dq 0x0000000000000000        ; Unused null descriptor (selector 0); must be first.

gdt_code:
  dw 0xFFFF                     ; Segment limit bits 15:0 (low word).
  dw 0x0000                     ; Segment base address bits 15:0 (low word).
  db 0x00                       ; Segment base bits 23:16.
  db 10011010b                  ; Access: present, ring 0, executable code, readable.
  db 11001111b                  ; Flags + limit bits 19:16: 4 KiB granularity, 32-bit ops, full 4 GiB span.
  db 0x00                       ; Segment base bits 31:24.

gdt_data:
  dw 0xFFFF                     ; Segment limit bits 15:0 (low word).
  dw 0x0000                     ; Segment base address bits 15:0 (low word).
  db 0x00                       ; Segment base bits 23:16.
  db 10010010b                  ; Access: present, ring 0, read/write data segment.
  db 11001111b                  ; Same limit/granularity as code: 4 KiB granularity, 32-bit, 4 GiB.
  db 0x00                       ; Segment base bits 31:24.

gdt_end:

gdt_descriptor:
  dw gdt_end - gdt_start - 1    ; GDT byte size minus one (required by LGDT).
  dd gdt_start                  ; Base address of the GDT (LGDT loads limit word then this dword).

;
; Part 3
;
; Code for switching from 16-bit real mode to 32-bit protected mode.
; Implement your switch_to_pm function below.
; Feel to add other function(s) as needed, because you do need another funtion running
; in 32-bit mode, inside which BEGIN_PM must be called.
;

[bits 16]

switch_to_pm:
  cli                           ; Ignore maskable IRQs across the mode transition.
  lgdt [gdt_descriptor]         ; Load GDT limit and base into GDTR.
  mov eax, cr0                  ; Read control register 0.
  or eax, 0x1                   ; Set the PE (protection enable) bit.
  mov cr0, eax                  ; Enter protected mode (segmentation now uses the GDT).
  jmp CODE_SEG:init_pm          ; Far jump flushes prefetch; CPU now fetches 32-bit code using CS.

[bits 32]                       ; Following instructions use 32-bit encoding and operand size.

; Some other function(s) that set the registers and stack pointers
; At the end of the function, call BEGIN_PM
init_pm:
  mov ax, DATA_SEG              ; Load flat data selector (RPL 0) into a general-purpose register.
  mov ds, ax                    ; Data segment for normal memory accesses.
  mov ss, ax                    ; Stack segment (same base/limit as data in this flat setup).
  mov es, ax                    ; Extra segment same as DS for string/MOVSD-style ops.
  mov fs, ax                    ; FS uses the same flat data descriptor as DS.
  mov gs, ax                    ; GS uses the same flat data descriptor as DS.
  mov esp, KERNEL_START_ADDRESS ; Stack grows down from high address used for early kernel setup.
  jmp BEGIN_PM                  ; Enter the main 32-bit entry point.

;
; Finally... This is the entry point for 32-bit code and we'll not return from it.
;

[bits 32]

BEGIN_PM:
  mov ebx, MSG_PMODE      ; Print message indicating we are in real mode.
  call print_string_pm
  call KERNEL_OFFSET      ; Begin executing the kernel.
  jmp $                   ; If return ever controls from the kernel, hang.


;
; Global variables and includes. No need to edit.
;
%include "print_string.asm"

BOOT_DRIVE db 0x0

MSG_REAL_MODE:
  db "started in 16-bit real mode", 0xa, 0xd, 0x0

MSG_LOAD_KERNEL:
  db "loading kernel into memory...", 0x0

MSG_PMODE:
  db "successfully landed in 32-bit protected mode.", 0x0

; Boot sector padding
times 510-($-$$) db 0x0
dw 0xaa55
