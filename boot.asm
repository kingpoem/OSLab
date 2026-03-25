[org 0x7c00]      ; Bootloader is loaded into memory starting at 0x7c00

KERNEL_OFFSET equ 0x1000  ; This is the memory offset into which we will load our kernel.
KERNEL_START_ADDRESS equ 0x9000 ; Stack top for real/protected mode (in 0x0000-0xFFFF).
; We will not rely on kernel size; we'll stop early when disk read fails.
; Note: real-mode stack is at 0x9000, so keep kernel load end below it.
KERNEL_SECTORS equ 63 ; Max sectors to try (kernel ends at <= 0x8FFF).
KERNEL_LBA_START equ 1 ; LBA where kernel.bin begins in the raw image.
CODE_SEG equ 0x08      ; Protected-mode code segment selector (CS).
DATA_SEG equ 0x10      ; Protected-mode data segment selector (DS/SS/ES/FS/GS).

[bits 16]

global start

start:
  ; Start up code: disabling all interrupts, setting register values, setting stack pointer, etc.
  ; After that, enable interrupts, allowing them to occur again, as we'll use INT10h and INT13h
  cli
  mov ax, 0x00
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7c00
  sti

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
  ; Load kernel via LBA using INT 13h AH=42h (Disk Address Packet).
  ; We read one sector at a time and stop on failure, so we don't need
  ; to know kernel.bin size at build time.
  xor di, di                 ; di = number of successfully loaded sectors
  mov si, dap_packet        ; DS:SI -> DAP
  mov cx, KERNEL_SECTORS   ; maximum sectors to try

.read_loop:
  mov dl, [BOOT_DRIVE]      ; drive id saved earlier by BIOS
  mov ah, 0x42              ; extended read
  mov word [dap_sector_count], 1
  int 0x13
  jc .read_failed

  inc di
  add dword [dap_lba_low], 1
  add word [dap_buffer_offset], 0x0200 ; +512 bytes
  loop .read_loop

  ret

.read_failed:
  test di, di
  jz disk_error
  ret

disk_error:
  mov bx, MSG_DISK_ERROR    ; Pointer to the disk-read error message.
  call print_string         ; Print the error message to the screen.
  jmp $                     ; Halt here if disk read fails.

;
; Part 2
;
; Code for setting up the global descriptor table
; Implement your GDT set up code below
;

gdt_start:
gdt_null dq 0x0               ; Null descriptor (required by the x86 GDT format).
gdt_code:                     ; Code segment descriptor.
  dw 0xFFFF                   ; Segment limit (low 16 bits).
  dw 0x0000                   ; Base address (low 16 bits).
  db 0x00                      ; Base address (middle 8 bits).
  db 10011010b                ; Access byte for executable/readable code segment.
  db 11001111b                ; Flags + segment limit (high 4 bits).
  db 0x00                      ; Base address (high 8 bits).
gdt_data:                     ; Data segment descriptor.
  dw 0xFFFF                   ; Segment limit (low 16 bits).
  dw 0x0000                   ; Base address (low 16 bits).
  db 0x00                      ; Base address (middle 8 bits).
  db 10010010b                ; Access byte for writable data segment.
  db 11001111b                ; Flags + segment limit (high 4 bits).
  db 0x00                      ; Base address (high 8 bits).
gdt_end:
gdt_descriptor:
  dw gdt_end - gdt_start - 1 ; GDT limit = size of GDT in bytes - 1.
  dd gdt_start                 ; GDT base linear address.

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
  cli                         ; Disable interrupts during the mode switch.
  lgdt [gdt_descriptor]      ; Load the GDT register with our GDT descriptor.
  mov eax, cr0                ; Read current CR0 control register.
  or eax, 0x00000001         ; Set the PE bit (Protection Enable) in CR0.
  mov cr0, eax                ; Write CR0 back to enable protected mode.
  jmp CODE_SEG:init_pm       ; Far jump to load CS and continue in 32-bit code.

[bits 32]

; Some other function(s) that set the registers and stack pointers
; At the end of the function, call BEGIN_PM
init_pm:
  mov ax, DATA_SEG           ; Load protected-mode data segment selector.
  mov ds, ax                 ; Initialize DS.
  mov ss, ax                 ; Initialize SS.
  mov es, ax                 ; Initialize ES.
  mov fs, ax                 ; Initialize FS.
  mov gs, ax                 ; Initialize GS.
  mov esp, KERNEL_START_ADDRESS ; Set protected-mode stack pointer.
  jmp BEGIN_PM               ; Enter the 32-bit kernel boot entry wrapper.

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

MSG_DISK_ERROR:
  db "disk read error", 0x0

MSG_PMODE:
  db "successfully landed in 32-bit protected mode.", 0x0

; Disk Address Packet for INT 13h AH=42h (extended read).
; DAP layout (16 bytes):
;  +0  size (0x10)
;  +1  reserved
;  +2  sectors to read (word)
;  +4  buffer offset (word)
;  +6  buffer segment (word)
;  +8  starting LBA (qword)
dap_packet:
  db 0x10
  db 0x00
dap_sector_count:
  dw 0x0001
dap_buffer_offset:
  dw KERNEL_OFFSET
dap_buffer_segment:
  dw 0x0000
dap_lba_low:
  dd KERNEL_LBA_START
dap_lba_high:
  dd 0x00000000

; Boot sector padding
times 510-($-$$) db 0x0
dw 0xaa55
